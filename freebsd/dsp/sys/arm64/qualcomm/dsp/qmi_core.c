/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * QMI's clients and servers over this kernel's QRTR ports (Linux's
 * drivers/soc/qcom/qmi_interface.c, on qrtr.h instead of kernel sockets):
 * a handle is a port; lookups and servers go to the name service; the
 * router's callback, which must not wait, only queues what arrives, and a
 * work item hands it on: new and deleted servers to the handle's ops,
 * responses to the transactions waiting for them, requests and indications
 * to the handlers. Errors are Linux's negative errnos, as its callers expect.
 *
 * Plain C over glink_os.h like qrtr_core.c, tested on the PC
 * (test/qmi_test.c).
 */

#include "glink_os.h"
#include "qrtr.h"
#include "qrtr_sock.h"
#include "qcom_qmi.h"

#ifdef _KERNEL
#include <sys/endian.h>
#else
#include <endian.h>
#endif

#ifndef MAX
#define	MAX(a, b)	((a) > (b) ? (a) : (b))
#endif

/* A packet for the handle, queued by the router's callback */
struct qmi_rx {
	struct qmi_rx	*next;
	uint32_t	node, port;
	size_t		len;
	uint8_t		data[];
};

/* A lookup or a server of this handle, for the name service */
struct qmi_ns_entry {
	struct qmi_ns_entry *next;
	unsigned int	service, version, instance;
};

/* A transaction's state while it waits */
struct qmi_txn_state {
	struct qmi_txn_state *next;
	struct qmi_txn	*txn;
	uint16_t	id;
	bool		done;
	int		result;
};

struct qmi_core {
	struct qmi_handle *qmi;
	struct qrtr_port *port;
	glink_work_t	rx_work;
	glink_mutex_t	lock;		/* all below */
	glink_cond_t	cond;		/* a transaction is done */
	struct qmi_rx	*rx_head, **rx_tail;
	bool		releasing;
	struct qmi_service *services;	/* what the lookups found */
	struct qmi_ns_entry *lookups, *servers;
	struct qmi_txn_state *txns;
	uint16_t	next_txn;
	/* the peer of a connected handle (qmi_connect) */
	bool		connected;
	uint32_t	peer_node, peer_port;
};

static inline struct qmi_core *
core_of(struct qmi_handle *qmi)
{
	return (qmi->priv);
}

/* --- receiving ---------------------------------------------------------------------- */

static void
qmi_rx_cb(void *arg, const struct qrtr_rx_info *info, const void *data,
    size_t len)
{
	struct qmi_core *c = arg;
	struct qmi_rx *rx;

	rx = glink_zalloc_nowait(sizeof(*rx) + len);
	if (rx == NULL) {
		glink_log("qmi: no memory, packet from %u:%u dropped\n",
		    info->src_node, info->src_port);
		return;
	}
	rx->node = info->src_node;
	rx->port = info->src_port;
	rx->len = len;
	memcpy(rx->data, data, len);
	glink_lock(&c->lock);
	if (c->releasing) {
		glink_unlock(&c->lock);
		glink_free(rx);
		return;
	}
	*c->rx_tail = rx;
	c->rx_tail = &rx->next;
	glink_unlock(&c->lock);
	glink_work_queue(&c->rx_work);
}

static void
qmi_recv_new_server(struct qmi_core *c, unsigned int service,
    unsigned int instance, unsigned int node, unsigned int port)
{
	struct qmi_handle *qmi = c->qmi;
	struct qmi_service *svc;

	if (qmi->ops.new_server == NULL)
		return;
	/* the end of a lookup's answer */
	if (node == 0 && port == 0)
		return;
	svc = glink_zalloc(sizeof(*svc));
	svc->service = service;
	svc->version = instance & 0xff;
	svc->instance = instance >> 8;
	svc->node = node;
	svc->port = port;
	if (qmi->ops.new_server(qmi, svc) < 0) {
		glink_free(svc);
		return;
	}
	glink_lock(&c->lock);
	svc->next = c->services;
	c->services = svc;
	glink_unlock(&c->lock);
}

/* The servers at node (and port, unless any_port) are gone */
static void
qmi_recv_del_server(struct qmi_core *c, unsigned int node, unsigned int port,
    bool any_port)
{
	struct qmi_handle *qmi = c->qmi;
	struct qmi_service *svc, **pp;

	for (;;) {
		glink_lock(&c->lock);
		for (pp = &c->services; (svc = *pp) != NULL; pp = &svc->next)
			if (svc->node == node && (any_port || svc->port == port))
				break;
		if (svc != NULL)
			*pp = svc->next;
		glink_unlock(&c->lock);
		if (svc == NULL)
			return;
		if (qmi->ops.del_server != NULL)
			qmi->ops.del_server(qmi, svc);
		glink_free(svc);
	}
}

static void
qmi_recv_ctrl_pkt(struct qmi_core *c, const struct qmi_rx *rx)
{
	struct qmi_handle *qmi = c->qmi;
	struct qrtr_ctrl_pkt pkt;

	if (rx->len < sizeof(pkt.cmd))
		return;
	memset(&pkt, 0, sizeof(pkt));
	memcpy(&pkt, rx->data, MIN(rx->len, sizeof(pkt)));
	switch (pkt.cmd) {
	case QRTR_TYPE_NEW_SERVER:
		qmi_recv_new_server(c, pkt.server.service, pkt.server.instance,
		    pkt.server.node, pkt.server.port);
		break;
	case QRTR_TYPE_DEL_SERVER:
		qmi_recv_del_server(c, pkt.server.node, pkt.server.port, false);
		break;
	case QRTR_TYPE_BYE:
		if (qmi->ops.bye != NULL)
			qmi->ops.bye(qmi, rx->node);
		qmi_recv_del_server(c, rx->node, 0, true);
		break;
	case QRTR_TYPE_DEL_CLIENT:
		if (qmi->ops.del_client != NULL)
			qmi->ops.del_client(qmi, pkt.client.node,
			    pkt.client.port);
		break;
	}
}

/* A request or indication for its handler, decoded */
static void
qmi_invoke_handler(struct qmi_core *c, struct sockaddr_qrtr *sq,
    struct qmi_txn *txn, const struct qmi_rx *rx)
{
	struct qmi_handle *qmi = c->qmi;
	const struct qmi_header *hdr = (const void *)rx->data;
	const struct qmi_msg_handler *h;
	void *dest;

	if (qmi->handlers == NULL)
		return;
	for (h = qmi->handlers; h->fn != NULL; h++)
		if (h->type == hdr->type && h->msg_id == le16toh(hdr->msg_id))
			break;
	if (h->fn == NULL)
		return;
	dest = glink_zalloc(MAX(h->decoded_size, 1));
	if (qmi_decode_message(rx->data, rx->len, h->ei, dest) < 0)
		glink_log("qmi: message %u from %u:%u not decoded\n",
		    le16toh(hdr->msg_id), rx->node, rx->port);
	else
		h->fn(qmi, sq, txn, dest);
	glink_free(dest);
}

static void
qmi_handle_message(struct qmi_core *c, struct sockaddr_qrtr *sq,
    const struct qmi_rx *rx)
{
	const struct qmi_header *hdr = (const void *)rx->data;
	struct qmi_txn_state *st;
	struct qmi_txn tmp;
	uint16_t id;
	int ret;

	if (rx->len < sizeof(*hdr)) {
		glink_log("qmi: short packet from %u:%u\n", rx->node, rx->port);
		return;
	}
	id = le16toh(hdr->txn_id);
	if (hdr->type != QMI_RESPONSE) {
		/* a transaction of the sender's, for the reply */
		memset(&tmp, 0, sizeof(tmp));
		tmp.qmi = c->qmi;
		tmp.id = id;
		qmi_invoke_handler(c, sq, &tmp, rx);
		return;
	}
	glink_lock(&c->lock);
	for (st = c->txns; st != NULL; st = st->next)
		if (st->id == id && !st->done)
			break;
	if (st == NULL) {
		/* nobody waits for it (any more) */
		glink_unlock(&c->lock);
		return;
	}
	if (st->txn->dest != NULL && st->txn->ei != NULL) {
		/* decoded under the lock: a cancel cannot free dest meanwhile */
		ret = qmi_decode_message(rx->data, rx->len, st->txn->ei,
		    st->txn->dest);
		if (ret < 0)
			glink_log("qmi: response %u from %u:%u not decoded\n",
			    le16toh(hdr->msg_id), rx->node, rx->port);
		st->result = ret;
		st->done = true;
		glink_broadcast(&c->cond);
		glink_unlock(&c->lock);
		return;
	}
	glink_unlock(&c->lock);
	qmi_invoke_handler(c, sq, st->txn, rx);
}

static void
qmi_rx_work(void *arg, int pending)
{
	struct qmi_core *c = arg;
	struct qmi_handle *qmi = c->qmi;
	struct sockaddr_qrtr sq;
	struct qmi_rx *rx;

	for (;;) {
		glink_lock(&c->lock);
		if ((rx = c->rx_head) != NULL) {
			c->rx_head = rx->next;
			if (c->rx_head == NULL)
				c->rx_tail = &c->rx_head;
		}
		glink_unlock(&c->lock);
		if (rx == NULL)
			return;
		memset(&sq, 0, sizeof(sq));
		sq.sq_len = sizeof(sq);
		sq.sq_family = AF_QIPCRTR;
		sq.sq_node = rx->node;
		sq.sq_port = rx->port;
		if (rx->port == QRTR_PORT_CTRL)
			qmi_recv_ctrl_pkt(c, rx);
		else if (qmi->ops.msg_handler != NULL)
			qmi->ops.msg_handler(qmi, &sq, rx->data, rx->len);
		else
			qmi_handle_message(c, &sq, rx);
		glink_free(rx);
	}
}

/* --- the handle ---------------------------------------------------------------------- */

int
qmi_handle_init(struct qmi_handle *qmi, size_t recv_buf_size,
    const struct qmi_ops *ops, const struct qmi_msg_handler *handlers)
{
	struct qmi_core *c;
	int error;

	memset(qmi, 0, sizeof(*qmi));
	c = glink_zalloc(sizeof(*c));
	c->qmi = qmi;
	c->rx_tail = &c->rx_head;
	c->next_txn = 1;
	glink_mutex_init(&c->lock);
	glink_cond_init(&c->cond);
	glink_work_init(&c->rx_work, qmi_rx_work, c);
	qmi->priv = c;
	qmi->sock = qmi;
	qmi->recv_buf_size = recv_buf_size;
	if (ops != NULL)
		qmi->ops = *ops;
	qmi->handlers = handlers;
	error = qrtr_port_open(0, qmi_rx_cb, c, &c->port);
	if (error != 0) {
		glink_cond_destroy(&c->cond);
		glink_mutex_destroy(&c->lock);
		glink_free(c);
		qmi->priv = NULL;
		return (-error);
	}
	return (0);
}

static void
free_ns_entries(struct qmi_ns_entry *e)
{
	struct qmi_ns_entry *next;

	for (; e != NULL; e = next) {
		next = e->next;
		glink_free(e);
	}
}

void
qmi_handle_release(struct qmi_handle *qmi)
{
	struct qmi_core *c = core_of(qmi);
	struct qmi_txn_state *st;
	struct qmi_service *svc;
	struct qmi_rx *rx;
	uint64_t until;

	if (c == NULL)
		return;
	glink_lock(&c->lock);
	c->releasing = true;
	glink_unlock(&c->lock);
	/* no callbacks after the port is closed, no work after the drain */
	qrtr_port_close(c->port);
	glink_work_drain(&c->rx_work);

	glink_lock(&c->lock);
	while ((rx = c->rx_head) != NULL) {
		c->rx_head = rx->next;
		glink_free(rx);
	}
	/* waiting transactions end (Linux: -ENETRESET), give them a moment */
	for (st = c->txns; st != NULL; st = st->next) {
		st->result = -ENETRESET;
		st->done = true;
	}
	glink_broadcast(&c->cond);
	until = glink_now_ms() + 500;
	while (c->txns != NULL && glink_now_ms() < until)
		glink_cond_wait(&c->cond, &c->lock, 50);
	while ((st = c->txns) != NULL) {
		c->txns = st->next;
		st->txn->priv = NULL;
		glink_free(st);
	}
	while ((svc = c->services) != NULL) {
		c->services = svc->next;
		glink_free(svc);
	}
	glink_unlock(&c->lock);
	free_ns_entries(c->lookups);
	free_ns_entries(c->servers);
	glink_cond_destroy(&c->cond);
	glink_mutex_destroy(&c->lock);
	glink_free(c);
	qmi->priv = NULL;
}

static int
qmi_send_ns(struct qmi_core *c, uint32_t cmd, const struct qmi_ns_entry *e)
{
	struct qrtr_ctrl_pkt pkt;

	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = cmd;
	pkt.server.service = e->service;
	pkt.server.instance = e->version | e->instance << 8;
	if (cmd == QRTR_TYPE_NEW_SERVER) {
		pkt.server.node = QRTR_LOCAL_NODE;
		pkt.server.port = qrtr_port_id(c->port);
	}
	return (-qrtr_sendto(c->port, QRTR_LOCAL_NODE, QRTR_PORT_CTRL, &pkt,
	    sizeof(pkt)));
}

static int
qmi_add_ns_entry(struct qmi_handle *qmi, struct qmi_ns_entry **list,
    uint32_t cmd, unsigned int service, unsigned int version,
    unsigned int instance)
{
	struct qmi_core *c = core_of(qmi);
	struct qmi_ns_entry *e;

	e = glink_zalloc(sizeof(*e));
	e->service = service;
	e->version = version;
	e->instance = instance;
	glink_lock(&c->lock);
	e->next = *list;
	*list = e;
	glink_unlock(&c->lock);
	/* the name service answers right away, into qmi_rx_cb: not locked */
	return (qmi_send_ns(c, cmd, e));
}

int
qmi_add_lookup(struct qmi_handle *qmi, unsigned int service,
    unsigned int version, unsigned int instance)
{
	return (qmi_add_ns_entry(qmi, &core_of(qmi)->lookups,
	    QRTR_TYPE_NEW_LOOKUP, service, version, instance));
}

int
qmi_add_server(struct qmi_handle *qmi, unsigned int service,
    unsigned int version, unsigned int instance)
{
	return (qmi_add_ns_entry(qmi, &core_of(qmi)->servers,
	    QRTR_TYPE_NEW_SERVER, service, version, instance));
}

/* --- transactions ---------------------------------------------------------------------- */

int
qmi_txn_init(struct qmi_handle *qmi, struct qmi_txn *txn,
    const struct qmi_elem_info *ei, void *c_struct)
{
	struct qmi_core *c = core_of(qmi);
	struct qmi_txn_state *st, *o;
	unsigned int i;

	memset(txn, 0, sizeof(*txn));
	txn->qmi = qmi;
	txn->ei = ei;
	txn->dest = c_struct;
	st = glink_zalloc(sizeof(*st));
	st->txn = txn;
	glink_lock(&c->lock);
	/* the next id in use by none, 1 to 0xffff, as Linux's cyclic idr */
	for (i = 0; i < 0xffff; i++) {
		st->id = c->next_txn;
		c->next_txn = c->next_txn == 0xffff ? 1 : c->next_txn + 1;
		for (o = c->txns; o != NULL; o = o->next)
			if (o->id == st->id)
				break;
		if (o == NULL)
			break;
	}
	if (i == 0xffff) {
		glink_unlock(&c->lock);
		glink_free(st);
		return (-ENOSPC);
	}
	st->next = c->txns;
	c->txns = st;
	glink_unlock(&c->lock);
	txn->id = st->id;
	txn->priv = st;
	return (0);
}

/* The state out of the handle's list; under the lock */
static void
qmi_txn_unlink(struct qmi_core *c, struct qmi_txn_state *st)
{
	struct qmi_txn_state **pp;

	for (pp = &c->txns; *pp != NULL; pp = &(*pp)->next)
		if (*pp == st) {
			*pp = st->next;
			break;
		}
}

int
qmi_txn_wait(struct qmi_txn *txn, unsigned long timeout)
{
	struct qmi_core *c = core_of(txn->qmi);
	struct qmi_txn_state *st = txn->priv;
	uint64_t until, now;
	bool done;
	int result;

	if (st == NULL)
		return (-EINVAL);
	until = glink_now_ms() + glink_ticks_to_ms(timeout);
	glink_lock(&c->lock);
	while (!st->done && (now = glink_now_ms()) < until)
		glink_cond_wait(&c->cond, &c->lock, (unsigned int)(until - now));
	done = st->done;
	result = st->result;
	qmi_txn_unlink(c, st);
	glink_broadcast(&c->cond);
	glink_unlock(&c->lock);
	glink_free(st);
	txn->priv = NULL;
	if (!done)
		return (-ETIMEDOUT);
	txn->result = result;
	return (result);
}

void
qmi_txn_cancel(struct qmi_txn *txn)
{
	struct qmi_core *c = core_of(txn->qmi);
	struct qmi_txn_state *st = txn->priv;

	if (st == NULL)
		return;
	glink_lock(&c->lock);
	qmi_txn_unlink(c, st);
	glink_broadcast(&c->cond);
	glink_unlock(&c->lock);
	glink_free(st);
	txn->priv = NULL;
}

/* --- sending ---------------------------------------------------------------------- */

static ssize_t
qmi_send_message(struct qmi_handle *qmi, struct sockaddr_qrtr *sq,
    unsigned int txn_id, int type, int msg_id, size_t len,
    const struct qmi_elem_info *ei, const void *c_struct)
{
	struct qmi_core *c = core_of(qmi);
	uint32_t node, port;
	void *msg;
	int error;

	if (sq != NULL) {
		node = sq->sq_node;
		port = sq->sq_port;
	} else {
		/* a connected handle's peer, as Linux's connected socket */
		glink_lock(&c->lock);
		node = c->peer_node;
		port = c->peer_port;
		error = c->connected ? 0 : ENOTCONN;
		glink_unlock(&c->lock);
		if (error != 0)
			return (-error);
	}
	msg = qmi_encode_message(type, msg_id, &len, txn_id, ei, c_struct);
	if (msg == NULL)
		return (-EINVAL);
	error = qrtr_sendto(c->port, node, port, msg, len);
	glink_free(msg);
	return (error != 0 ? -error : (ssize_t)len);
}

int
qmi_connect(struct qmi_handle *qmi, const struct sockaddr_qrtr *sq, size_t len)
{
	struct qmi_core *c;

	if (qmi == NULL || (c = core_of(qmi)) == NULL || sq == NULL ||
	    len < sizeof(*sq))
		return (-EINVAL);
	glink_lock(&c->lock);
	c->peer_node = sq->sq_node;
	c->peer_port = sq->sq_port;
	c->connected = true;
	glink_unlock(&c->lock);
	return (0);
}

ssize_t
qmi_send_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq,
    struct qmi_txn *txn, int msg_id, size_t len,
    const struct qmi_elem_info *ei, const void *c_struct)
{
	return (qmi_send_message(qmi, sq, txn->id, QMI_REQUEST, msg_id, len,
	    ei, c_struct));
}

ssize_t
qmi_send_response(struct qmi_handle *qmi, struct sockaddr_qrtr *sq,
    struct qmi_txn *txn, int msg_id, size_t len,
    const struct qmi_elem_info *ei, const void *c_struct)
{
	return (qmi_send_message(qmi, sq, txn->id, QMI_RESPONSE, msg_id, len,
	    ei, c_struct));
}

ssize_t
qmi_send_indication(struct qmi_handle *qmi, struct sockaddr_qrtr *sq,
    int msg_id, size_t len, const struct qmi_elem_info *ei,
    const void *c_struct)
{
	return (qmi_send_message(qmi, sq, 0, QMI_INDICATION, msg_id, len, ei,
	    c_struct));
}
