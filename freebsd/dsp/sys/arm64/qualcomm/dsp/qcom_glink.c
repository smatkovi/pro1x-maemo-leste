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
 * receive tasks, which bring the acknowledgements, run in the other). A
 * user that registers after the remote opened its channel is told then
 * (Linux's driver core binds a late driver to the channel's device).
 *
 * Taking over Linux's edge (qcom_glink_edge_adopt, a kexec from a running
 * Leste with the remote still running): the FIFOs keep their indices (the
 * remote reads and writes on), no VERSION is sent, and IPCRTR's channel ids
 * are found in what the FIFOs still hold behind their heads, as KolibriOS
 * does (docs/pro1x-modem.md there): Linux's lcid in the newest QRTR packet
 * from node 1 in the transmit FIFO, the remote's in the newest one to
 * node 1 in the receive FIFO (else their OPEN commands).
 *
 *   sysctl hw.qcom_glink     each edge: FIFOs, negotiation, channels
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
#include <sys/sbuf.h>
#include <sys/sysctl.h>
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
#define	ADOPT_BOUNCE		(64 * 1024)	/* a QRTR packet at most */
#define	CMD_OPEN		2
#define	CMD_TX_DATA		9

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
	bool		adopted;
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

/* (under the list's lock: an edge that stops leaves the list first) */
static void
edges_reannounce(const char *channel)
{
	struct qcom_glink_edge *e;

	mtx_lock(&glink_list_lock);
	LIST_FOREACH(e, &edges, link)
		glink_edge_reannounce(e->glink, channel);
	mtx_unlock(&glink_list_lock);
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
	/* channels the remotes opened before this user came */
	edges_reannounce(channel);
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

/* --- taking over Linux's edge ------------------------------------------------------ */

static void
ring_peek(const char *fifo, size_t len, size_t at, void *buf, size_t n)
{
	size_t k = MIN(n, len - at);

	memcpy(buf, fifo + at, k);
	if (k != n)
		memcpy((char *)buf + k, fifo, n - k);
}

/*
 * A QRTR packet (v1, or v2 from the remote) filling a chunk of len bytes:
 * from node 1 (to_us false: what Linux sent) or to node 1 (to_us true);
 * *node: the other node.
 */
static bool
looks_qrtr(const uint8_t *p, size_t len, bool to_us, uint32_t *node)
{
	uint32_t v[8];
	uint16_t w[4];
	size_t hl;

	memcpy(v, p, sizeof(v));
	if (v[0] == 1) {
		if (v[1] < 1 || v[1] > 11 || v[4] > 1 ||
		    roundup2((uint64_t)v[5], 4) + 32 != len)
			return (false);
		if (!to_us && v[2] == 1) {
			*node = v[6];
			return (true);
		}
		if (to_us && v[2] != 1 && (v[6] == 1 || v[6] == 0xffffffff)) {
			*node = v[2];
			return (true);
		}
		return (false);
	}
	if (to_us && p[0] == 3 && p[1] >= 1 && p[1] <= 11) {
		hl = 16 + p[3];
		memcpy(w, p + 8, sizeof(w));
		if (hl + roundup2((uint64_t)v[1], 4) != len || w[0] == 1 ||
		    (w[2] != 1 && w[2] != 0xffff))
			return (false);
		*node = w[0];
		return (true);
	}
	return (false);
}

/*
 * IPCRTR's id on one side, from the FIFO's past: walking back from its
 * head, the newest TX_DATA that holds one whole QRTR packet of the right
 * direction and ends before the head; else the newest OPEN of "IPCRTR".
 * -1: none found. *how: what it was.
 */
static int
ring_find_ipcrtr(const char *fifo, size_t len, uint32_t head, bool to_us,
    uint32_t *node, const char **how)
{
	struct {
		uint16_t	cmd, param1;
		uint32_t	param2, chunk, left;
	} __packed h;
	uint8_t q[32];
	char name[8];
	size_t back, at, end;
	int open_id = -1;

	if (fifo == NULL || len < 64 || head >= len || (head & 7) != 0)
		return (-1);
	for (back = 8; back < len; back += 8) {
		at = (head + len - back) % len;
		ring_peek(fifo, len, at, &h, sizeof(h));
		if (h.cmd == CMD_TX_DATA && h.left == 0 && h.chunk >= 32 &&
		    h.chunk <= 16384) {
			end = roundup2(16 + (size_t)h.chunk, 8);
			if (end > back)
				continue;	/* would run past the head */
			ring_peek(fifo, len, (at + 16) % len, q, sizeof(q));
			if (looks_qrtr(q, h.chunk, to_us, node)) {
				*how = "QRTR data";
				return (h.param1);
			}
		} else if (h.cmd == CMD_OPEN && open_id < 0 &&
		    (h.param2 & 0xffff) == 7 && back >= 16) {
			ring_peek(fifo, len, (at + 8) % len, name, sizeof(name));
			if (memcmp(name, "IPCRTR", 7) == 0)
				open_id = h.param1;
		}
	}
	*how = open_id >= 0 ? "OPEN" : "nothing";
	return (open_id);
}

/* --- the edge of a remote processor ------------------------------------------------ */

static int edge_setup(device_t, phandle_t, void **, bool);

int
qcom_glink_edge_start(device_t dev, phandle_t rproc, void **cookie)
{
	return (edge_setup(dev, rproc, cookie, false));
}

int
qcom_glink_edge_adopt(device_t dev, phandle_t rproc, void **cookie)
{
	return (edge_setup(dev, rproc, cookie, true));
}

static int
edge_setup(device_t dev, phandle_t rproc, void **cookie, bool adopt)
{
	struct qcom_glink_edge *e;
	pcell_t cells[3];
	phandle_t iparent;
	size_t size;
	uint32_t tx_node = 0, rx_node = 0;
	const char *tx_how = "", *rx_how = "";
	int error, irq, rid, ncells, lcid = -1, rcid = -1;

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
	e->adopted = adopt;
	if (!adopt) {
		/* our sides of both FIFOs start empty */
		e->desc[2] = 0;
		e->desc[1] = 0;
	} else {
		/* as Linux left them: the remote reads and writes on */
		smem_rx_avail(e);
		device_printf(dev, "glink edge %s: adopting: tx tail %u head %u, "
		    "rx tail %u head %u (%zu bytes unread)\n", e->label,
		    e->desc[0], e->desc[1], e->desc[2], e->desc[3],
		    e->rx_fifo != NULL ? smem_rx_avail(e) : 0);
		if (e->rx_fifo == NULL) {
			device_printf(dev, "glink edge %s: no receive FIFO: "
			    "the remote never ran\n", e->label);
			error = ENXIO;
			goto fail;
		}
		lcid = ring_find_ipcrtr(e->tx_fifo, e->tx_len, e->desc[1], false,
		    &tx_node, &tx_how);
		rcid = ring_find_ipcrtr(e->rx_fifo, e->rx_len, e->desc[3], true,
		    &rx_node, &rx_how);
		device_printf(dev, "glink edge %s: IPCRTR: Linux's id %d (%s, "
		    "to node %u), the remote's %d (%s, from node %u)\n",
		    e->label, lcid, tx_how, tx_node, rcid, rx_how, rx_node);
	}

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
	if (!adopt)
		e->glink = glink_edge_create(e->label, &smem_ops, e,
		    edge_announce, e);
	else {
		e->glink = glink_edge_adopt(e->label, &smem_ops, e,
		    edge_announce, e);
		/* before anything is read: the pending data is IPCRTR's too */
		if (lcid > 0 && rcid > 0 && (error = glink_adopt_channel(e->glink,
		    "IPCRTR", lcid, rcid, ADOPT_BOUNCE)) != 0)
			device_printf(dev, "glink edge %s: IPCRTR not adopted: "
			    "%d\n", e->label, error);
		else if (lcid <= 0 || rcid <= 0)
			device_printf(dev, "glink edge %s: IPCRTR's ids not found: "
			    "not adopted (opened anew when the remote asks)\n",
			    e->label);
	}
	error = bus_setup_intr(dev, e->irq, INTR_TYPE_MISC | INTR_MPSAFE, NULL,
	    edge_intr, e, &e->ih);
	if (error != 0)
		goto fail;
	mtx_lock(&glink_list_lock);
	LIST_INSERT_HEAD(&edges, e, link);
	mtx_unlock(&glink_list_lock);
	/* what came before the interrupt was set up */
	taskqueue_enqueue(glink_tq, &e->rx_task);
	device_printf(dev, "glink edge %s %s (remote pid %u, FIFO %zu bytes)\n",
	    e->label, adopt ? "adopted" : "up", e->remote_pid, e->tx_len);
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

/* --- for the test runs ------------------------------------------------------------- */

static void
channel_line(void *arg, const struct glink_channel_info *ci)
{
	sbuf_printf(arg, "  %-24s lcid %u rcid %u%s%s%s%s intents ours %u "
	    "theirs %u, msgs rx %u tx %u%s\n", ci->name, ci->lcid, ci->rcid,
	    ci->up ? " up" : "", ci->user ? " held" : "",
	    ci->open_acked ? "" : " (our OPEN unacked)",
	    ci->adopted ? " adopted" : "", ci->lintents, ci->rintents,
	    ci->rx_msgs, ci->tx_msgs, ci->bounced != 0 ? " (some via Linux's "
	    "intents)" : "");
}

static void
glink_report_sbuf(struct sbuf *sb)
{
	struct glink_edge_info ei;
	struct qcom_glink_edge *e;

	mtx_lock(&glink_list_lock);
	LIST_FOREACH(e, &edges, link) {
		glink_edge_info(e->glink, &ei, NULL, NULL);
		sbuf_printf(sb, "%s (pid %u)%s: FIFOs tx %zu rx %zu, tx tail %u "
		    "head %u, rx tail %u head %u; remote %s VERSION, features "
		    "%#x%s; msgs rx %u tx %u; %u channels\n", e->label,
		    e->remote_pid, ei.adopted ? " adopted" : "", e->tx_len,
		    e->rx_len, e->desc[0], e->desc[1], e->desc[2], e->desc[3],
		    ei.remote_version ? "sent" : "never sent", ei.features,
		    ei.bad ? ", STOPPED (unknown command)" : "", ei.rx_msgs,
		    ei.tx_msgs, ei.nchannels);
		glink_edge_info(e->glink, &ei, channel_line, sb);
	}
	mtx_unlock(&glink_list_lock);
}

static int
glink_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct sbuf *sb;
	int error;

	/* rendered with the locks held, copied out without them */
	sb = sbuf_new_auto();
	sbuf_printf(sb, "\n");
	glink_report_sbuf(sb);
	if ((error = sbuf_finish(sb)) == 0)
		error = SYSCTL_OUT(req, sbuf_data(sb), sbuf_len(sb) + 1);
	sbuf_delete(sb);
	return (error);
}

SYSCTL_PROC(_hw, OID_AUTO, qcom_glink, CTLTYPE_STRING | CTLFLAG_RD |
    CTLFLAG_MPSAFE, NULL, 0, glink_sysctl, "A",
    "GLINK edges over SMEM: FIFOs, negotiation, channels");
