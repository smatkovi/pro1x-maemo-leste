/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * snd_pro1x: the Pro1X's sound as a sound(4) device (/dev/dsp, /dev/mixer;
 * freebsd/audio/README.md, "sound(4)").
 *
 * The hardware behind it is the ADSP: it plays from (and records into)
 * memory of the application processor in blocks, by physical address,
 * when it is handed them in ASM data commands over APR. Those, like every
 * other packet to the ADSP (the AFE's ports and clocks, the ADM's routing,
 * the ASM session, voice) and the codec's register sequences, are
 * pro1x-audiod's, in user space (/dev/apr, /dev/qcom_lpass). This driver
 * is what sound(4) needs underneath: one playback and one recording
 * channel whose hardware buffers the ADSP can reach - physically
 * contiguous, below 4 GiB (the ADSP takes the SMMU stream in the
 * address's upper bits, as Linux's q6asm-dai sends it), uncached (the
 * ADSP is not coherent with the CPU's caches; sound(4)'s mmap maps the
 * channel's soft buffer, never this one) - and /dev/pro1x_pcm, through
 * which the daemon learns when a channel starts and stops and tells the
 * driver which blocks the ADSP is done with (pro1x_pcm.h). Without the
 * daemon a clock stands in for the ADSP, so that no writer hangs.
 *
 * The device tree's sound card (compatible "fxtec,pro1x-sndcard") is
 * the device; the SMMU stream of Linux's q6asm-dais node (0x1c1) is
 * switched to translation off at attach (qcom_smmu_bypass, freebsd/dsp),
 * as FastRPC's are. With hw.snd_pro1x.nexus=1 the driver also attaches
 * without the device tree (built with SND_PRO1X_VM: no SMMU, for the
 * FreeBSD VM's tests).
 *
 * Formats: 48 kHz, 16 bit, stereo playback, mono or stereo recording
 * (sound(4) converts everything else); blocks are multiples of 480
 * frames (Linux's q6asm-dai constraint), playback 40-100 ms, recording
 * 3840 bytes (Linux's capture periods are at most 4 KiB). Mixer: PCM in
 * software (SD_F_SOFTPCMVOL); the master volume, loudspeaker, earpiece,
 * microphones and FM levels and the recording source are passed on to
 * the daemon, which owns the codec.
 *
 *   sysctl dev.pcm.N.pro1x    the channels' state, counters
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/devctl.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/poll.h>
#include <sys/sbuf.h>
#include <sys/selinfo.h>
#include <sys/sysctl.h>
#include <sys/uio.h>

#include <machine/bus.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#ifdef HAVE_KERNEL_OPTION_HEADERS
#include "opt_snd.h"
#endif

#include <dev/sound/pcm/sound.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "mixer_if.h"

#include "pro1x_pcm.h"
#ifndef SND_PRO1X_VM
#include "qcom_dsp.h"
#endif

#define	P1_RATE		48000
#define	P1_QLEN		64
#define	P1_PLAY_MINSTEPS 4	/* 40 ms */
#define	P1_PLAY_MAXSTEPS 10	/* 100 ms */
#define	P1_PLAY_MAXBLKS	8
#define	P1_REC_BLKSZ	3840	/* <= 4096, a multiple of 480 frames */
#define	P1_REC_BLKS	8
/* Linux's q6asm-dai: the stream id of the iommus in the address */
#define	P1_SID_MASK	0xf

static int p1_nexus;
SYSCTL_NODE(_hw, OID_AUTO, snd_pro1x, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "The Pro1X's sound(4) device (freebsd/audio)");
SYSCTL_INT(_hw_snd_pro1x, OID_AUTO, nexus, CTLFLAG_RDTUN, &p1_nexus, 0,
    "Attach without the device tree (tests)");

struct p1_softc;

struct p1_chan {
	struct p1_softc	*sc;
	struct pcm_channel *pc;
	struct snd_dbuf	*b;
	struct pcmchan_caps *caps;
	int		dir;		/* PCMDIR_PLAY, PCMDIR_REC */
	int		idx;		/* PRO1X_PCM_PLAY, PRO1X_PCM_REC */
	uint32_t	fmt;
	uint32_t	blksz, blkcnt;	/* as of the start */
	bool		run;
	bool		by_daemon;	/* else the clock */
	uint32_t	gen;
	uint32_t	hw;		/* blocks done since the start */
	struct callout	clock;
	sbintime_t	period;
	/* counters */
	u_int		starts, daemon_blocks, clock_blocks, stale, fails;
};

struct p1_softc {
	struct snddev_info info;	/* first: sound(4)'s softc */
	device_t	dev;
	struct mtx	lock;
	struct p1_chan	ch[PRO1X_PCM_NCHAN];
	uint32_t	play_fmts[2], rec_fmts[3];
	struct pcmchan_caps pcaps, rcaps;
	bus_dma_tag_t	dtag;
	uint64_t	sidbits;	/* sid << 32, or 0 */
	struct cdev	*ctl;
	bool		ctl_open;
	struct selinfo	sel;
	struct pro1x_pcm_event q[P1_QLEN];
	u_int		qhead, qlen, qdropped;
	u_int		mix[SOUND_MIXER_NRDEVICES];
	uint32_t	recsrc;
	bool		stopped;
	uint32_t	jack;		/* the daemon's last plug */
	char		jack_event[60];
};

static MALLOC_DEFINE(M_P1, "snd_pro1x", "snd_pro1x");

/* --- events for the daemon -------------------------------------------- */

static void
p1_post(struct p1_softc *sc, const struct pro1x_pcm_event *ev)
{
	mtx_assert(&sc->lock, MA_OWNED);
	if (!sc->ctl_open)
		return;
	if (sc->qlen == P1_QLEN) {
		/* the daemon learns of a missed START/STOP from ADVANCE */
		sc->qdropped++;
		return;
	}
	sc->q[(sc->qhead + sc->qlen) % P1_QLEN] = *ev;
	sc->qlen++;
	wakeup(&sc->q);
	selwakeup(&sc->sel);
}

static uint32_t
p1_framesz(uint32_t fmt)
{
	return (AFMT_CHANNEL(fmt) * 2);
}

static void
p1_chan_event(struct p1_chan *ch, uint32_t type, struct pro1x_pcm_event *ev)
{
	memset(ev, 0, sizeof(*ev));
	ev->type = type;
	ev->chan = ch->idx;
	ev->gen = ch->gen;
	ev->rate = P1_RATE;
	ev->channels = AFMT_CHANNEL(ch->fmt);
	ev->bits = 16;
	ev->blksz = ch->blksz;
	ev->blkcnt = ch->blkcnt;
	ev->bufsize = PRO1X_PCM_BUFMAX;
	ev->paddr = ch->b->buf_addr;
	ev->dspaddr = ch->b->buf_addr | ch->sc->sidbits;
}

/* blocks the daemon may hand to the ADSP from block hw on: CHN_LOCK held */
static uint32_t
p1_ready(struct p1_chan *ch)
{
	if (ch->blksz == 0)
		return (0);
	if (ch->dir == PCMDIR_REC)
		return (ch->blkcnt);
	return (MIN(ch->b->rl / ch->blksz, ch->blkcnt));
}

/* --- the clock without the daemon ------------------------------------- */

static void
p1_clock(void *arg)
{
	struct p1_chan *ch = arg;
	struct p1_softc *sc = ch->sc;

	mtx_assert(&sc->lock, MA_OWNED);
	if (!ch->run || ch->by_daemon || sc->stopped)
		return;
	if (ch->dir == PCMDIR_REC)
		memset(ch->b->buf + (ch->hw % ch->blkcnt) * ch->blksz, 0,
		    ch->blksz);
	ch->hw++;
	ch->clock_blocks++;
	mtx_unlock(&sc->lock);
	chn_intr(ch->pc);
	mtx_lock(&sc->lock);
	if (ch->run && !ch->by_daemon && !sc->stopped)
		callout_reset_sbt(&ch->clock, ch->period, 0, p1_clock, ch, 0);
}

static void
p1_clock_start(struct p1_chan *ch)
{
	mtx_assert(&ch->sc->lock, MA_OWNED);
	ch->period = SBT_1S * ch->blksz / (P1_RATE * p1_framesz(ch->fmt));
	callout_reset_sbt(&ch->clock, ch->period, 0, p1_clock, ch, 0);
}

/* --- sound(4) channels ------------------------------------------------- */

static void *
p1_chan_init(kobj_t obj, void *devinfo, struct snd_dbuf *b,
    struct pcm_channel *c, int dir)
{
	struct p1_softc *sc = devinfo;
	struct p1_chan *ch;
	int idx = dir == PCMDIR_PLAY ? PRO1X_PCM_PLAY : PRO1X_PCM_REC;

	if (sndbuf_alloc(b, sc->dtag, 0, PRO1X_PCM_BUFMAX) != 0) {
		device_printf(sc->dev, "%s buffer: no %u bytes below 4 GiB\n",
		    idx ? "recording" : "playback", PRO1X_PCM_BUFMAX);
		return (NULL);
	}
	if (b->buf_addr + PRO1X_PCM_BUFMAX > (1ULL << 32)) {
		/* the tag says otherwise: the ADSP's stream bits would clash */
		device_printf(sc->dev, "buffer at %#jx: above 4 GiB\n",
		    (uintmax_t)b->buf_addr);
		return (NULL);
	}
	ch = &sc->ch[idx];
	ch->sc = sc;
	ch->dir = dir;
	ch->idx = idx;
	ch->b = b;
	ch->caps = idx == PRO1X_PCM_PLAY ? &sc->pcaps : &sc->rcaps;
	ch->fmt = ch->caps->fmtlist[0];
	memset(b->buf, 0, PRO1X_PCM_BUFMAX);
	callout_init_mtx(&ch->clock, &sc->lock, 0);
	ch->pc = c;
	if (bootverbose)
		device_printf(sc->dev, "%s buffer at %#jx (ADSP %#jx)\n",
		    idx ? "recording" : "playback", (uintmax_t)b->buf_addr,
		    (uintmax_t)(b->buf_addr | sc->sidbits));
	return (ch);
}

static int
p1_chan_setformat(kobj_t obj, void *data, uint32_t format)
{
	struct p1_chan *ch = data;
	int i;

	for (i = 0; ch->caps->fmtlist[i] != 0; i++)
		if (format == ch->caps->fmtlist[i]) {
			ch->fmt = format;
			return (0);
		}
	return (EINVAL);
}

static uint32_t
p1_chan_setspeed(kobj_t obj, void *data, uint32_t speed)
{
	return (P1_RATE);
}

static uint32_t
p1_chan_setblocksize(kobj_t obj, void *data, uint32_t blocksize)
{
	struct p1_chan *ch = data;
	uint32_t step = PRO1X_PCM_FRAMES_STEP * p1_framesz(ch->fmt);
	uint32_t blksz, blkcnt;

	if (ch->dir == PCMDIR_REC) {
		blksz = P1_REC_BLKSZ - P1_REC_BLKSZ % step;
		blkcnt = P1_REC_BLKS;
	} else {
		blksz = howmany(blocksize, step) * step;
		blksz = MAX(blksz, P1_PLAY_MINSTEPS * step);
		blksz = MIN(blksz, P1_PLAY_MAXSTEPS * step);
		blkcnt = MIN(P1_PLAY_MAXBLKS, PRO1X_PCM_BUFMAX / blksz);
	}
	if (sndbuf_resize(ch->b, blkcnt, blksz) != 0)
		device_printf(ch->sc->dev, "%s: %u blocks of %u bytes refused\n",
		    ch->idx ? "recording" : "playback", blkcnt, blksz);
	return (ch->b->blksz);
}

static int
p1_chan_trigger(kobj_t obj, void *data, int go)
{
	struct p1_chan *ch = data;
	struct p1_softc *sc = ch->sc;
	struct pro1x_pcm_event ev;

	if (!PCMTRIG_COMMON(go))
		return (0);
	mtx_lock(&sc->lock);
	if (sc->stopped) {
		mtx_unlock(&sc->lock);
		return (0);
	}
	switch (go) {
	case PCMTRIG_START:
		/* sound(4) has filled bufhard (silence) up to its ready */
		ch->blksz = ch->b->blksz;
		ch->blkcnt = ch->b->blkcnt;
		ch->run = true;
		ch->gen++;
		ch->hw = 0;
		ch->starts++;
		if (sc->ctl_open) {
			ch->by_daemon = true;
			p1_chan_event(ch, PRO1X_PCM_EV_START, &ev);
			ev.ready = p1_ready(ch);
			p1_post(sc, &ev);
		} else {
			ch->by_daemon = false;
			p1_clock_start(ch);
		}
		break;
	case PCMTRIG_STOP:
	case PCMTRIG_ABORT:
		if (!ch->run)
			break;
		ch->run = false;
		if (ch->by_daemon) {
			p1_chan_event(ch, PRO1X_PCM_EV_STOP, &ev);
			p1_post(sc, &ev);
		} else
			callout_stop(&ch->clock);
		break;
	}
	mtx_unlock(&sc->lock);
	return (0);
}

static uint32_t
p1_chan_getptr(kobj_t obj, void *data)
{
	struct p1_chan *ch = data;
	uint32_t ptr = 0;

	mtx_lock(&ch->sc->lock);
	if (ch->run && ch->blkcnt != 0)
		ptr = (ch->hw % ch->blkcnt) * ch->blksz;
	mtx_unlock(&ch->sc->lock);
	return (ptr);
}

static struct pcmchan_caps *
p1_chan_getcaps(kobj_t obj, void *data)
{
	return (((struct p1_chan *)data)->caps);
}

static kobj_method_t p1_chan_methods[] = {
	KOBJMETHOD(channel_init,	p1_chan_init),
	KOBJMETHOD(channel_setformat,	p1_chan_setformat),
	KOBJMETHOD(channel_setspeed,	p1_chan_setspeed),
	KOBJMETHOD(channel_setblocksize, p1_chan_setblocksize),
	KOBJMETHOD(channel_trigger,	p1_chan_trigger),
	KOBJMETHOD(channel_getptr,	p1_chan_getptr),
	KOBJMETHOD(channel_getcaps,	p1_chan_getcaps),
	KOBJMETHOD_END
};
CHANNEL_DECLARE(p1_chan);

/* --- the mixer: passed on to the daemon --------------------------------- */

#define	P1_MIX_DEVS	(SOUND_MASK_VOLUME | SOUND_MASK_PCM | \
	SOUND_MASK_SPEAKER | SOUND_MASK_PHONEOUT | SOUND_MASK_MIC | \
	SOUND_MASK_LINE | SOUND_MASK_RADIO)
#define	P1_REC_DEVS	(SOUND_MASK_MIC | SOUND_MASK_LINE)

static int
p1_mixer_init(struct snd_mixer *m)
{
	struct p1_softc *sc = mix_getdevinfo(m);

	pcm_setflags(sc->dev, pcm_getflags(sc->dev) | SD_F_SOFTPCMVOL);
	mix_setdevs(m, P1_MIX_DEVS);
	mix_setrecdevs(m, P1_REC_DEVS);
	return (0);
}

static int
p1_mixer_set(struct snd_mixer *m, unsigned dev, unsigned left, unsigned right)
{
	struct p1_softc *sc = mix_getdevinfo(m);
	struct pro1x_pcm_event ev;

	if (dev >= SOUND_MIXER_NRDEVICES)
		return (-1);
	mtx_lock(&sc->lock);
	sc->mix[dev] = left | right << 8;
	memset(&ev, 0, sizeof(ev));
	ev.type = PRO1X_PCM_EV_MIXER;
	ev.dev = dev;
	ev.left = left;
	ev.right = right;
	p1_post(sc, &ev);
	mtx_unlock(&sc->lock);
	return (left | right << 8);
}

static uint32_t
p1_mixer_setrecsrc(struct snd_mixer *m, uint32_t src)
{
	struct p1_softc *sc = mix_getdevinfo(m);
	struct pro1x_pcm_event ev;

	/* one source at a time: the built-in microphone first */
	src = (src & SOUND_MASK_MIC) ? SOUND_MASK_MIC :
	    (src & SOUND_MASK_LINE) ? SOUND_MASK_LINE : SOUND_MASK_MIC;
	mtx_lock(&sc->lock);
	sc->recsrc = src;
	memset(&ev, 0, sizeof(ev));
	ev.type = PRO1X_PCM_EV_RECSRC;
	ev.recsrc = src;
	p1_post(sc, &ev);
	mtx_unlock(&sc->lock);
	return (src);
}

static kobj_method_t p1_mixer_methods[] = {
	KOBJMETHOD(mixer_init,		p1_mixer_init),
	KOBJMETHOD(mixer_set,		p1_mixer_set),
	KOBJMETHOD(mixer_setrecsrc,	p1_mixer_setrecsrc),
	KOBJMETHOD_END
};
MIXER_DECLARE(p1_mixer);

/* --- /dev/pro1x_pcm: the daemon's side --------------------------------- */

static d_open_t p1_ctl_open;
static d_close_t p1_ctl_close;
static d_read_t p1_ctl_read;
static d_ioctl_t p1_ctl_ioctl;
static d_poll_t p1_ctl_poll;
static d_mmap_t p1_ctl_mmap;

static struct cdevsw p1_ctl_cdevsw = {
	.d_version =	D_VERSION,
	.d_name =	"pro1x_pcm",
	.d_open =	p1_ctl_open,
	.d_close =	p1_ctl_close,
	.d_read =	p1_ctl_read,
	.d_ioctl =	p1_ctl_ioctl,
	.d_poll =	p1_ctl_poll,
	.d_mmap =	p1_ctl_mmap,
};

static int
p1_ctl_open(struct cdev *cdev, int oflags, int devtype, struct thread *td)
{
	struct p1_softc *sc = cdev->si_drv1;
	struct pro1x_pcm_event ev;
	struct p1_chan *ch;
	u_int i;

	mtx_lock(&sc->lock);
	if (sc->ctl_open) {
		mtx_unlock(&sc->lock);
		return (EBUSY);
	}
	sc->ctl_open = true;
	sc->qhead = sc->qlen = 0;
	for (i = 0; i < PRO1X_PCM_NCHAN; i++) {
		ch = &sc->ch[i];
		if (ch->pc == NULL)
			continue;
		p1_chan_event(ch, PRO1X_PCM_EV_HELLO, &ev);
		p1_post(sc, &ev);
	}
	for (i = 0; i < SOUND_MIXER_NRDEVICES; i++) {
		if ((P1_MIX_DEVS & (1 << i)) == 0 || i == SOUND_MIXER_PCM)
			continue;
		memset(&ev, 0, sizeof(ev));
		ev.type = PRO1X_PCM_EV_MIXER;
		ev.dev = i;
		ev.left = sc->mix[i] & 0x7f;
		ev.right = (sc->mix[i] >> 8) & 0x7f;
		p1_post(sc, &ev);
	}
	memset(&ev, 0, sizeof(ev));
	ev.type = PRO1X_PCM_EV_RECSRC;
	ev.recsrc = sc->recsrc ? sc->recsrc : SOUND_MASK_MIC;
	p1_post(sc, &ev);
	mtx_unlock(&sc->lock);

	/* running channels go over to the daemon, as a new start */
	for (i = 0; i < PRO1X_PCM_NCHAN; i++) {
		ch = &sc->ch[i];
		if (ch->pc == NULL)
			continue;
		CHN_LOCK(ch->pc);
		mtx_lock(&sc->lock);
		if (ch->run && !ch->by_daemon) {
			callout_stop(&ch->clock);
			ch->by_daemon = true;
			ch->gen++;
			p1_chan_event(ch, PRO1X_PCM_EV_START, &ev);
			ev.ready = p1_ready(ch);
			p1_post(sc, &ev);
		}
		mtx_unlock(&sc->lock);
		CHN_UNLOCK(ch->pc);
	}
	return (0);
}

static int
p1_ctl_close(struct cdev *cdev, int fflag, int devtype, struct thread *td)
{
	struct p1_softc *sc = cdev->si_drv1;
	struct p1_chan *ch;
	u_int i;

	mtx_lock(&sc->lock);
	sc->ctl_open = false;
	sc->qlen = 0;
	/* what the daemon drove, the clock drives on */
	for (i = 0; i < PRO1X_PCM_NCHAN; i++) {
		ch = &sc->ch[i];
		if (ch->run && ch->by_daemon) {
			ch->by_daemon = false;
			p1_clock_start(ch);
		}
	}
	wakeup(&sc->q);
	selwakeup(&sc->sel);
	mtx_unlock(&sc->lock);
	return (0);
}

static int
p1_ctl_read(struct cdev *cdev, struct uio *uio, int ioflag)
{
	struct p1_softc *sc = cdev->si_drv1;
	struct pro1x_pcm_event ev;
	int error = 0;

	if (uio->uio_resid < sizeof(ev))
		return (EINVAL);
	mtx_lock(&sc->lock);
	while (sc->qlen == 0) {
		if (ioflag & O_NONBLOCK) {
			mtx_unlock(&sc->lock);
			return (EWOULDBLOCK);
		}
		error = msleep(&sc->q, &sc->lock, PCATCH, "p1pcm", 0);
		if (error != 0 || !sc->ctl_open) {
			mtx_unlock(&sc->lock);
			return (error != 0 ? error : ENXIO);
		}
	}
	while (sc->qlen > 0 && uio->uio_resid >= sizeof(ev)) {
		ev = sc->q[sc->qhead];
		sc->qhead = (sc->qhead + 1) % P1_QLEN;
		sc->qlen--;
		mtx_unlock(&sc->lock);
		error = uiomove(&ev, sizeof(ev), uio);
		mtx_lock(&sc->lock);
		if (error != 0)
			break;
	}
	mtx_unlock(&sc->lock);
	return (error);
}

static void
p1_answer(struct p1_chan *ch, struct pro1x_pcm_advance *a)
{
	CHN_LOCK(ch->pc);
	mtx_lock(&ch->sc->lock);
	a->running = ch->run && ch->by_daemon && ch->gen == a->gen;
	a->hw = ch->hw;
	a->ready = a->running ? p1_ready(ch) : 0;
	mtx_unlock(&ch->sc->lock);
	CHN_UNLOCK(ch->pc);
}

static int
p1_ctl_ioctl(struct cdev *cdev, u_long cmd, caddr_t data, int fflag,
    struct thread *td)
{
	struct p1_softc *sc = cdev->si_drv1;
	struct pro1x_pcm_advance *a = (struct pro1x_pcm_advance *)data;
	struct pro1x_pcm_status *s = (struct pro1x_pcm_status *)data;
	struct pro1x_pcm_jack *j = (struct pro1x_pcm_jack *)data;
	char ev[80];
	struct p1_chan *ch;
	uint32_t i, n;
	bool ok;

	switch (cmd) {
	case PRO1X_PCM_ADVANCE:
		if (a->chan >= PRO1X_PCM_NCHAN || sc->ch[a->chan].pc == NULL)
			return (EINVAL);
		ch = &sc->ch[a->chan];
		/* one interrupt per block: sound(4)'s delta is mod bufsize */
		n = MIN(a->blocks, ch->blkcnt);
		for (i = 0; i < n; i++) {
			mtx_lock(&sc->lock);
			ok = ch->run && ch->by_daemon && ch->gen == a->gen;
			if (ok) {
				ch->hw++;
				ch->daemon_blocks++;
			} else
				ch->stale++;
			mtx_unlock(&sc->lock);
			if (!ok)
				break;
			chn_intr(ch->pc);
		}
		p1_answer(ch, a);
		return (0);
	case PRO1X_PCM_FAIL:
		if (a->chan >= PRO1X_PCM_NCHAN || sc->ch[a->chan].pc == NULL)
			return (EINVAL);
		ch = &sc->ch[a->chan];
		mtx_lock(&sc->lock);
		if (ch->run && ch->by_daemon && ch->gen == a->gen) {
			ch->by_daemon = false;
			ch->fails++;
			p1_clock_start(ch);
			device_printf(sc->dev, "%s: the ADSP path failed (%d): "
			    "discarding at the sample rate\n",
			    ch->idx ? "recording" : "playback", a->error);
		}
		mtx_unlock(&sc->lock);
		p1_answer(ch, a);
		return (0);
	case PRO1X_PCM_STATUS:
		if (s->chan >= PRO1X_PCM_NCHAN || sc->ch[s->chan].pc == NULL)
			return (EINVAL);
		ch = &sc->ch[s->chan];
		CHN_LOCK(ch->pc);
		mtx_lock(&sc->lock);
		s->running = ch->run;
		s->gen = ch->gen;
		s->hw = ch->hw;
		s->ready = ch->run ? p1_ready(ch) : 0;
		s->blksz = ch->blksz;
		s->blkcnt = ch->blkcnt;
		s->by_daemon = ch->by_daemon;
		s->rate = P1_RATE;
		s->channels = AFMT_CHANNEL(ch->fmt);
		s->bits = 16;
		s->xruns = ch->pc->xruns;
		s->paddr = ch->b->buf_addr;
		s->dspaddr = ch->b->buf_addr | sc->sidbits;
		mtx_unlock(&sc->lock);
		CHN_UNLOCK(ch->pc);
		return (0);
	case PRO1X_PCM_JACK:
		j->event[sizeof(j->event) - 1] = '\0';
		mtx_lock(&sc->lock);
		sc->jack = j->plug;
		strlcpy(sc->jack_event, j->event, sizeof(sc->jack_event));
		mtx_unlock(&sc->lock);
		if (strncmp(j->event, "jack ", 5) == 0) {
			snprintf(ev, sizeof(ev), "plug=%s", j->event + 5);
			devctl_notify("PRO1X", "audio", "JACK", ev);
		} else {
			snprintf(ev, sizeof(ev), "event=\"%s\"", j->event);
			devctl_notify("PRO1X", "audio", "BUTTON", ev);
		}
		return (0);
	default:
		return (ENOTTY);
	}
}

static int
p1_ctl_poll(struct cdev *cdev, int events, struct thread *td)
{
	struct p1_softc *sc = cdev->si_drv1;
	int revents = 0;

	mtx_lock(&sc->lock);
	if (events & (POLLIN | POLLRDNORM)) {
		if (sc->qlen > 0)
			revents |= events & (POLLIN | POLLRDNORM);
		else
			selrecord(td, &sc->sel);
	}
	mtx_unlock(&sc->lock);
	return (revents);
}

/* the buffers for the daemon's checks: playback at 0, recording behind */
static int
p1_ctl_mmap(struct cdev *cdev, vm_ooffset_t offset, vm_paddr_t *paddr,
    int nprot, vm_memattr_t *memattr)
{
	struct p1_softc *sc = cdev->si_drv1;
	struct p1_chan *ch;

	if (offset < 0 || offset >= PRO1X_PCM_NCHAN * PRO1X_PCM_BUFMAX ||
	    (nprot & PROT_EXEC) != 0)
		return (EINVAL);
	ch = &sc->ch[offset / PRO1X_PCM_BUFMAX];
	if (ch->pc == NULL)
		return (EINVAL);
	*paddr = ch->b->buf_addr + offset % PRO1X_PCM_BUFMAX;
	*memattr = VM_MEMATTR_UNCACHEABLE;
	return (0);
}

/* --- attach ----------------------------------------------------------- */

static int
p1_state_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct p1_softc *sc = arg1;
	struct p1_chan *ch, snap[PRO1X_PCM_NCHAN];
	struct sbuf sb;
	char jack[60];
	bool open;
	u_int i, dropped;
	int error;

	/* a copy first: the sysctl's sbuf may sleep */
	mtx_lock(&sc->lock);
	memcpy(snap, sc->ch, sizeof(snap));
	open = sc->ctl_open;
	dropped = sc->qdropped;
	strlcpy(jack, sc->jack_event[0] ? sc->jack_event : "jack unknown",
	    sizeof(jack));
	mtx_unlock(&sc->lock);
	sbuf_new_for_sysctl(&sb, NULL, 512, req);
	sbuf_printf(&sb, "daemon %s, %u events dropped, last %s", open ?
	    "attached" : "absent", dropped, jack);
	for (i = 0; i < PRO1X_PCM_NCHAN; i++) {
		ch = &snap[i];
		if (ch->pc == NULL)
			continue;
		sbuf_printf(&sb, "; %s: %s gen %u, %u starts, block %u, %u x "
		    "%u bytes at %#jx (ADSP %#jx), blocks by the daemon %u, by "
		    "the clock %u, stale %u, failed %u", i ? "rec" : "play",
		    !ch->run ? "stopped" : ch->by_daemon ? "daemon" : "clock",
		    ch->gen, ch->starts, ch->hw, ch->blkcnt, ch->blksz,
		    (uintmax_t)ch->b->buf_addr,
		    (uintmax_t)(ch->b->buf_addr | sc->sidbits),
		    ch->daemon_blocks, ch->clock_blocks, ch->stale, ch->fails);
	}
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

#ifndef SND_PRO1X_VM
/*
 * Linux's q6asm-dais node (qcom,q6asm-dais): its "iommus" stream without
 * translation, and the stream's low bits for the ADSP's addresses
 * (q6asm_dai_probe: args[0] & SID_MASK_DEFAULT, shifted by 32).
 */
static void
p1_smmu(struct p1_softc *sc)
{
	phandle_t node;
	pcell_t iommus[3];
	int error;

	node = ofw_bus_find_compatible(OF_finddevice("/"), "qcom,q6asm-dais");
	if (node <= 0 || OF_getencprop(node, "iommus", iommus,
	    sizeof(iommus)) != sizeof(iommus)) {
		device_printf(sc->dev, "no q6asm-dais iommus: ADSP addresses "
		    "without a stream\n");
		return;
	}
	sc->sidbits = (uint64_t)(iommus[1] & P1_SID_MASK) << 32;
	error = qcom_smmu_bypass(iommus[0], iommus[1], iommus[2], "snd_pro1x");
	if (error != 0)
		device_printf(sc->dev, "SMMU stream %#x: %d (the ADSP may not "
		    "reach the buffers)\n", iommus[1], error);
}
#endif

static int
p1_attach(device_t dev)
{
	struct p1_softc *sc = device_get_softc(dev);
	char status[SND_STATUSLEN];
	int error;

	sc->dev = dev;
	mtx_init(&sc->lock, device_get_nameunit(dev), "snd_pro1x", MTX_DEF);
	knlist_init_mtx(&sc->sel.si_note, &sc->lock);
	sc->play_fmts[0] = SND_FORMAT(AFMT_S16_LE, 2, 0);
	sc->play_fmts[1] = 0;
	sc->rec_fmts[0] = SND_FORMAT(AFMT_S16_LE, 2, 0);
	sc->rec_fmts[1] = SND_FORMAT(AFMT_S16_LE, 1, 0);
	sc->rec_fmts[2] = 0;
	sc->pcaps = (struct pcmchan_caps){ P1_RATE, P1_RATE, sc->play_fmts, 0 };
	sc->rcaps = (struct pcmchan_caps){ P1_RATE, P1_RATE, sc->rec_fmts, 0 };
	sc->recsrc = SOUND_MASK_MIC;
#ifndef SND_PRO1X_VM
	p1_smmu(sc);
#endif
	/* contiguous (one segment), page aligned, below 4 GiB */
	error = bus_dma_tag_create(bus_get_dma_tag(dev), PAGE_SIZE, 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL,
	    PRO1X_PCM_BUFMAX, 1, PRO1X_PCM_BUFMAX, 0, NULL, NULL, &sc->dtag);
	if (error != 0) {
		device_printf(dev, "DMA tag: %d\n", error);
		goto fail;
	}
	pcm_setflags(dev, pcm_getflags(dev) | SD_F_MPSAFE);
	pcm_init(dev, sc);
	/* a channel without its buffer is left out (and said so) */
	pcm_addchan(dev, PCMDIR_PLAY, &p1_chan_class, sc);
	pcm_addchan(dev, PCMDIR_REC, &p1_chan_class, sc);
	snprintf(status, sizeof(status), "on %s (ADSP, pro1x-audiod)",
	    device_get_nameunit(device_get_parent(dev)));
	if ((error = pcm_register(dev, status)) != 0) {
		device_printf(dev, "pcm_register: %d\n", error);
		return (error);
	}
	mixer_init(dev, &p1_mixer_class, sc);
	sc->ctl = make_dev(&p1_ctl_cdevsw, device_get_unit(dev), UID_ROOT,
	    GID_WHEEL, 0600, "pro1x_pcm");
	sc->ctl->si_drv1 = sc;
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "pro1x",
	    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, sc, 0,
	    p1_state_sysctl, "A", "The channels and pro1x-audiod");
	device_printf(dev, "playback at %#jx, recording at %#jx (ADSP "
	    "addresses | %#jx); /dev/pro1x_pcm for pro1x-audiod\n",
	    sc->ch[0].pc ? (uintmax_t)sc->ch[0].b->buf_addr : 0,
	    sc->ch[1].pc ? (uintmax_t)sc->ch[1].b->buf_addr : 0,
	    (uintmax_t)sc->sidbits);
	return (0);
fail:
	if (sc->dtag != NULL)
		bus_dma_tag_destroy(sc->dtag);
	knlist_destroy(&sc->sel.si_note);
	mtx_destroy(&sc->lock);
	return (error != 0 ? error : ENXIO);
}

static int
p1_detach(device_t dev)
{
	struct p1_softc *sc = device_get_softc(dev);
	int error, i;

	mtx_lock(&sc->lock);
	if (sc->ctl_open) {
		mtx_unlock(&sc->lock);
		return (EBUSY);
	}
	sc->stopped = true;
	mtx_unlock(&sc->lock);
	for (i = 0; i < PRO1X_PCM_NCHAN; i++)
		if (sc->ch[i].pc != NULL)
			callout_drain(&sc->ch[i].clock);
	if ((error = pcm_unregister(dev)) != 0) {
		mtx_lock(&sc->lock);
		sc->stopped = false;
		mtx_unlock(&sc->lock);
		return (error);
	}
	if (sc->ctl != NULL)
		destroy_dev(sc->ctl);
	seldrain(&sc->sel);
	knlist_destroy(&sc->sel.si_note);
	bus_dma_tag_destroy(sc->dtag);
	mtx_destroy(&sc->lock);
	return (0);
}

static int
p1_fdt_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "fxtec,pro1x-sndcard"))
		return (ENXIO);
	device_set_desc(dev, "F(x)tec Pro1X sound (ADSP)");
	return (BUS_PROBE_DEFAULT);
}

static device_method_t p1_fdt_methods[] = {
	DEVMETHOD(device_probe,		p1_fdt_probe),
	DEVMETHOD(device_attach,	p1_attach),
	DEVMETHOD(device_detach,	p1_detach),
	DEVMETHOD_END
};

static driver_t p1_fdt_driver = {
	"pcm",
	p1_fdt_methods,
	sizeof(struct p1_softc),
};

static void
p1_identify(driver_t *driver, device_t parent)
{
	if (!p1_nexus || device_find_child(parent, driver->name,
	    DEVICE_UNIT_ANY) != NULL)
		return;
	if (BUS_ADD_CHILD(parent, 0, driver->name, DEVICE_UNIT_ANY) == NULL)
		device_printf(parent, "snd_pro1x: add child failed\n");
}

static int
p1_nexus_probe(device_t dev)
{
	if (!p1_nexus)
		return (ENXIO);
	device_set_desc(dev, "F(x)tec Pro1X sound (ADSP, without the device "
	    "tree)");
	return (BUS_PROBE_NOWILDCARD);
}

static device_method_t p1_nexus_methods[] = {
	DEVMETHOD(device_identify,	p1_identify),
	DEVMETHOD(device_probe,		p1_nexus_probe),
	DEVMETHOD(device_attach,	p1_attach),
	DEVMETHOD(device_detach,	p1_detach),
	DEVMETHOD_END
};

static driver_t p1_nexus_driver = {
	"pcm",
	p1_nexus_methods,
	sizeof(struct p1_softc),
};

DRIVER_MODULE(snd_pro1x, simplebus, p1_fdt_driver, 0, 0);
DRIVER_MODULE(snd_pro1x, ofwbus, p1_fdt_driver, 0, 0);
DRIVER_MODULE(snd_pro1x, nexus, p1_nexus_driver, 0, 0);
MODULE_DEPEND(snd_pro1x, sound, SOUND_MINVER, SOUND_PREFVER, SOUND_MAXVER);
#ifndef SND_PRO1X_VM
MODULE_DEPEND(snd_pro1x, qcom_dsp, 1, 1, 1);
#endif
MODULE_VERSION(snd_pro1x, 1);
