/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pro1x-fmaudio -- the FM radio's sound path on FreeBSD (freebsd/fm/README.md):
 *
 *   pro1x-fmaudio [options] probe    the ADSP's SLIMbus service up, the NGD's
 *                                    capability exchange, the WCN3950's two
 *                                    logical addresses and its revision; down
 *   pro1x-fmaudio [options] slim     probe, then the FM stream on the bus
 *                                    (ports 1, 2 on channels 160, 161) for
 *                                    -t seconds
 *   pro1x-fmaudio [options] afe      slim, then the ADSP's side: SLIMBUS_8_TX
 *                                    configured and started; without -N also
 *                                    the clocks, RX_CODEC_DMA_RX_0 and the AFE
 *                                    loopback between them (Android's
 *                                    "play-fm"), for -t seconds
 *   pro1x-fmaudio [options] clocks   the output path's LPASS clock votes
 *                                    alone, for -t seconds
 *   pro1x-fmaudio [options] regs     the NGD's registers (QMI power first)
 *   pro1x-fmaudio [options] start    afe until SIGTERM or SIGINT: pro1x-fmd's
 *                                    "audio on" (prints "fmaudio: ready" or
 *                                    "fmaudio: error ...")
 *
 *   -v  every message on the bus and to the ADSP   -T  every register access
 *   -t SEC  how long test commands hold (default 5)
 *   -w SEC  how long to wait for the WCN3950 on the bus (default 5)
 *   -g GAIN the loopback gain, Q13 (default 0x2000, unity)
 *   -c A,B  the SLIMbus channels (default 160,161)
 *   -p A,B  the WCN3950's FM ports (default 1,2)
 *   -N      no output side: SLIMBUS_8_TX only (no clocks, RX port, loopback)
 *   -M MS   the timeout of one SLIMbus message (default 1000, Linux's HZ)
 *
 * Needs (freebsd/dsp/README.md): the ADSP running (pro1x-dsp start adsp),
 * qrtr.ko and pd-mapper, qcom_apr.ko (/dev/apr, which allows one user, so
 * not together with pro1x-q6), qcom_lpass.ko (/dev/qcom_lpass); the
 * WCN3950 up with its FM receiver on (pro1x-fmd, "on": FC0E 01 puts the
 * receiver's audio on its SLIMbus ports). No register is read before the
 * ADSP's SLIMbus service has powered the bus up (QMI).
 *
 * What is missing for sound (README, stage 3): the RX macro, the SoundWire
 * master and the WCD937x behind RX_CODEC_DMA_RX_0.
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fmaudio.h"

/* the QMI encoding of pro1x-q6 (host tested by freebsd/fm/test/test_q6) */
#define	PRO1X_Q6_NO_MAIN
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../../dsp/tools/pro1x-q6/pro1x-q6.c"
#pragma GCC diagnostic pop

#define	LPASS_DEV	"/dev/qcom_lpass"
#define	APR_DEV		"/dev/apr"
#define	NGD_PA		0x0a5c0000	/* slim-ngd@a5c0000 (patch 0021) */
#define	NGD_LEN		0x2c000
#define	MCLK_FREQ	19200000	/* lpass-rx-macro.c; va: 2 x 9.6 MHz */

static struct {
	int	hold_s, wait_s;
	uint16_t gain;
	bool	no_output;
} opt = { 5, 5, AFE_LOOPBACK_GAIN_UNITY, false };

static volatile sig_atomic_t stop;

static void
on_signal(int sig)
{
	stop = 1;
}

/* --- QMI: the ADSP's SLIMbus service (qcom_slim_qmi_*) ------------------ */

struct slimqmi {
	int	s;
	bool	powered;
	uint16_t txn;
#ifdef __FreeBSD__
	struct sockaddr_qrtr srv;
#endif
};

#if defined(FMAUDIO_TEST)
/* the host test's (freebsd/fm/test/test_fmaudio.c): a simulated ADSP */
static int qmi_up(struct slimqmi *q);
static void qmi_down(struct slimqmi *q);
static int map_window(struct mmio *m, const char *name, uint64_t pa,
    size_t len);
#elif defined(__FreeBSD__)
static struct sockaddr_qrtr
qrtr_addr(uint32_t node, uint32_t port)
{
	struct sockaddr_qrtr sq;

	memset(&sq, 0, sizeof(sq));
	sq.sq_len = sizeof(sq);
	sq.sq_family = AF_QIPCRTR;
	sq.sq_node = node;
	sq.sq_port = port;
	return (sq);
}

/*
 * The service (qmi_add_lookup(769, 1, 0)): the name service's answers,
 * instance word version | instance << 8 = 1.
 */
static int
qmi_find(struct slimqmi *q)
{
	struct sockaddr_qrtr ns = qrtr_addr(1, QRTR_PORT_CTRL), from;
	uint32_t pkt[5] = { QRTR_TYPE_NEW_LOOKUP, SLIMBUS_QMI_SVC_ID, 0, 0, 0 };
	uint32_t rsp[5];
	socklen_t fl;
	int found = -1;

	if (sendto(q->s, pkt, sizeof(pkt), 0, (struct sockaddr *)&ns,
	    sizeof(ns)) != sizeof(pkt))
		return (-1);
	for (;;) {
		fl = sizeof(from);
		if (recvfrom(q->s, rsp, sizeof(rsp), 0, (struct sockaddr *)&from,
		    &fl) < (ssize_t)sizeof(rsp))
			break;
		if (rsp[0] != QRTR_TYPE_NEW_SERVER || rsp[1] == 0)
			break;
		fma_log("slim: QMI service %u instance %#x at %u:%u", rsp[1],
		    rsp[2], rsp[3], rsp[4]);
		if (rsp[1] == SLIMBUS_QMI_SVC_ID && rsp[2] ==
		    (SLIMBUS_QMI_SVC_V1 | SLIMBUS_QMI_INS_ID << 8) && found < 0) {
			q->srv = qrtr_addr(rsp[3], rsp[4]);
			found = 0;
		}
	}
	return (found);
}

static int
qmi_req(struct slimqmi *q, const uint8_t *req, size_t len, uint16_t txn,
    uint16_t msg_id, const char *what)
{
	uint8_t rsp[256];
	uint16_t result, error;
	ssize_t n;
	int i;

	if (sendto(q->s, req, len, 0, (struct sockaddr *)&q->srv,
	    sizeof(q->srv)) != (ssize_t)len) {
		fma_log("slim: QMI %s: %s", what, strerror(errno));
		return (-1);
	}
	for (i = 0; i < 8; i++) {
		if ((n = recv(q->s, rsp, sizeof(rsp), 0)) < 0)
			break;
		if (qmi_parse_result(rsp, (size_t)n, txn, msg_id, &result,
		    &error) == 0) {
			fma_log("slim: QMI %s: result %u error %u", what, result,
			    error);
			return (result == 0 ? 0 : -1);
		}
	}
	fma_log("slim: QMI %s: no answer", what);
	return (-1);
}

/* qcom_slim_qmi_init (the ADSP the bus's master) and the power request */
static int
qmi_up(struct slimqmi *q)
{
	struct timeval tv = { 1, 0 };	/* SLIMBUS_QMI_RESP_TOUT */
	uint8_t req[64];
	size_t len;
	int i;

	if ((q->s = socket(AF_QIPCRTR, SOCK_DGRAM, 0)) < 0) {
		fma_log("slim: socket(AF_QIPCRTR): %s (kldload qrtr)",
		    strerror(errno));
		return (-1);
	}
	setsockopt(q->s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	for (i = 0; i < 10 && qmi_find(q) != 0; i++)
		fma_sleep_ms(500);
	if (i == 10) {
		fma_log("slim: no SLIMbus service (QMI %u version %u) on QRTR",
		    SLIMBUS_QMI_SVC_ID, SLIMBUS_QMI_SVC_V1);
		return (-1);
	}
	if (connect(q->s, (struct sockaddr *)&q->srv, sizeof(q->srv)) != 0)
		fma_log("slim: QMI connect: %s", strerror(errno));
	len = qmi_select_instance(req, ++q->txn);
	if (qmi_req(q, req, len, q->txn, SLIMBUS_QMI_SELECT_INSTANCE_REQ,
	    "select instance 0, the ADSP master") != 0)
		return (-1);
	len = qmi_power(req, ++q->txn, true);
	if (qmi_req(q, req, len, q->txn, SLIMBUS_QMI_POWER_REQ,
	    "power active") != 0)
		return (-1);
	q->powered = true;
	return (0);
}

static void
qmi_down(struct slimqmi *q)
{
	uint8_t req[64];
	size_t len;

	if (q->powered) {
		len = qmi_power(req, ++q->txn, false);
		qmi_req(q, req, len, q->txn, SLIMBUS_QMI_POWER_REQ,
		    "power inactive");
		q->powered = false;
	}
	if (q->s >= 0)
		close(q->s);
	q->s = -1;
}

/* --- the windows, /dev/apr ----------------------------------------------- */

static int
map_window(struct mmio *m, const char *name, uint64_t pa, size_t len)
{
	static int fd = -1;
	void *p;
	int trace;

	if (fd < 0 && (fd = open(LPASS_DEV, O_RDWR)) < 0) {
		fma_log("mmio: %s: %s (kldload qcom_lpass)", LPASS_DEV,
		    strerror(errno));
		return (-1);
	}
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)pa);
	if (p == MAP_FAILED) {
		fma_log("mmio: %s at %#jx: %s", name, (uintmax_t)pa,
		    strerror(errno));
		return (-1);
	}
	trace = m->trace;
	memset(m, 0, sizeof(*m));
	m->trace = trace;
	m->name = name;
	m->pa = pa;
	m->len = len;
	m->base = p;
	return (0);
}
#else /* neither the phone nor the test */
static int
qmi_up(struct slimqmi *q)
{
	fma_log("slim: QRTR is FreeBSD's here");
	return (-1);
}

static void
qmi_down(struct slimqmi *q)
{
}

static int
map_window(struct mmio *m, const char *name, uint64_t pa, size_t len)
{
	fma_log("mmio: %s is FreeBSD's", LPASS_DEV);
	return (-1);
}
#endif

#ifndef FMAUDIO_TEST
static int
apr_send(void *ctx, const uint8_t *pkt, size_t len)
{
	int fd = *(int *)ctx;

	if (fma_verbose)
		fma_log("apr: tx svc %u opcode %#x token %#x (%zu bytes)", pkt[8],
		    le32dec_(pkt + 16), le32dec_(pkt + 12), len);
	if (write(fd, pkt, len) != (ssize_t)len) {
		fma_log("apr: %s", strerror(errno));
		return (-1);
	}
	return (0);
}

static int
apr_recv(void *ctx, uint8_t *buf, size_t size, int timeout_ms)
{
	struct pollfd pfd = { .fd = *(int *)ctx, .events = POLLIN };
	ssize_t n;
	int r;

	if ((r = poll(&pfd, 1, timeout_ms)) <= 0)
		return (r);
	if ((n = read(pfd.fd, buf, size)) < 0)
		return (-1);
	if (fma_verbose && n >= APR_HDR_SIZE)
		fma_log("apr: rx svc %u opcode %#x token %#x (%zd bytes)", buf[4],
		    le32dec_(buf + 16), le32dec_(buf + 12), n);
	return ((int)n);
}
#endif

/* --- the path ------------------------------------------------------------ */

struct path {
	struct slimqmi	qmi;
	struct mmio	ngdw;
	struct ngd	ngd;
	struct slim_fm	fm;
	int		aprfd;
	struct apr_io	io;
	struct afe	afe;
	/* what is on, for the way back */
	bool		tx_started, rx_started, loopback, clocks;
	bool		voted;
	uint32_t	dcodec_handle;
};

static int
slim_up(struct path *p)
{
	struct slim_ea ea = { 0, 0, WCN3950_PROD_CODE, WCN3950_MANF_ID };
	uint64_t until;
	uint8_t rev[2];
	int r;

	if (qmi_up(&p->qmi) != 0)
		return (-1);
	/* only now: the bus is powered (qcom_slim_ngd_power_up) */
	if (map_window(&p->ngdw, "slim-ngd", NGD_PA, NGD_LEN) != 0)
		return (-1);
	p->ngd.m = &p->ngdw;
	if (ngd_power_up(&p->ngd) != 0) {
		fma_log("slim: the NGD did not come up");
		return (-1);
	}
	/* qcom_slim_ngd_notify_slaves, btfm_slim_hw_init: generic, interface */
	until = fma_now_ms() + (uint64_t)opt.wait_s * 1000;
	for (;;) {
		ea.dev_index = 1;
		r = ngd_get_laddr(&p->ngd, &ea, &p->fm.pgd_la);
		if (r == 0) {
			ea.dev_index = 0;
			r = ngd_get_laddr(&p->ngd, &ea, &p->fm.ifd_la);
		}
		if (r == 0 || fma_now_ms() >= until)
			break;
		fma_sleep_ms(100);
	}
	if (r != 0) {
		fma_log("slim: the WCN3950 (%03x:%03x) is not on the bus: %s "
		    "(is the FM receiver on?)", WCN3950_MANF_ID,
		    WCN3950_PROD_CODE, strerror(-r));
		ngd_dump(&p->ngd, "without the WCN3950");
		return (-1);
	}
	fma_log("slim: WCN3950 generic device at LA %#x, interface device at "
	    "LA %#x", p->fm.pgd_la, p->fm.ifd_la);
	if (ngd_read_ve(&p->ngd, p->fm.ifd_la, WCN399X_HW_REV_MSB, &rev[0],
	    1) == 0 && ngd_read_ve(&p->ngd, p->fm.ifd_la, WCN399X_HW_REV_LSB,
	    &rev[1], 1) == 0)
		fma_log("slim: WCN3950 SLIMbus slave revision %u.%u step %u",
		    rev[0] >> 4, rev[0] & 0xf, rev[1]);
	else
		fma_log("slim: the WCN3950's revision registers do not answer");
	return (0);
}

#ifdef FMAUDIO_TEST
static int apr_attach(struct path *p);
#else
static int
apr_attach(struct path *p)
{
	if ((p->aprfd = open(APR_DEV, O_RDWR)) < 0) {
		fma_log("apr: %s: %s (kldload qcom_apr; the ADSP running; "
		    "pro1x-q6 not running)", APR_DEV, strerror(errno));
		return (-1);
	}
	p->io.send = apr_send;
	p->io.recv = apr_recv;
	p->io.ctx = &p->aprfd;
	return (0);
}
#endif

static int
apr_up(struct path *p)
{
	if (p->aprfd >= 0)
		return (0);
	if (apr_attach(p) != 0)
		return (-1);
	p->afe.io = &p->io;
	p->afe.timeout_ms = 3000;	/* q6afe.c TIMEOUT_MS */
	return (adsp_ready(&p->afe, 10000) == 0 ? 0 : -1);
}

/*
 * The clocks of the output path, as the macros' probes take them (RX
 * macro: mclk, npl at 19.2 MHz and the dcodec vote; the VA macro's mclk,
 * which on SM6115 is TX_CORE_MCLK, at 2 x 9.6 MHz, with its npl), each
 * through q6afe-clocks.
 */
static const uint32_t out_clocks[] = {
	Q6AFE_LPASS_CLK_ID_TX_CORE_MCLK, Q6AFE_LPASS_CLK_ID_TX_CORE_NPL_MCLK,
	Q6AFE_LPASS_CLK_ID_RX_CORE_MCLK, Q6AFE_LPASS_CLK_ID_RX_CORE_NPL_MCLK,
};

static int
clocks_on(struct path *p)
{
	unsigned i;

	if (afe_vote(&p->afe, Q6AFE_LPASS_CORE_HW_DCODEC_BLOCK,
	    "LPASS_HW_DCODEC", &p->dcodec_handle) != 0)
		return (-1);
	p->voted = true;
	for (i = 0; i < sizeof(out_clocks) / sizeof(out_clocks[0]); i++)
		if (afe_clock(&p->afe, out_clocks[i], MCLK_FREQ) != 0)
			return (-1);
	p->clocks = true;
	fma_log("afe: the output path's clocks on");
	return (0);
}

static void
clocks_off(struct path *p)
{
	unsigned i;

	if (p->clocks)
		for (i = sizeof(out_clocks) / sizeof(out_clocks[0]); i-- > 0; )
			afe_clock(&p->afe, out_clocks[i], 0);
	p->clocks = false;
	if (p->voted)
		afe_unvote(&p->afe, Q6AFE_LPASS_CORE_HW_DCODEC_BLOCK,
		    p->dcodec_handle);
	p->voted = false;
}

/*
 * Android's "play-fm" (the Pro1X's mixer_paths.xml, the downstream
 * msm-pcm-routing and the FM HAL's order): the loopback's gain and the
 * loopback on SLIMBUS_8_TX towards RX_CODEC_DMA_RX_0, the RX port, then
 * the TX port; the SLIMbus channels are active before the TX port starts
 * (the downstream BE's codec DAI, btfm_slim, prepares before the CPU DAI).
 */
static int
afe_up(struct path *p)
{
	uint8_t cfg[AFE_PORT_CONFIG_SIZE];

	if (apr_up(p) != 0)
		return (-1);
	if (!opt.no_output) {
		if (clocks_on(p) != 0)
			return (-1);
		if (afe_loopback_gain(&p->afe, AFE_PORT_ID_SLIMBUS_MULTI_CHAN_8_TX,
		    AFE_TOKEN_SLIMBUS_8_TX, opt.gain) != 0)
			return (-1);
		if (afe_loopback(&p->afe, AFE_PORT_ID_SLIMBUS_MULTI_CHAN_8_TX,
		    AFE_TOKEN_SLIMBUS_8_TX, AFE_PORT_ID_RX_CODEC_DMA_RX_0,
		    true) != 0)
			return (-1);
		p->loopback = true;
		afe_cfg_cdc_dma(cfg, p->fm.rate, p->fm.bps, 2);
		if (afe_port_start(&p->afe, AFE_PORT_ID_RX_CODEC_DMA_RX_0,
		    AFE_TOKEN_RX_CODEC_DMA_RX_0, AFE_PARAM_ID_CODEC_DMA_CONFIG,
		    cfg) != 0)
			return (-1);
		p->rx_started = true;
	}
	afe_cfg_slimbus(cfg, p->fm.rate, p->fm.bps, p->fm.nports, p->fm.ch);
	if (afe_port_start(&p->afe, AFE_PORT_ID_SLIMBUS_MULTI_CHAN_8_TX,
	    AFE_TOKEN_SLIMBUS_8_TX, AFE_PARAM_ID_SLIMBUS_CONFIG, cfg) != 0)
		return (-1);
	p->tx_started = true;
	return (0);
}

static void
path_down(struct path *p)
{
	if (p->tx_started)
		afe_port_stop(&p->afe, AFE_PORT_ID_SLIMBUS_MULTI_CHAN_8_TX,
		    AFE_TOKEN_SLIMBUS_8_TX);
	p->tx_started = false;
	if (p->loopback)
		afe_loopback(&p->afe, AFE_PORT_ID_SLIMBUS_MULTI_CHAN_8_TX,
		    AFE_TOKEN_SLIMBUS_8_TX, AFE_PORT_ID_RX_CODEC_DMA_RX_0, false);
	p->loopback = false;
	if (p->rx_started)
		afe_port_stop(&p->afe, AFE_PORT_ID_RX_CODEC_DMA_RX_0,
		    AFE_TOKEN_RX_CODEC_DMA_RX_0);
	p->rx_started = false;
	clocks_off(p);
	if (p->ngd.up) {
		fm_stream_disable(&p->ngd, &p->fm);
		if (p->fm.ports_on)
			fm_ports(&p->ngd, &p->fm, false);
		ngd_dump(&p->ngd, "at the end");
		fma_log("slim: %u sent, %u received, %u NACKed, %u errors, "
		    "%u dropped", p->ngd.tx, p->ngd.rx, p->ngd.nacks,
		    p->ngd.errors, p->ngd.dropped);
	}
	qmi_down(&p->qmi);
	if (p->aprfd >= 0)
		close(p->aprfd);
	p->aprfd = -1;
}

static int
stream_up(struct path *p)
{
	if (fm_ports(&p->ngd, &p->fm, true) != 0)
		return (-1);
	return (fm_stream_enable(&p->ngd, &p->fm) == 0 ? 0 : -1);
}

/* hold: messages from the bus and the ADSP are logged */
static void
hold(struct path *p, int seconds)
{
	uint64_t until = fma_now_ms() + (uint64_t)seconds * 1000;
	uint8_t buf[4096];
	uint32_t status, last = 0;
	int n;

	while (!stop && (seconds < 0 || fma_now_ms() < until)) {
		if (p->aprfd >= 0 && (n = p->io.recv(p->io.ctx, buf, sizeof(buf),
		    200)) > 0 && n >= APR_HDR_SIZE)
			fma_log("apr: event from service %u: opcode %#x token "
			    "%#x (%d bytes)", buf[4], le32dec_(buf + 16),
			    le32dec_(buf + 12), n);
		else if (p->aprfd < 0)
			fma_sleep_ms(200);
		if (p->ngd.up) {
			status = mmio_rd(&p->ngdw, NGD_BASE + NGD_STATUS);
			if (status != last)
				fma_log("slim: NGD status %#x", status);
			last = status;
		}
	}
}

static int
parse_pair(const char *s, uint8_t *v)
{
	char *e;
	unsigned long a, b;

	a = strtoul(s, &e, 0);
	if (*e != ',')
		return (-1);
	b = strtoul(e + 1, &e, 0);
	if (*e != '\0' || a > 255 || b > 255)
		return (-1);
	v[0] = (uint8_t)a;
	v[1] = (uint8_t)b;
	return (0);
}

static void
usage(void)
{
	fprintf(stderr, "usage: pro1x-fmaudio [-vTN] [-t sec] [-w sec] "
	    "[-g gain] [-c ch,ch] [-p port,port] [-M ms]\n"
	    "                     probe | slim | afe | clocks | regs | start\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	struct path p;
	const char *cmd;
	int c, rc = 1;

	setvbuf(stdout, NULL, _IOLBF, 0);
	memset(&p, 0, sizeof(p));
	p.qmi.s = -1;
	p.aprfd = -1;
	p.fm.rate = 48000;	/* downstream bengal.c SLIM_TX_8: 48 kHz, S16, 2 */
	p.fm.bps = 16;
	p.fm.nports = 2;
	p.fm.port[0] = 1;	/* SLAVE_SB_PGD_PORT_TX1_FM */
	p.fm.port[1] = 2;
	p.fm.ch[0] = 160;	/* wcn399x: 160, 161 (downstream 159-161) */
	p.fm.ch[1] = 161;
	while ((c = getopt(argc, argv, "vTNt:w:g:c:p:M:")) != -1) {
		switch (c) {
		case 'v':
			fma_verbose = 1;
			break;
		case 'T':
			p.ngdw.trace = 1;
			break;
		case 'N':
			opt.no_output = true;
			break;
		case 't':
			opt.hold_s = atoi(optarg);
			break;
		case 'w':
			opt.wait_s = atoi(optarg);
			break;
		case 'g':
			opt.gain = (uint16_t)strtoul(optarg, NULL, 0);
			break;
		case 'M':
			p.ngd.timeout_ms = (unsigned)atoi(optarg);
			break;
		case 'c':
			if (parse_pair(optarg, p.fm.ch) != 0)
				usage();
			break;
		case 'p':
			if (parse_pair(optarg, p.fm.port) != 0)
				usage();
			break;
		default:
			usage();
		}
	}
	if (optind + 1 != argc)
		usage();
	cmd = argv[optind];
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGHUP, on_signal);
	signal(SIGPIPE, SIG_IGN);

	if (strcmp(cmd, "regs") == 0) {
		if (qmi_up(&p.qmi) == 0 &&
		    map_window(&p.ngdw, "slim-ngd", NGD_PA, NGD_LEN) == 0) {
			p.ngd.m = &p.ngdw;
			ngd_dump(&p.ngd, "now");
			rc = 0;
		}
		qmi_down(&p.qmi);
		return (rc);
	}
	if (strcmp(cmd, "clocks") == 0) {
		if (apr_up(&p) == 0 && clocks_on(&p) == 0) {
			hold(&p, opt.hold_s);
			rc = 0;
		}
		path_down(&p);
		return (rc);
	}
	if (strcmp(cmd, "probe") != 0 && strcmp(cmd, "slim") != 0 &&
	    strcmp(cmd, "afe") != 0 && strcmp(cmd, "start") != 0)
		usage();

	if (slim_up(&p) != 0) {
		printf("fmaudio: error slimbus\n");
		goto out;
	}
	if (strcmp(cmd, "probe") == 0) {
		rc = 0;
		goto out;
	}
	if (stream_up(&p) != 0) {
		printf("fmaudio: error slimbus stream\n");
		goto out;
	}
	if (strcmp(cmd, "slim") == 0) {
		hold(&p, opt.hold_s);
		rc = 0;
		goto out;
	}
	if (afe_up(&p) != 0) {
		printf("fmaudio: error afe\n");
		goto out;
	}
	if (strcmp(cmd, "start") == 0) {
		printf("fmaudio: ready%s\n", opt.no_output ? " (no output)" :
		    " (no codec path yet: RX_CODEC_DMA_RX_0 feeds nothing)");
		hold(&p, -1);
	} else
		hold(&p, opt.hold_s);
	rc = 0;
out:
	path_down(&p);
	fma_log("fmaudio: %s %s", cmd, rc == 0 ? "done" : "failed");
	return (rc);
}
