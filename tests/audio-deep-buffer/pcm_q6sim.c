/*
 * pcm_q6sim -- an ALSA playback PCM that behaves, as far as a sound server
 * can tell, like a q6asm-dai front end of the Pro1X (sound/soc/qcom/qdsp6/
 * q6asm-dai.c), so that PulseAudio's wake-ups can be counted on a PC.
 *
 * What it copies from q6asm-dai:
 *   - 48 kHz, S16_LE, 2 channels (PulseAudio's settings on the phone);
 *   - periods of 480-frame steps, 2..8 of them, a period at most
 *     max_period_bytes (65536 before kernel patch 0035, 122880 after);
 *   - the position moves in whole periods: each period is one ASM write
 *     that the DSP finishes at once (WRITE_DONE), and only when the
 *     application had written the period completely (the DSP gets a write
 *     per full period, see q6asm_dai_ack); without one the DSP plays
 *     silence and the position stands still;
 *   - no rewinds (q6asm-dai has SNDRV_PCM_INFO_NO_REWINDS; PulseAudio does
 *     not rewind an ioplug PCM either).
 * Like the phone's PCM it makes PulseAudio run without timer-based
 * scheduling (there: the BATCH flag; here: not a hw PCM).
 *
 * The period clock is a timerfd, which is also the PCM's poll descriptor:
 * one expiry per period, i.e. one "interrupt". With "stats <file>" in its
 * configuration the counts are written there at each period: the periods
 * played, the expiries the DSP starved at, the starts, and the setup.
 *
 * Build (in the target, e.g. the Leste chroot):
 *   gcc -O2 -Wall -shared -fPIC -DPIC -o libasound_module_pcm_q6sim.so pcm_q6sim.c -lasound
 * Use (asound.conf):
 *   pcm_type.q6sim { lib "/path/libasound_module_pcm_q6sim.so" }
 *   pcm.q6sim { type q6sim max_period_bytes 122880 stats "/tmp/q6sim.stats" }
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#define RATE 48000
#define CHANNELS 2
#define FRAME_BYTES 4
#define STEP_FRAMES 480

struct q6sim {
	snd_pcm_ioplug_t io;
	int tfd;
	int stats_fd;
	unsigned int max_period_bytes;
	uint64_t hw;		/* frames the "DSP" took, monotonic */
	uint64_t appl;		/* frames the application wrote, monotonic */
	snd_pcm_uframes_t last_appl;
	uint64_t periods, starved, opens;
	struct timespec started;
};

static void stats(struct q6sim *q)
{
	char line[256];
	int n;

	if (q->stats_fd < 0)
		return;
	n = snprintf(line, sizeof(line),
		     "periods=%" PRIu64 " starved=%" PRIu64 " starts=%" PRIu64
		     " period_frames=%lu buffer_frames=%lu\n",
		     q->periods, q->starved, q->opens,
		     (unsigned long)q->io.period_size, (unsigned long)q->io.buffer_size);
	if (n > 0 && pwrite(q->stats_fd, line, n, 0) == n)
		(void)ftruncate(q->stats_fd, n);
}

/* io.appl_ptr wraps at the PCM's boundary: follow it as a monotonic count */
static void track_appl(struct q6sim *q)
{
	snd_pcm_uframes_t a = q->io.appl_ptr;

	if (a >= q->last_appl)
		q->appl += a - q->last_appl;
	else			/* wrapped */
		q->appl += a;
	q->last_appl = a;
}

static void timer_set(struct q6sim *q, int on)
{
	struct itimerspec t;
	uint64_t ns = (uint64_t)q->io.period_size * 1000000000ULL / RATE;

	memset(&t, 0, sizeof(t));
	if (on) {
		t.it_value.tv_sec = ns / 1000000000ULL;
		t.it_value.tv_nsec = ns % 1000000000ULL;
		t.it_interval = t.it_value;
	}
	timerfd_settime(q->tfd, 0, &t, NULL);
}

/* the "DSP" at each expiry: takes one queued full period, or starves */
static void run_dsp(struct q6sim *q)
{
	uint64_t n = 0;

	if (read(q->tfd, &n, sizeof(n)) != sizeof(n))
		return;
	track_appl(q);
	while (n--) {
		if (q->appl - q->hw >= q->io.period_size) {
			q->hw += q->io.period_size;
			q->periods++;
		} else {
			q->starved++;
		}
	}
	stats(q);
}

static int q6sim_start(snd_pcm_ioplug_t *io)
{
	struct q6sim *q = io->private_data;

	q->opens++;
	track_appl(q);
	timer_set(q, 1);
	return 0;
}

static int q6sim_stop(snd_pcm_ioplug_t *io)
{
	struct q6sim *q = io->private_data;

	timer_set(q, 0);
	stats(q);
	return 0;
}

static snd_pcm_sframes_t q6sim_pointer(snd_pcm_ioplug_t *io)
{
	struct q6sim *q = io->private_data;

	return (snd_pcm_sframes_t)(q->hw % io->buffer_size);
}

static int q6sim_prepare(snd_pcm_ioplug_t *io)
{
	struct q6sim *q = io->private_data;

	timer_set(q, 0);
	q->hw = q->appl = 0;
	q->last_appl = io->appl_ptr;
	return 0;
}

static int q6sim_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd,
			      unsigned int nfds, unsigned short *revents)
{
	struct q6sim *q = io->private_data;
	uint64_t filled;

	(void)nfds;
	if (pfd[0].revents & POLLIN)
		run_dsp(q);
	track_appl(q);
	filled = q->appl - q->hw;
	*revents = (io->state != SND_PCM_STATE_RUNNING ||
		    io->buffer_size - filled >= io->period_size) ? POLLOUT : 0;
	return 0;
}

static int q6sim_close(snd_pcm_ioplug_t *io)
{
	struct q6sim *q = io->private_data;

	stats(q);
	close(q->tfd);
	if (q->stats_fd >= 0)
		close(q->stats_fd);
	free(q);
	return 0;
}

static const snd_pcm_ioplug_callback_t q6sim_cb = {
	.start = q6sim_start,
	.stop = q6sim_stop,
	.pointer = q6sim_pointer,
	.prepare = q6sim_prepare,
	.poll_revents = q6sim_poll_revents,
	.close = q6sim_close,
};

static int constraints(struct q6sim *q)
{
	static const unsigned int access[] = {
		SND_PCM_ACCESS_MMAP_INTERLEAVED, SND_PCM_ACCESS_RW_INTERLEAVED };
	static const unsigned int format[] = { SND_PCM_FORMAT_S16_LE };
	unsigned int periods[64], n = 0, b;
	int err;

	for (b = STEP_FRAMES * FRAME_BYTES; b <= q->max_period_bytes && n < 64;
	     b += STEP_FRAMES * FRAME_BYTES)
		periods[n++] = b;
	if ((err = snd_pcm_ioplug_set_param_list(&q->io, SND_PCM_IOPLUG_HW_ACCESS, 2, access)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_list(&q->io, SND_PCM_IOPLUG_HW_FORMAT, 1, format)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&q->io, SND_PCM_IOPLUG_HW_CHANNELS,
						   CHANNELS, CHANNELS)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&q->io, SND_PCM_IOPLUG_HW_RATE, RATE, RATE)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_list(&q->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES,
						 n, periods)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&q->io, SND_PCM_IOPLUG_HW_PERIODS, 2, 8)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&q->io, SND_PCM_IOPLUG_HW_BUFFER_BYTES,
						   2 * STEP_FRAMES * FRAME_BYTES,
						   8 * q->max_period_bytes)) < 0)
		return err;
	return 0;
}

SND_PCM_PLUGIN_DEFINE_FUNC(q6sim)
{
	snd_config_iterator_t i, next;
	struct q6sim *q;
	long max_period = 65536;
	const char *s = NULL;
	int err;

	(void)root;
	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;

		if (snd_config_get_id(n, &id) < 0)
			continue;
		if (!strcmp(id, "comment") || !strcmp(id, "type") || !strcmp(id, "hint"))
			continue;
		if (!strcmp(id, "max_period_bytes")) {
			if (snd_config_get_integer(n, &max_period) < 0)
				return -EINVAL;
			continue;
		}
		if (!strcmp(id, "stats")) {
			if (snd_config_get_string(n, &s) < 0)
				return -EINVAL;
			continue;
		}
		SNDERR("q6sim: unknown field %s", id);
		return -EINVAL;
	}
	if (stream != SND_PCM_STREAM_PLAYBACK)
		return -EINVAL;

	q = calloc(1, sizeof(*q));
	if (!q)
		return -ENOMEM;
	q->max_period_bytes = (unsigned int)max_period;
	q->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	q->stats_fd = -1;
	if (s && *s)
		q->stats_fd = open(s, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
	q->io.version = SND_PCM_IOPLUG_VERSION;
	q->io.name = "q6asm-dai simulation";
	q->io.mmap_rw = 1;
	q->io.poll_fd = q->tfd;
	q->io.poll_events = POLLIN;
	q->io.callback = &q6sim_cb;
	q->io.private_data = q;

	err = snd_pcm_ioplug_create(&q->io, name, stream, mode);
	if (err < 0) {
		close(q->tfd);
		free(q);
		return err;
	}
	err = constraints(q);
	if (err < 0) {
		snd_pcm_ioplug_delete(&q->io);
		return err;
	}
	*pcmp = q->io.pcm;
	return 0;
}

SND_PCM_PLUGIN_SYMBOL(q6sim);
