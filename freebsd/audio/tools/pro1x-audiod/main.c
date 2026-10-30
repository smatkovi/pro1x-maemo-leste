/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pro1x-audiod -- FreeBSD's sound on the Pro1X (freebsd/audio/README.md):
 * the ADSP's audio services (/dev/apr), snd_pro1x.ko's channels
 * (/dev/pro1x_pcm), the codec's replay sequences (/dev/qcom_lpass, I2C,
 * GPIOs, rails), and a control socket, one text line per request and per
 * answer (pro1x-audioctl):
 *
 *   status                     "ok play=... rec=... route=spk rx=up(1) ..."
 *   route spk|hph|ear          the playback path (loudspeaker, headphones,
 *                              earpiece)
 *   rec mic|amic               the capture path (built-in DMIC3, headset)
 *   volume 0-100               the master volume (the RX macro's gain)
 *   fm on [gain]|off           the FM radio's loopback (SLIMBUS_8_TX)
 *   voice start|stop           a call's voice session and its back ends
 *   up rx|va|tx, down ...      hold a back end (tests)
 *   seq <section>              run a section of the sequences (tests)
 *   reload                     read the sequences again
 *   rd <target> <reg>          read a register (wcd, aw0, rxmacro, ...)
 *   wr <target> <reg> <val>    write one
 *   swr <words>                SoundWire: init, enum, stream, dump
 *
 *   pro1x-audiod [-d] [-v] [-T] [-n] [-s seqfile] [-r route] [-i iicdev]
 *                [-t idle_ms] [-S socket]
 *     -d  stay in the foreground, log to stdout   -v  every command
 *     -T  every register access                   -n  no /dev/pro1x_pcm
 *
 * Needs (freebsd/dsp, freebsd/fm): the ADSP running (pro1x-dsp start adsp)
 * with pd-mapper, qcom_apr.ko (/dev/apr: one user, so pro1x-q6 and
 * pro1x-fmaudio's AFE part cannot run beside it), qcom_lpass.ko,
 * qcom_dsp.ko (the rails), snd_pro1x.ko (after sound.ko).
 */

#include <sys/types.h>
#include <sys/gpio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/rtprio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/un.h>

#include <dev/iicbus/iic.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audiod.h"

#define	SOCK_PATH	"/var/run/pro1x-audio.sock"
#define	SEQ_HOST	"/dev/pro1x-host/freebsd/audio/pro1x-audio.seq"
#define	SEQ_IMAGE	"/usr/local/share/pro1x/audio/pro1x-audio.seq"

static struct audiod D;
static struct voice V;
static volatile sig_atomic_t stop;
static int apr_fd = -1, pcm_fd = -1, gpio_fd = -1, iic_fd = -1;
static const char *seq_path, *sock_path = SOCK_PATH, *iic_path;
static struct mmio windows[TGT_NMMIO];

/* the LPASS windows (qcom_lpass.c's table) by target */
static const struct { uint64_t pa; size_t len; } wtab[TGT_NMMIO] = {
	[TGT_RXMACRO] = { 0x0a600000, 0x1000 },
	[TGT_TXMACRO] = { 0x0a620000, 0x1000 },
	[TGT_VAMACRO] = { 0x0a730000, 0x1000 },
	[TGT_SWR0] = { 0x0a740000, 0x10000 },
	[TGT_SWR1] = { 0x0a610000, 0x10000 },
	[TGT_LPASS_TLMM] = { 0x0a7c0000, 0x20000 },
	[TGT_LPASS_TLMM_SLEW] = { 0x0a950000, 0x10000 },
	[TGT_LPASS_AUDIOCC] = { 0x0a6a9000, 0x1000 },
	[TGT_LPASSCC] = { 0x0a7e0000, 0x12000 },
};

static void
on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

/* --- the transports ----------------------------------------------------- */

static int
apr_send_fd(void *ctx, const uint8_t *pkt, size_t len)
{
	(void)ctx;
	return (write(apr_fd, pkt, len) == (ssize_t)len ? 0 : -1);
}

static int
apr_recv_fd(void *ctx, uint8_t *buf, size_t size, int timeout_ms)
{
	struct pollfd p = { apr_fd, POLLIN, 0 };
	ssize_t n;

	(void)ctx;
	if (poll(&p, 1, timeout_ms) <= 0)
		return (0);
	n = read(apr_fd, buf, size);
	return (n < 0 ? (errno == EWOULDBLOCK ? 0 : -1) : (int)n);
}

static struct apr_io aprio = { apr_send_fd, apr_recv_fd, NULL };

static int
pcm_read_fd(void *ctx, struct pro1x_pcm_event *ev, unsigned max)
{
	ssize_t n;

	(void)ctx;
	n = read(pcm_fd, ev, max * sizeof(*ev));
	if (n < 0)
		return (errno == EWOULDBLOCK ? 0 : -1);
	return ((int)(n / (ssize_t)sizeof(*ev)));
}

static int
pcm_advance_fd(void *ctx, struct pro1x_pcm_advance *a)
{
	(void)ctx;
	return (ioctl(pcm_fd, PRO1X_PCM_ADVANCE, a));
}

static int
pcm_fail_fd(void *ctx, struct pro1x_pcm_advance *a)
{
	(void)ctx;
	return (ioctl(pcm_fd, PRO1X_PCM_FAIL, a));
}

static struct pcm_io pcmio = { pcm_read_fd, pcm_advance_fd, pcm_fail_fd, NULL };

/* --- the platform: windows, GPIOs, rails, I2C, delays ------------------- */

static void
map_windows(void)
{
	int fd, t;
	void *p;

	if ((fd = open("/dev/qcom_lpass", O_RDWR)) < 0) {
		fma_log("/dev/qcom_lpass: %s (kldload qcom_lpass): no codec",
		    strerror(errno));
		return;
	}
	for (t = 0; t < TGT_NMMIO; t++) {
		p = mmap(NULL, wtab[t].len, PROT_READ | PROT_WRITE, MAP_SHARED,
		    fd, (off_t)wtab[t].pa);
		if (p == MAP_FAILED) {
			fma_log("%s at %#jx: %s", tgt_name(t),
			    (uintmax_t)wtab[t].pa, strerror(errno));
			continue;
		}
		windows[t].name = tgt_name(t);
		windows[t].pa = wtab[t].pa;
		windows[t].len = wtab[t].len;
		windows[t].base = p;
		D.cio.win[t] = &windows[t];
	}
	close(fd);
	D.swr[0].m = D.cio.win[TGT_SWR0];
	D.swr[1].m = D.cio.win[TGT_SWR1];
}

static int
cb_gpio(void *ctx, unsigned pin, int val)
{
	struct gpio_pin gp;
	struct gpio_req gr;

	(void)ctx;
	if (gpio_fd < 0 && (gpio_fd = open("/dev/gpioc0", O_RDWR)) < 0)
		return (-errno);
	memset(&gp, 0, sizeof(gp));
	gp.gp_pin = pin;
	gp.gp_flags = GPIO_PIN_OUTPUT;
	if (ioctl(gpio_fd, GPIOSETCONFIG, &gp) != 0)
		return (-errno);
	memset(&gr, 0, sizeof(gr));
	gr.gp_pin = pin;
	gr.gp_value = val ? GPIO_PIN_HIGH : GPIO_PIN_LOW;
	if (ioctl(gpio_fd, GPIOSET, &gr) != 0)
		return (-errno);
	if (D.cio.trace)
		fma_log("gpio %u <- %d", pin, val);
	return (0);
}

/* qcom_dsp's RPM votes: "l9 1" (the device tree's minimum voltage) */
static int
cb_rail(void *ctx, const char *name, bool on, uint32_t uv)
{
	char req[32];

	(void)ctx;
	(void)uv;
	snprintf(req, sizeof(req), "%s %d", name, on ? 1 : 0);
	if (sysctlbyname("hw.qcom_rpm.regulator", NULL, NULL, req,
	    strlen(req)) != 0) {
		fma_log("rail %s: %s", req, strerror(errno));
		return (-errno);
	}
	fma_log("rail %s %s", name, on ? "on" : "off");
	return (0);
}

static uint8_t
aw_addr(int tgt)
{
	return (tgt == TGT_AW0 ? 0x59 : 0x5a);	/* the device tree's */
}

static int
iic_xfer(uint8_t addr, uint8_t reg, uint8_t *val, bool read)
{
	struct iic_msg m[2];
	struct iic_rdwr_data rd;
	uint8_t w[2] = { reg, read ? 0 : *val };

	memset(m, 0, sizeof(m));
	m[0].slave = (uint16_t)(addr << 1);	/* FreeBSD: 8-bit addresses */
	m[0].flags = IIC_M_WR;
	m[0].len = read ? 1 : 2;
	m[0].buf = w;
	m[1].slave = (uint16_t)(addr << 1);
	m[1].flags = IIC_M_RD;
	m[1].len = 1;
	m[1].buf = val;
	rd.msgs = m;
	rd.nmsgs = read ? 2 : 1;
	return (ioctl(iic_fd, I2CRDWR, &rd) == 0 ? 0 : -errno);
}

/*
 * The keyboard's bus (4a84000) has the AW87339s: the iic device whose 0x59
 * answers with the chip id 0x39 (aw87339.c's check). The chip answers only
 * with its enable line high (aw87339_i2c_probe raises it 2-3 ms before the
 * read), so the bus is looked for at the first access, which the
 * sequence makes after its "gpio 85 1"; until it is found every access
 * looks again.
 */
static int
open_iic(void)
{
	char path[32];
	uint8_t id = 0;
	int i;

	if (iic_fd >= 0)
		return (0);
	if (iic_path != NULL) {
		if ((iic_fd = open(iic_path, O_RDWR)) < 0)
			fma_log("%s: %s", iic_path, strerror(errno));
		return (iic_fd < 0 ? -ENXIO : 0);
	}
	for (i = 0; i < 4; i++) {
		snprintf(path, sizeof(path), "/dev/iic%d", i);
		if ((iic_fd = open(path, O_RDWR)) < 0)
			continue;
		if (iic_xfer(0x59, 0x00, &id, true) == 0 && id == 0x39) {
			fma_log("AW87339s on %s", path);
			return (0);
		}
		close(iic_fd);
		iic_fd = -1;
	}
	fma_log("no AW87339 on /dev/iic0-3 (its enable line low?)");
	return (-ENXIO);
}

static int
cb_i2c_write(void *ctx, int tgt, uint8_t reg, uint8_t val)
{
	(void)ctx;
	if (open_iic() != 0)
		return (-ENXIO);
	if (D.cio.trace)
		fma_log("i2c %#x: %#x <- %#x", aw_addr(tgt), reg, val);
	return (iic_xfer(aw_addr(tgt), reg, &val, false));
}

static int
cb_i2c_read(void *ctx, int tgt, uint8_t reg, uint8_t *val)
{
	(void)ctx;
	if (open_iic() != 0)
		return (-ENXIO);
	return (iic_xfer(aw_addr(tgt), reg, val, true));
}

static void
cb_delay(void *ctx, uint32_t us)
{
	(void)ctx;
	usleep(us);
}

/* --- APR packets nobody waits for ---------------------------------------- */

static void
on_async(struct q6 *q, const uint8_t *pkt, size_t len)
{
	(void)q;
	switch (apr_src_svc(pkt)) {
	case APR_SVC_ASM:
		pcm_asm_event(&D, pkt, len);
		return;
	default:
		if (apr_opcode(pkt) != APR_BASIC_RSP_RESULT)
			fma_log("apr: event from service %u: opcode %#x token "
			    "%#x, %zu bytes", apr_src_svc(pkt), apr_opcode(pkt),
			    apr_token(pkt), len);
	}
}

/* --- the control socket ----------------------------------------------------- */

static const char *
stname(const struct stream *s)
{
	return (s->state == ST_RUNNING ? "running" : s->state == ST_FAILED ?
	    "failed" : "idle");
}

static int
load_seq(char *msg, size_t len)
{
	struct seq s;
	char e[256];
	const char *path = seq_path;

	if (path == NULL)
		path = access(SEQ_HOST, R_OK) == 0 ? SEQ_HOST : SEQ_IMAGE;
	if (seq_load(&s, path, e, sizeof(e)) != 0) {
		snprintf(msg, len, "%s", e);
		return (-1);
	}
	if (D.have_seq)
		seq_free(&D.seq);
	D.seq = s;
	D.have_seq = true;
	snprintf(msg, len, "%s: %u sections, %u lines", path, s.nsect, s.nlines);
	return (0);
}

static int
be_by_name(const char *s)
{
	return (strcmp(s, "rx") == 0 ? BE_RX : strcmp(s, "va") == 0 ? BE_VA :
	    strcmp(s, "tx") == 0 ? BE_TX : -1);
}

static void
request(char *line, char *ans, size_t alen)
{
	char *w[8], *p, msg[256];
	int n = 0, r, be;
	uint32_t reg, val;
	struct seq_line l;

	for (p = strtok(line, " \t\r\n"); p != NULL && n < 8;
	    p = strtok(NULL, " \t\r\n"))
		w[n++] = p;
	if (n == 0) {
		snprintf(ans, alen, "error EINVAL empty");
		return;
	}
	if (strcmp(w[0], "status") == 0) {
		snprintf(ans, alen, "ok play=%s rec=%s route=%s rec_route=%s "
		    "rx=%s(%d) va=%s(%d) tx=%s(%d) fm=%d voice=%s volume=%u "
		    "blocks_out=%u blocks_in=%u seq=%s apr=%u/%u/%u timeouts=%u "
		    "refused=%u swr0=%s/dev%u swr1=%s/dev%u",
		    stname(&D.st[0]), stname(&D.st[1]), D.route, D.recroute,
		    D.be[BE_RX].up ? "up" : "down", D.be[BE_RX].users,
		    D.be[BE_VA].up ? "up" : "down", D.be[BE_VA].users,
		    D.be[BE_TX].up ? "up" : "down", D.be[BE_TX].users, D.fm_on,
		    V.state == VP_UP ? "up" : V.state == VP_FAILED ? "failed" :
		    "idle", D.mix[0], D.blocks_out, D.blocks_in,
		    D.have_seq ? D.seq.file : "none", D.q.sent, D.q.received,
		    D.q.async_pkts, D.q.timeouts, D.q.refused,
		    D.swr[0].up ? "up" : "down", D.swr[0].devnum,
		    D.swr[1].up ? "up" : "down", D.swr[1].devnum);
		return;
	}
	if (strcmp(w[0], "route") == 0 && n == 2) {
		r = backend_route(&D, w[1]);
		snprintf(ans, alen, r == 0 ? "ok route=%s" : "error EINVAL %s",
		    w[1]);
		return;
	}
	if (strcmp(w[0], "rec") == 0 && n == 2 && (!strcmp(w[1], "mic") ||
	    !strcmp(w[1], "amic"))) {
		snprintf(D.recroute, sizeof(D.recroute), "%s", w[1]);
		snprintf(ans, alen, "ok rec_route=%s (from the next recording)",
		    w[1]);
		return;
	}
	if (strcmp(w[0], "volume") == 0 && n == 2) {
		backend_mixer(&D, 0, (unsigned)strtoul(w[1], NULL, 0) > 100 ?
		    100 : (unsigned)strtoul(w[1], NULL, 0));
		snprintf(ans, alen, "ok volume=%u", D.mix[0]);
		return;
	}
	if (strcmp(w[0], "fm") == 0 && n >= 2) {
		if (n == 3)
			D.fm_gain = (uint16_t)strtoul(w[2], NULL, 0);
		r = backend_fm(&D, strcmp(w[1], "on") == 0);
		if (r == 0)
			snprintf(ans, alen, "ok fm=%d", D.fm_on);
		else
			snprintf(ans, alen, "error EIO fm: %s", q6_err(r));
		return;
	}
	if (strcmp(w[0], "voice") == 0 && n == 2) {
		if (strcmp(w[1], "start") == 0) {
			if ((r = voice_start(&D.q, &V)) == 0) {
				if (backend_get(&D, BE_RX) == 0)
					backend_get(&D, BE_VA);
			}
		} else {
			r = voice_stop(&D.q, &V);
			if (D.be[BE_RX].users > 0)
				backend_put(&D, BE_RX);
			if (D.be[BE_VA].users > 0)
				backend_put(&D, BE_VA);
		}
		if (r == 0)
			snprintf(ans, alen, "ok voice=%s mvm=%#x cvp=%#x",
			    V.state == VP_UP ? "up" : "idle", V.mvm, V.cvp);
		else
			snprintf(ans, alen, "error EIO voice: step %d, status "
			    "%#x", V.failed_step, V.failed_status);
		return;
	}
	if ((strcmp(w[0], "up") == 0 || strcmp(w[0], "down") == 0) && n == 2 &&
	    (be = be_by_name(w[1])) >= 0) {
		if (w[0][0] == 'u')
			r = backend_get(&D, be);
		else {
			r = 0;
			backend_put(&D, be);
		}
		snprintf(ans, alen, r == 0 ? "ok %s %s" : "error EIO %s %s",
		    w[1], D.be[be].up ? "up" : "down");
		return;
	}
	if (strcmp(w[0], "seq") == 0 && n == 2) {
		/* by hand: the caller knows which blocks are clocked */
		D.cur = &D.be[BE_RX];
		r = seq_run(&D.seq, w[1], &D.cio);
		snprintf(ans, alen, r == 0 ? "ok [%s] done" : r == -ENOENT ?
		    "error ENOENT no [%s]" : "error EIO [%s]: errors", w[1]);
		return;
	}
	if (strcmp(w[0], "reload") == 0) {
		r = load_seq(msg, sizeof(msg));
		snprintf(ans, alen, "%s %s", r == 0 ? "ok" : "error EINVAL", msg);
		return;
	}
	if ((strcmp(w[0], "rd") == 0 && n == 3) ||
	    (strcmp(w[0], "wr") == 0 && n == 4)) {
		char text[128];
		struct seq one;

		snprintf(text, sizeof(text), "[x]\n%s %s %s %s\n",
		    w[0][0] == 'r' ? "r" : "w", w[1], w[2], n == 4 ? w[3] : "");
		if (seq_parse(&one, text, "request", msg, sizeof(msg)) != 0) {
			snprintf(ans, alen, "error EINVAL %s", msg);
			return;
		}
		l = one.lines[0];
		reg = l.reg;
		val = 0;
		D.cio.trace++;
		r = seq_run(&one, "x", &D.cio);
		D.cio.trace--;
		seq_free(&one);
		snprintf(ans, alen, r == 0 ? "ok (the value is in the log)" :
		    "error EIO %s %#x", w[1], reg);
		(void)val;
		return;
	}
	if (strcmp(w[0], "swr") == 0 && n >= 3) {
		char words[160] = "";
		int i;

		for (i = 1; i < n; i++) {
			strlcat(words, w[i], sizeof(words));
			strlcat(words, " ", sizeof(words));
		}
		r = D.cio.swr(&D, words);
		snprintf(ans, alen, r == 0 ? "ok" : "error EIO %s", q6_err(r));
		return;
	}
	snprintf(ans, alen, "error EINVAL unknown request %s", w[0]);
}

static void
serve(int ls)
{
	char line[256], ans[1024];
	ssize_t n;
	int c;

	if ((c = accept(ls, NULL, NULL)) < 0)
		return;
	n = read(c, line, sizeof(line) - 1);
	if (n > 0) {
		line[n] = '\0';
		request(line, ans, sizeof(ans));
		strlcat(ans, "\n", sizeof(ans));
		(void)write(c, ans, strlen(ans));
	}
	close(c);
}

static int
listen_sock(void)
{
	struct sockaddr_un sun;
	int s;

	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return (-1);
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, sock_path, sizeof(sun.sun_path));
	unlink(sock_path);
	if (bind(s, (struct sockaddr *)&sun, sizeof(sun)) != 0 ||
	    listen(s, 4) != 0) {
		close(s);
		return (-1);
	}
	chmod(sock_path, 0666);
	return (s);
}

static void
usage(void)
{
	fprintf(stderr, "usage: pro1x-audiod [-dvTn] [-s seqfile] [-r route] "
	    "[-i iicdev] [-t idle_ms] [-S socket]\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	struct pro1x_pcm_event ev[16];
	struct pollfd pfd[3];
	struct rtprio rt = { RTP_PRIO_REALTIME, 10 };
	char msg[256];
	int c, ls, n, i, fg = 0, nopcm = 0, timeout;
	uint64_t now, next;

	while ((c = getopt(argc, argv, "dvTns:r:i:t:S:")) != -1)
		switch (c) {
		case 'd': fg = 1; break;
		case 'v': fma_verbose = 1; break;
		case 'T': D.cio.trace = 1; break;
		case 'n': nopcm = 1; break;
		case 's': seq_path = optarg; break;
		case 'r': snprintf(D.route, sizeof(D.route), "%s", optarg); break;
		case 'i': iic_path = optarg; break;
		case 't': D.idle_ms = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'S': sock_path = optarg; break;
		default: usage();
		}
	backend_init(&D);
	D.voice = &V;
	D.cio.gpio = cb_gpio;
	D.cio.rail = cb_rail;
	D.cio.i2c_write = cb_i2c_write;
	D.cio.i2c_read = cb_i2c_read;
	D.cio.delay_us = cb_delay;
	if ((apr_fd = open("/dev/apr", O_RDWR)) < 0) {
		fprintf(stderr, "pro1x-audiod: /dev/apr: %s (kldload qcom_apr; "
		    "the ADSP running?)\n", strerror(errno));
		return (1);
	}
	D.q.io = &aprio;
	D.q.timeout_ms = 3000;	/* q6afe.c's TIMEOUT_MS; q6asm waits 5 s */
	D.q.async = on_async;
	D.pcm = &pcmio;
	if (!nopcm && (pcm_fd = open(PRO1X_PCM_DEV, O_RDWR | O_NONBLOCK)) < 0) {
		fprintf(stderr, "pro1x-audiod: %s: %s (kldload snd_pro1x)\n",
		    PRO1X_PCM_DEV, strerror(errno));
		return (1);
	}
	map_windows();
	if (load_seq(msg, sizeof(msg)) != 0)
		fma_log("sequences: %s: no codec path, the ADSP's side alone",
		    msg);
	else
		fma_log("sequences: %s", msg);
	if ((ls = listen_sock()) < 0) {
		fprintf(stderr, "pro1x-audiod: %s: %s\n", sock_path,
		    strerror(errno));
		return (1);
	}
	if (!fg && daemon(0, 0) != 0)
		return (1);
	if (rtprio(RTP_SET, 0, &rt) != 0)
		fma_log("rtprio: %s", strerror(errno));
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);
	fma_log("pro1x-audiod: ready (route %s, idle %u ms)", D.route,
	    D.idle_ms);
	while (!stop) {
		n = 0;
		pfd[n++] = (struct pollfd){ apr_fd, POLLIN, 0 };
		pfd[n++] = (struct pollfd){ ls, POLLIN, 0 };
		if (pcm_fd >= 0)
			pfd[n++] = (struct pollfd){ pcm_fd, POLLIN, 0 };
		now = fma_now_ms();
		next = now + 1000;
		for (i = 0; i < BE_N; i++)
			if (D.be[i].idle_at != 0 && D.be[i].idle_at < next)
				next = D.be[i].idle_at;
		timeout = next > now ? (int)(next - now) : 0;
		if (poll(pfd, (nfds_t)n, timeout) < 0 && errno != EINTR)
			break;
		if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR))
			if (apr_poll(&D.q, 0) < 0) {
				fma_log("/dev/apr: the ADSP went away");
				break;
			}
		if (pcm_fd >= 0 && (pfd[2].revents & POLLIN))
			while ((c = D.pcm->read_events(NULL, ev, 16)) > 0)
				for (i = 0; i < c; i++)
					pcm_event(&D, &ev[i]);
		if (pfd[1].revents & POLLIN)
			serve(ls);
		backend_tick(&D);
	}
	fma_log("pro1x-audiod: stopping");
	pcm_stop_all(&D);
	if (D.fm_on)
		backend_fm(&D, false);
	if (V.state == VP_UP)
		voice_stop(&D.q, &V);
	for (i = 0; i < BE_N; i++) {
		D.be[i].users = 0;
		D.be[i].idle_at = 1;
	}
	backend_tick(&D);
	unlink(sock_path);
	return (0);
}
