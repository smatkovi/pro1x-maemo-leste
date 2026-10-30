/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * QRTR over GLINK (Linux's net/qrtr/smd.c): every remote processor that
 * opens the channel "IPCRTR" becomes a QRTR endpoint. What the router hands
 * the endpoint is queued and sent by the endpoint's own task, since GLINK
 * may wait (for FIFO space, for an intent) and the router must not. When the
 * processor goes away, the endpoint goes in a task too (GLINK's down
 * callback runs with the edge locked).
 *
 * The endpoint is registered before the channel opens: the remote sends
 * its HELLO as soon as it has one of our intents, which glink_open offers
 * (Linux registers the endpoint in qcom_smd_qrtr_probe, before its intents
 * go out in qcom_glink_announce_create); packets for the remote wait in
 * the queue until the channel is up. A channel taken over from Linux
 * (adopted) gets a HELLO from our name service, as from a node that
 * started again, so that the remote tells its servers once more.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>

#include "glink.h"
#include "qrtr.h"
#include "qcom_dsp.h"

#define	IPCRTR_INTENTS		5
#define	IPCRTR_INTENT_SIZE	4096
#define	OPEN_TIMEOUT_MS		5000
#define	TXQ_MAX			256

static MALLOC_DEFINE(M_QRTRGL, "qrtr_glink", "QRTR over GLINK");

struct txpkt {
	STAILQ_ENTRY(txpkt) link;
	size_t		len;
	char		data[];
};

struct qrtr_gl_ep {
	LIST_ENTRY(qrtr_gl_ep) link;
	char		label[32];
	struct glink_channel *ch;
	struct qrtr_node *node;
	struct mtx	lock;
	STAILQ_HEAD(, txpkt) txq;
	int		ntx;
	bool		down;
	struct task	tx_task;
	struct task	down_task;
	struct taskqueue *tq;
};

static LIST_HEAD(, qrtr_gl_ep) eps = LIST_HEAD_INITIALIZER(eps);
static struct mtx eps_lock;
MTX_SYSINIT(qrtr_glink_eps, &eps_lock, "qrtr_glink eps", MTX_DEF);

static int
ep_xmit(void *arg, const void *pkt, size_t len)
{
	struct qrtr_gl_ep *ep = arg;
	struct txpkt *t;

	t = malloc(sizeof(*t) + len, M_QRTRGL, M_NOWAIT);
	if (t == NULL)
		return (ENOMEM);
	t->len = len;
	memcpy(t->data, pkt, len);
	mtx_lock(&ep->lock);
	if (ep->down || ep->ntx >= TXQ_MAX) {
		mtx_unlock(&ep->lock);
		free(t, M_QRTRGL);
		return (ep->down ? ENODEV : ENOBUFS);
	}
	STAILQ_INSERT_TAIL(&ep->txq, t, link);
	ep->ntx++;
	mtx_unlock(&ep->lock);
	taskqueue_enqueue(ep->tq, &ep->tx_task);
	return (0);
}

static void
ep_tx_task(void *arg, int pending)
{
	struct qrtr_gl_ep *ep = arg;
	struct txpkt *t;
	int error;

	for (;;) {
		mtx_lock(&ep->lock);
		/* not open yet: they wait */
		if (ep->ch != NULL && (t = STAILQ_FIRST(&ep->txq)) != NULL) {
			STAILQ_REMOVE_HEAD(&ep->txq, link);
			ep->ntx--;
		} else
			t = NULL;
		mtx_unlock(&ep->lock);
		if (t == NULL)
			break;
		error = ep->down ? ENODEV : glink_send(ep->ch, t->data, t->len);
		if (error != 0 && !ep->down)
			printf("qrtr_glink: %s: send: %d\n", ep->label, error);
		free(t, M_QRTRGL);
	}
}

/* GLINK's receive context: the router does not wait */
static void
ep_rx(void *arg, const void *data, size_t len)
{
	struct qrtr_gl_ep *ep = arg;

	if (ep->node != NULL)
		qrtr_endpoint_post(ep->node, data, len);
}

static void
ep_down_task(void *arg, int pending)
{
	struct qrtr_gl_ep *ep = arg;
	struct txpkt *t;

	mtx_lock(&eps_lock);
	LIST_REMOVE(ep, link);
	mtx_unlock(&eps_lock);
	qrtr_endpoint_unregister(ep->node);
	ep->node = NULL;
	taskqueue_drain(ep->tq, &ep->tx_task);
	if (ep->ch != NULL)
		glink_close(ep->ch);
	while ((t = STAILQ_FIRST(&ep->txq)) != NULL) {
		STAILQ_REMOVE_HEAD(&ep->txq, link);
		free(t, M_QRTRGL);
	}
	printf("qrtr_glink: %s gone\n", ep->label);
	/* this runs on the system's task queue, not on ep->tq */
	taskqueue_free(ep->tq);
	mtx_destroy(&ep->lock);
	free(ep, M_QRTRGL);
}

/* With the edge locked: no GLINK call here */
static void
ep_state(void *arg, bool up)
{
	struct qrtr_gl_ep *ep = arg;

	if (up)
		return;
	mtx_lock(&ep->lock);
	ep->down = true;
	mtx_unlock(&ep->lock);
	TASK_INIT(&ep->down_task, 0, ep_down_task, ep);
	taskqueue_enqueue(taskqueue_thread, &ep->down_task);
}

/* A processor opened IPCRTR (the glink users' task queue: we may wait) */
static void
ipcrtr_announced(void *arg, struct glink_edge *edge, const char *label,
    const char *channel)
{
	struct glink_channel *ch;
	struct qrtr_gl_ep *ep;
	struct txpkt *t;
	int error;

	ep = malloc(sizeof(*ep), M_QRTRGL, M_WAITOK | M_ZERO);
	strlcpy(ep->label, label, sizeof(ep->label));
	mtx_init(&ep->lock, "qrtr_glink ep", NULL, MTX_DEF);
	STAILQ_INIT(&ep->txq);
	TASK_INIT(&ep->tx_task, 0, ep_tx_task, ep);
	ep->tq = taskqueue_create("qrtr_glink", M_WAITOK,
	    taskqueue_thread_enqueue, &ep->tq);
	taskqueue_start_threads(&ep->tq, 1, PI_NET, "qrtr %s", label);
	/* before our intents go out: the remote's HELLO may come at once */
	ep->node = qrtr_endpoint_register(ep_xmit, ep);
	error = glink_open(edge, channel, IPCRTR_INTENTS, IPCRTR_INTENT_SIZE,
	    ep_rx, ep_state, ep, OPEN_TIMEOUT_MS, &ch);
	if (error != 0) {
		printf("qrtr_glink: %s: open %s: %d\n", label, channel, error);
		qrtr_endpoint_unregister(ep->node);
		ep->node = NULL;
		taskqueue_drain(ep->tq, &ep->tx_task);
		while ((t = STAILQ_FIRST(&ep->txq)) != NULL) {
			STAILQ_REMOVE_HEAD(&ep->txq, link);
			free(t, M_QRTRGL);
		}
		taskqueue_free(ep->tq);
		mtx_destroy(&ep->lock);
		free(ep, M_QRTRGL);
		return;
	}
	mtx_lock(&ep->lock);
	ep->ch = ch;
	mtx_unlock(&ep->lock);
	mtx_lock(&eps_lock);
	LIST_INSERT_HEAD(&eps, ep, link);
	mtx_unlock(&eps_lock);
	/* what waited for the channel */
	taskqueue_enqueue(ep->tq, &ep->tx_task);
	printf("qrtr_glink: %s up%s\n", label,
	    glink_channel_adopted(ch) ? " (Linux's channel adopted)" : "");
	if (glink_channel_adopted(ch)) {
		/* to the remote's node (found in the FIFOs), not to "any" */
		if (glink_channel_hint(ch) != 0xffffffffu)
			qrtr_endpoint_assign(ep->node, glink_channel_hint(ch));
		qrtr_endpoint_hello(ep->node);
	}
}

void
qrtr_glink_init(void)
{
	qcom_glink_register("IPCRTR", ipcrtr_announced, NULL);
}

void
qrtr_glink_fini(void)
{
	qcom_glink_unregister("IPCRTR");
}
