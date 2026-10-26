/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * GLINK over SMEM (Linux's qcom_glink_smem.c): the edge to a remote
 * processor, its "glink-edge" child node, set up once the processor reports
 * ready (qcom_pas.c) and torn down when it stops. Our FIFO is SMEM item 479
 * (16 KB, allocated here), the remote's 480; item 478 holds the four
 * indices (our tail and head, then the remote's). The remote's interrupt
 * (the node's) runs glink_edge_rx in a task; ours is an APCS doorbell.
 *
 * Users of a channel register by channel name (qcom_glink_register): when a
 * remote processor opens such a channel, the user's callback runs in a task
 * queue of its own, where it may open the channel and wait (the edges'
 * receive tasks, which bring the acknowledgements, run in the other).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/rman.h>
#include <sys/taskqueue.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <machine/atomic.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "glink.h"
#include "qcom_dsp.h"

#define	SMEM_GLINK_DESCRIPTOR	478
#define	SMEM_GLINK_FIFO_0	479
#define	SMEM_GLINK_FIFO_1	480
#define	TX_FIFO_SIZE		16384
#define	FIFO_FULL_RESERVE	8
#define	TX_BLOCKED_RESERVE	8
#define	IRQ_RID			16

struct qcom_glink_edge {
	LIST_ENTRY(qcom_glink_edge) link;
	device_t	dev;		/* the remote processor's */
	phandle_t	node;		/* glink-edge */
	char		label[32];
	u_int		remote_pid;
	phandle_t	ipc_xref;
	u_int		ipc_bit;
	struct resource	*irq;
	void		*ih;
	struct task	rx_task;
	volatile uint32_t *desc;	/* tx tail, tx head, rx tail, rx head */
	char		*tx_fifo, *rx_fifo;
	size_t		tx_len, rx_len;
	struct glink_edge *glink;
};

struct glink_user {
	LIST_ENTRY(glink_user) link;
	char		channel[32];
	qcom_glink_user_cb_t *cb;
	void		*arg;
};

struct announce_task {
	struct task	task;
	struct qcom_glink_edge *edge;
	char		channel[32];
};

static LIST_HEAD(, qcom_glink_edge) edges = LIST_HEAD_INITIALIZER(edges);
static LIST_HEAD(, glink_user) users = LIST_HEAD_INITIALIZER(users);
static struct mtx glink_list_lock;
MTX_SYSINIT(qcom_glink_list, &glink_list_lock, "qcom_glink list", MTX_DEF);
static struct taskqueue *glink_tq;	/* the edges' receive tasks */
static struct taskqueue *glink_user_tq;	/* the users' announcements */

/* --- the FIFOs in SMEM ------------------------------------------------------------ */

static size_t
smem_rx_avail(void *arg)
{
	struct qcom_glink_edge *e = arg;
	uint32_t head, tail;
	size_t len;
	int error;

	if (e->rx_fifo == NULL) {
		e->rx_fifo = qcom_smem_get(e->remote_pid, SMEM_GLINK_FIFO_1,
		    &len, &error);
		if (e->rx_fifo == NULL)
			return (0);
		e->rx_len = len;
	}
	head = e->desc[3];
	tail = e->desc[2];
	return (head >= tail ? head - tail : e->rx_len - tail + head);
}

static void
smem_rx_peek(void *arg, void *buf, size_t off, size_t len)
{
	struct qcom_glink_edge *e = arg;
	size_t tail, n;

	tail = e->desc[2] + off;
	if (tail >= e->rx_len)
		tail -= e->rx_len;
	n = MIN(len, e->rx_len - tail);
	memcpy(buf, e->rx_fifo + tail, n);
	if (n != len)
		memcpy((char *)buf + n, e->rx_fifo, len - n);
}

static void
smem_rx_advance(void *arg, size_t len)
{
	struct qcom_glink_edge *e = arg;
	size_t tail;

	tail = e->desc[2] + len;
	if (tail >= e->rx_len)
		tail -= e->rx_len;
	e->desc[2] = tail;
}

static size_t
smem_tx_avail(void *arg)
{
	struct qcom_glink_edge *e = arg;
	uint32_t head = e->desc[1], tail = e->desc[0];
	size_t avail;

	avail = tail <= head ? e->tx_len - head + tail : tail - head;
	if (avail < FIFO_FULL_RESERVE + TX_BLOCKED_RESERVE)
		return (0);
	return (avail - FIFO_FULL_RESERVE - TX_BLOCKED_RESERVE);
}

static size_t
smem_write_one(struct qcom_glink_edge *e, size_t head, const void *data,
    size_t len)
{
	size_t n = MIN(len, e->tx_len - head);

	memcpy(e->tx_fifo + head, data, n);
	if (n != len)
		memcpy(e->tx_fifo, (const char *)data + n, len - n);
	head += len;
	if (head >= e->tx_len)
		head -= e->tx_len;
	return (head);
}

static void
smem_tx_write(void *arg, const void *hdr, size_t hlen, const void *data,
    size_t dlen)
{
	struct qcom_glink_edge *e = arg;
	size_t head;

	head = smem_write_one(e, e->desc[1], hdr, hlen);
	if (dlen != 0)
		head = smem_write_one(e, head, data, dlen);
	head = roundup2(head, 8);
	if (head >= e->tx_len)
		head -= e->tx_len;
	/* the data before the head that publishes it */
	wmb();
	e->desc[1] = head;
}

static void
smem_tx_kick(void *arg)
{
	struct qcom_glink_edge *e = arg;

	qcom_ipc_kick(e->ipc_xref, e->ipc_bit);
}

static size_t
smem_tx_length(void *arg)
{
	return (((struct qcom_glink_edge *)arg)->tx_len);
}

static const struct glink_fifo_ops smem_ops = {
	smem_rx_avail, smem_rx_peek, smem_rx_advance, smem_tx_avail,
	smem_tx_write, smem_tx_kick, smem_tx_length
};

/* --- interrupts and announcements -------------------------------------------------- */

static void
edge_rx_task(void *arg, int pending)
{
	struct qcom_glink_edge *e = arg;

	glink_edge_rx(e->glink);
}

static void
edge_intr(void *arg)
{
	struct qcom_glink_edge *e = arg;

	taskqueue_enqueue(glink_tq, &e->rx_task);
}

static void
announce_task(void *arg, int pending)
{
	struct announce_task *at = arg;
	struct glink_user *u;
	qcom_glink_user_cb_t *cb = NULL;
	void *cbarg = NULL;

	mtx_lock(&glink_list_lock);
	LIST_FOREACH(u, &users, link)
		if (strcmp(u->channel, at->channel) == 0) {
			cb = u->cb;
			cbarg = u->arg;
			break;
		}
	mtx_unlock(&glink_list_lock);
	if (cb != NULL)
		cb(cbarg, at->edge->glink, at->edge->label, at->channel);
	free(at, M_GLINK);
}

/* From the receive context: the user's part runs in the task queue */
static void
edge_announce(void *arg, const char *channel)
{
	struct qcom_glink_edge *e = arg;
	struct announce_task *at;

	at = malloc(sizeof(*at), M_GLINK, M_NOWAIT | M_ZERO);
	if (at == NULL)
		return;
	at->edge = e;
	strlcpy(at->channel, channel, sizeof(at->channel));
	TASK_INIT(&at->task, 0, announce_task, at);
	taskqueue_enqueue(glink_user_tq, &at->task);
}

void
qcom_glink_register(const char *channel, qcom_glink_user_cb_t *cb, void *arg)
{
	struct glink_user *u;

	u = malloc(sizeof(*u), M_GLINK, M_WAITOK | M_ZERO);
	strlcpy(u->channel, channel, sizeof(u->channel));
	u->cb = cb;
	u->arg = arg;
	mtx_lock(&glink_list_lock);
	LIST_INSERT_HEAD(&users, u, link);
	mtx_unlock(&glink_list_lock);
}

void
qcom_glink_unregister(const char *channel)
{
	struct glink_user *u;

	mtx_lock(&glink_list_lock);
	LIST_FOREACH(u, &users, link)
		if (strcmp(u->channel, channel) == 0) {
			LIST_REMOVE(u, link);
			break;
		}
	mtx_unlock(&glink_list_lock);
	/* an announcement may be running with it */
	if (glink_user_tq != NULL)
		taskqueue_drain_all(glink_user_tq);
	free(u, M_GLINK);
}

struct glink_edge *
qcom_glink_find(const char *label)
{
	struct qcom_glink_edge *e;
	struct glink_edge *g = NULL;

	mtx_lock(&glink_list_lock);
	LIST_FOREACH(e, &edges, link)
		if (strcmp(e->label, label) == 0) {
			g = e->glink;
			break;
		}
	mtx_unlock(&glink_list_lock);
	return (g);
}

/* --- the edge of a remote processor ------------------------------------------------ */

static phandle_t
edge_node(phandle_t rproc)
{
	phandle_t child;
	char name[32];

	for (child = OF_child(rproc); child != 0; child = OF_peer(child)) {
		if (OF_getprop(child, "name", name, sizeof(name)) > 0 &&
		    strcmp(name, "glink-edge") == 0)
			return (child);
	}
	return (0);
}

int
qcom_glink_edge_start(device_t dev, phandle_t rproc, void **cookie)
{
	struct qcom_glink_edge *e;
	pcell_t cells[3];
	phandle_t iparent;
	size_t size;
	int error, irq, rid, ncells;

	if (glink_tq == NULL) {
		glink_tq = taskqueue_create("qcom_glink", M_WAITOK,
		    taskqueue_thread_enqueue, &glink_tq);
		taskqueue_start_threads(&glink_tq, 1, PI_NET, "qcom_glink");
		glink_user_tq = taskqueue_create("qcom_glink_user", M_WAITOK,
		    taskqueue_thread_enqueue, &glink_user_tq);
		taskqueue_start_threads(&glink_user_tq, 1, PI_NET,
		    "qcom_glink_user");
	}
	e = malloc(sizeof(*e), M_GLINK, M_WAITOK | M_ZERO);
	e->dev = dev;
	if ((e->node = edge_node(rproc)) == 0) {
		error = ENXIO;
		goto fail;
	}
	if (OF_getprop(e->node, "label", e->label, sizeof(e->label) - 1) <= 0 ||
	    OF_getencprop(e->node, "qcom,remote-pid", &e->remote_pid,
	    sizeof(e->remote_pid)) <= 0 ||
	    (error = qcom_ipc_get(e->node, 0, &e->ipc_xref, &e->ipc_bit)) != 0) {
		device_printf(dev, "glink-edge: label, remote-pid or mboxes missing\n");
		error = ENXIO;
		goto fail;
	}

	error = qcom_smem_alloc(e->remote_pid, SMEM_GLINK_DESCRIPTOR, 32);
	if (error != 0 && error != EEXIST)
		goto fail;
	e->desc = qcom_smem_get(e->remote_pid, SMEM_GLINK_DESCRIPTOR, &size,
	    &error);
	if (e->desc == NULL || size != 32) {
		device_printf(dev, "glink: descriptor %d\n", error);
		error = EINVAL;
		goto fail;
	}
	error = qcom_smem_alloc(e->remote_pid, SMEM_GLINK_FIFO_0, TX_FIFO_SIZE);
	if (error != 0 && error != EEXIST)
		goto fail;
	e->tx_fifo = qcom_smem_get(e->remote_pid, SMEM_GLINK_FIFO_0, &e->tx_len,
	    &error);
	if (e->tx_fifo == NULL)
		goto fail;
	/* our sides of both FIFOs start empty */
	e->desc[2] = 0;
	e->desc[1] = 0;

	/* the remote's doorbell: the edge node's interrupt, at the GIC */
	iparent = ofw_bus_find_iparent(e->node);
	if (OF_getencprop(OF_node_from_xref(iparent), "#interrupt-cells", &ncells,
	    sizeof(ncells)) <= 0 || ncells > 3 ||
	    OF_getencprop(e->node, "interrupts", cells, ncells * sizeof(pcell_t)) !=
	    (ssize_t)(ncells * sizeof(pcell_t))) {
		error = ENXIO;
		goto fail;
	}
	irq = ofw_bus_map_intr(dev, iparent, ncells, cells);
	rid = IRQ_RID;
	bus_set_resource(dev, SYS_RES_IRQ, rid, irq, 1);
	e->irq = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid, RF_ACTIVE);
	if (e->irq == NULL) {
		error = ENXIO;
		goto fail;
	}
	TASK_INIT(&e->rx_task, 0, edge_rx_task, e);
	e->glink = glink_edge_create(e->label, &smem_ops, e, edge_announce, e);
	error = bus_setup_intr(dev, e->irq, INTR_TYPE_MISC | INTR_MPSAFE, NULL,
	    edge_intr, e, &e->ih);
	if (error != 0)
		goto fail;
	mtx_lock(&glink_list_lock);
	LIST_INSERT_HEAD(&edges, e, link);
	mtx_unlock(&glink_list_lock);
	/* what came before the interrupt was set up */
	taskqueue_enqueue(glink_tq, &e->rx_task);
	device_printf(dev, "glink edge %s up (remote pid %u, FIFO %zu bytes)\n",
	    e->label, e->remote_pid, e->tx_len);
	*cookie = e;
	return (0);
fail:
	device_printf(dev, "glink edge: %d\n", error);
	if (e->glink != NULL) {
		glink_edge_abort(e->glink);
		glink_edge_destroy(e->glink);
	}
	if (e->irq != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, IRQ_RID, e->irq);
	free(e, M_GLINK);
	return (error != 0 ? error : ENXIO);
}

/* The remote processor stopped or crashed. Its channels' users get a down. */
void
qcom_glink_edge_stop(void *cookie)
{
	struct qcom_glink_edge *e = cookie;

	if (e == NULL)
		return;
	mtx_lock(&glink_list_lock);
	LIST_REMOVE(e, link);
	mtx_unlock(&glink_list_lock);
	bus_teardown_intr(e->dev, e->irq, e->ih);
	bus_release_resource(e->dev, SYS_RES_IRQ, IRQ_RID, e->irq);
	glink_edge_abort(e->glink);
	taskqueue_drain_all(glink_tq);
	/* announcements of this edge: their opens fail now, they end */
	taskqueue_drain_all(glink_user_tq);
	/* users close their channels on the down; what is left goes */
	glink_edge_destroy(e->glink);
	free(e, M_GLINK);
}
