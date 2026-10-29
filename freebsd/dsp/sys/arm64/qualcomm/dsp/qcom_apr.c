/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * APR, the packet router of the ADSP's audio services (Linux's
 * drivers/soc/qcom/apr.c transport), for user space: the ADSP opens the
 * GLINK channel "apr_audio_svc" on its edge ("lpass"), and /dev/apr carries
 * whole APR packets (a 20-byte header: hdr_field, pkt_size, source and
 * destination service, domain and port, token, opcode; then the payload)
 * between that channel and one process: write(2) one packet, read(2) the
 * next one received. The services (q6core, q6afe, q6asm, q6adm) are the
 * process's business (freebsd/dsp/tools/pro1x-q6). This is the first piece
 * of audio on FreeBSD (freebsd/fm/README.md); nothing is routed or
 * interpreted here.
 *
 *   sysctl hw.qcom_apr     the channel, packets sent, received, dropped
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/poll.h>
#include <sys/queue.h>
#include <sys/selinfo.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <sys/uio.h>

#include "glink.h"
#include "qcom_dsp.h"

#define	APR_CHANNEL		"apr_audio_svc"
#define	APR_EDGE		"lpass"		/* the ADSP's glink-edge */
#define	APR_HDR_SIZE		20	/* sizeof(struct apr_hdr) */
#define	APR_MAX_PKT		4096
#define	APR_QUEUE_MAX		64
#define	APR_INTENTS		8

static MALLOC_DEFINE(M_APR, "qcom_apr", "Qualcomm APR packets");

struct apr_pkt {
	STAILQ_ENTRY(apr_pkt) link;
	size_t		len;
	uint8_t		data[];
};

static struct {
	struct mtx	lock;
	struct glink_channel *ch;
	bool		up, open;
	STAILQ_HEAD(, apr_pkt) q;
	u_int		qlen;
	struct selinfo	sel;
	struct cdev	*cdev;
	u_int		sent, received, dropped;
} apr;

MTX_SYSINIT(qcom_apr_lock, &apr.lock, "qcom_apr", MTX_DEF);

static void
flush_queue(void)
{
	struct apr_pkt *p;

	mtx_assert(&apr.lock, MA_OWNED);
	while ((p = STAILQ_FIRST(&apr.q)) != NULL) {
		STAILQ_REMOVE_HEAD(&apr.q, link);
		free(p, M_APR);
	}
	apr.qlen = 0;
}

/* GLINK's receive context: copy, queue, wake */
static void
apr_rx(void *arg, const void *data, size_t len)
{
	struct apr_pkt *p;

	if (len < APR_HDR_SIZE || len > APR_MAX_PKT) {
		apr.dropped++;
		return;
	}
	p = malloc(sizeof(*p) + len, M_APR, M_NOWAIT);
	if (p == NULL) {
		apr.dropped++;
		return;
	}
	p->len = len;
	memcpy(p->data, data, len);
	mtx_lock(&apr.lock);
	apr.received++;
	if (!apr.open || apr.qlen >= APR_QUEUE_MAX) {
		apr.dropped++;
		mtx_unlock(&apr.lock);
		free(p, M_APR);
		return;
	}
	STAILQ_INSERT_TAIL(&apr.q, p, link);
	apr.qlen++;
	wakeup(&apr.q);
	selwakeup(&apr.sel);
	mtx_unlock(&apr.lock);
}

static void
apr_state(void *arg, bool up)
{
	if (up)
		return;
	/* the ADSP is gone: readers fail (no GLINK call from here) */
	mtx_lock(&apr.lock);
	apr.up = false;
	wakeup(&apr.q);
	selwakeup(&apr.sel);
	mtx_unlock(&apr.lock);
}

static d_open_t apr_open;
static d_close_t apr_close;
static d_read_t apr_read;
static d_write_t apr_write;
static d_poll_t apr_poll;

static struct cdevsw apr_cdevsw = {
	.d_version =	D_VERSION,
	.d_name =	"apr",
	.d_open =	apr_open,
	.d_close =	apr_close,
	.d_read =	apr_read,
	.d_write =	apr_write,
	.d_poll =	apr_poll,
};

static int
apr_open(struct cdev *dev, int oflags, int devtype, struct thread *td)
{
	int error = 0;

	mtx_lock(&apr.lock);
	if (apr.open)
		error = EBUSY;
	else {
		apr.open = true;
		flush_queue();
	}
	mtx_unlock(&apr.lock);
	return (error);
}

static int
apr_close(struct cdev *dev, int fflag, int devtype, struct thread *td)
{
	mtx_lock(&apr.lock);
	apr.open = false;
	flush_queue();
	mtx_unlock(&apr.lock);
	return (0);
}

static int
apr_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct apr_pkt *p;
	int error = 0;

	mtx_lock(&apr.lock);
	while ((p = STAILQ_FIRST(&apr.q)) == NULL) {
		if (!apr.up) {
			mtx_unlock(&apr.lock);
			return (ENXIO);
		}
		if (ioflag & O_NONBLOCK) {
			mtx_unlock(&apr.lock);
			return (EWOULDBLOCK);
		}
		error = msleep(&apr.q, &apr.lock, PCATCH, "aprrd", 0);
		if (error != 0) {
			mtx_unlock(&apr.lock);
			return (error);
		}
	}
	STAILQ_REMOVE_HEAD(&apr.q, link);
	apr.qlen--;
	mtx_unlock(&apr.lock);
	/* one packet per read; a short buffer gets its start */
	error = uiomove(p->data, MIN(p->len, uio->uio_resid), uio);
	free(p, M_APR);
	return (error);
}

static int
apr_write(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct glink_channel *ch;
	uint8_t *buf;
	size_t len = uio->uio_resid;
	int error;

	if (len < APR_HDR_SIZE || len > APR_MAX_PKT)
		return (EINVAL);
	buf = malloc(len, M_APR, M_WAITOK);
	error = uiomove(buf, len, uio);
	/* pkt_size (bytes 2-3, little endian) is the whole packet */
	if (error == 0 && (size_t)(buf[2] | buf[3] << 8) != len)
		error = EINVAL;
	if (error == 0) {
		mtx_lock(&apr.lock);
		ch = apr.up ? apr.ch : NULL;
		mtx_unlock(&apr.lock);
		error = ch == NULL ? ENXIO : glink_send(ch, buf, len);
		if (error == 0) {
			mtx_lock(&apr.lock);
			apr.sent++;
			mtx_unlock(&apr.lock);
		}
	}
	free(buf, M_APR);
	return (error);
}

static int
apr_poll(struct cdev *dev, int events, struct thread *td)
{
	int revents = 0;

	mtx_lock(&apr.lock);
	if (events & (POLLIN | POLLRDNORM)) {
		if (!STAILQ_EMPTY(&apr.q) || !apr.up)
			revents |= events & (POLLIN | POLLRDNORM);
		else
			selrecord(td, &apr.sel);
	}
	if (events & (POLLOUT | POLLWRNORM))
		revents |= events & (POLLOUT | POLLWRNORM);
	mtx_unlock(&apr.lock);
	return (revents);
}

/* The ADSP opened apr_audio_svc (the GLINK users' task queue) */
static void
apr_announced(void *arg, struct glink_edge *edge, const char *label,
    const char *channel)
{
	struct glink_channel *ch;
	int error;

	if (strcmp(label, APR_EDGE) != 0) {
		printf("qcom_apr: %s on edge %s, not the ADSP's: ignored\n",
		    channel, label);
		return;
	}
	mtx_lock(&apr.lock);
	if (apr.ch != NULL) {
		/* the ADSP started again: the old channel is closed first */
		ch = apr.ch;
		apr.ch = NULL;
		mtx_unlock(&apr.lock);
		glink_close(ch);
	} else
		mtx_unlock(&apr.lock);
	error = glink_open(edge, channel, APR_INTENTS, APR_MAX_PKT, apr_rx,
	    apr_state, NULL, 5000, &ch);
	if (error != 0) {
		printf("qcom_apr: open %s: %d\n", channel, error);
		return;
	}
	mtx_lock(&apr.lock);
	apr.ch = ch;
	apr.up = true;
	mtx_unlock(&apr.lock);
	printf("qcom_apr: %s open on %s, /dev/apr\n", channel, label);
}

/*
 * Loaded after the ADSP came up: its open of the channel was announced to
 * nobody, so ours goes now (GLINK keeps the remote's open).
 */
static struct task apr_late_task;

static void
apr_late_open(void *arg, int pending)
{
	struct glink_edge *edge;

	if (apr.ch != NULL || (edge = qcom_glink_find(APR_EDGE)) == NULL)
		return;
	apr_announced(NULL, edge, APR_EDGE, APR_CHANNEL);
}

static int
apr_channel_sysctl(SYSCTL_HANDLER_ARGS)
{
	char buf[96];

	mtx_lock(&apr.lock);
	snprintf(buf, sizeof(buf), "%s, %u sent, %u received, %u dropped, "
	    "%u queued", apr.up ? "open" : apr.ch != NULL ? "closed by the "
	    "ADSP" : "not opened by the ADSP yet", apr.sent, apr.received,
	    apr.dropped, apr.qlen);
	mtx_unlock(&apr.lock);
	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}
SYSCTL_PROC(_hw, OID_AUTO, qcom_apr, CTLTYPE_STRING | CTLFLAG_RD |
    CTLFLAG_MPSAFE, NULL, 0, apr_channel_sysctl, "A",
    "The ADSP's APR channel (freebsd/dsp)");

static int
qcom_apr_modevent(module_t mod, int what, void *arg)
{
	switch (what) {
	case MOD_LOAD:
		STAILQ_INIT(&apr.q);
		knlist_init_mtx(&apr.sel.si_note, &apr.lock);
		apr.cdev = make_dev(&apr_cdevsw, 0, UID_ROOT, GID_WHEEL, 0660,
		    "apr");
		qcom_glink_register(APR_CHANNEL, apr_announced, NULL);
		/* the ADSP may have opened it before we loaded */
		TASK_INIT(&apr_late_task, 0, apr_late_open, NULL);
		taskqueue_enqueue(taskqueue_thread, &apr_late_task);
		return (0);
	case MOD_UNLOAD:
		/* GLINK keeps our callbacks: stay */
		return (EBUSY);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t qcom_apr_mod = {
	"qcom_apr",
	qcom_apr_modevent,
	NULL
};

DECLARE_MODULE(qcom_apr, qcom_apr_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(qcom_apr, 1);
MODULE_DEPEND(qcom_apr, qcom_dsp, 1, 1, 1);
