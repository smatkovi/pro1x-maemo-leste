/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The QRTR router and name service (qrtr.h), after Linux's af_qrtr.c and
 * ns.c.
 *
 * Packets for local ports go to their callback outside the router's lock
 * (a reference keeps the port while it runs). Packets for other nodes go to
 * their endpoint, which queues them: nothing here waits for a remote
 * processor, except a sender of data that has 10 packets on their way to
 * one port unconfirmed (Linux's flow control: the 5th asks for a
 * RESUME_TX). The name service handles its packets in its own work item,
 * which holds the lock of its lists (not of its queue) while it tells
 * others: a port's callback may send to the name service from there.
 *
 * A packet's type is the payload's command when it goes to the control
 * port, or comes from it (the name service's notices to a port: Linux's
 * qrtr_sendmsg types by the sender's port), else data.
 */

#include "qrtr.h"

#ifdef _KERNEL
MALLOC_DECLARE(M_GLINK);
#else
#include <sys/queue.h>
#endif

#define	PROTO_VER_1		1
#define	PROTO_VER_2		3
#define	PORT_CTRL_LEGACY	0xffff
#define	MIN_EPH_PORT		0x4000
#define	MAX_EPH_PORT		0x7fff
#define	FLOW_HIGH		10
#define	FLOW_LOW		5
#define	FLOW_TIMEOUT_MS		30000
#define	V2_CONFIRM_RX		0x1

struct qrtr_hdr_v1 {
	uint32_t	version;
	uint32_t	type;
	uint32_t	src_node_id;
	uint32_t	src_port_id;
	uint32_t	confirm_rx;
	uint32_t	size;
	uint32_t	dst_node_id;
	uint32_t	dst_port_id;
} __attribute__((__packed__));

struct qrtr_hdr_v2 {
	uint8_t		version;
	uint8_t		type;
	uint8_t		flags;
	uint8_t		optlen;
	uint32_t	size;
	uint16_t	src_node_id;
	uint16_t	src_port_id;
	uint16_t	dst_node_id;
	uint16_t	dst_port_id;
} __attribute__((__packed__));

struct qrtr_port {
	TAILQ_ENTRY(qrtr_port) link;
	uint32_t	id;
	qrtr_rx_cb_t	*cb;
	void		*arg;
	int		refs;		/* callbacks running */
	bool		closing;
};

struct flow {
	TAILQ_ENTRY(flow) link;
	uint32_t	node, port;
	int		pending;
	bool		failed;
};

struct qrtr_node {
	TAILQ_ENTRY(qrtr_node) link;
	qrtr_xmit_t	*xmit;
	void		*arg;
	uint32_t	nid;		/* QRTR_NODE_BCAST: not known yet */
	bool		gone;
	int		refs;
	TAILQ_HEAD(, flow) flows;
	u_int		rx, tx, hellos;
};

/* node ids reached through an endpoint (a node may bridge others) */
struct nid_map {
	TAILQ_ENTRY(nid_map) link;
	uint32_t	nid;
	struct qrtr_node *node;
};

static struct {
	glink_mutex_t	lock;
	glink_cond_t	cond;
	bool		up;
	TAILQ_HEAD(, qrtr_port) ports;
	TAILQ_HEAD(, qrtr_node) nodes;
	TAILQ_HEAD(, nid_map) nids;
	uint32_t	next_port;
} r;

/* --- the name service's state (only its work item changes it) --------------------- */

struct ns_server {
	TAILQ_ENTRY(ns_server) link;
	uint32_t	service, instance, node, port;
};

struct ns_lookup {
	TAILQ_ENTRY(ns_lookup) link;
	uint32_t	service, instance, node, port;
};

struct ns_pkt {
	TAILQ_ENTRY(ns_pkt) link;
	uint32_t	src_node, src_port;
	struct qrtr_ctrl_pkt pkt;
};

static struct {
	glink_mutex_t	lock;		/* the lists */
	glink_mutex_t	qlock;		/* the queue */
	TAILQ_HEAD(, ns_server) servers;
	TAILQ_HEAD(, ns_lookup) lookups;
	TAILQ_HEAD(, ns_pkt) queue;
	glink_work_t	work;
	struct qrtr_port *port;
	int		nlookups, nservers;
} ns;

#define	NS_MAX_SERVERS		256
#define	NS_MAX_LOOKUPS		64

static int send_from(uint32_t src_node, uint32_t src_port, uint32_t dst_node,
    uint32_t dst_port, const void *data, size_t len);

/* --- nodes and flow control ----------------------------------------------------------- */

static struct qrtr_node *
node_lookup(uint32_t nid)
{
	struct nid_map *m;

	TAILQ_FOREACH(m, &r.nids, link)
		if (m->nid == nid && !m->node->gone)
			return (m->node);
	return (NULL);
}

static void
node_assign(struct qrtr_node *n, uint32_t nid)
{
	struct nid_map *m;

	if (nid == QRTR_NODE_BCAST || nid == QRTR_LOCAL_NODE)
		return;
	TAILQ_FOREACH(m, &r.nids, link)
		if (m->nid == nid) {
			m->node = n;
			goto out;
		}
	m = glink_zalloc(sizeof(*m));
	m->nid = nid;
	m->node = n;
	TAILQ_INSERT_TAIL(&r.nids, m, link);
out:
	if (n->nid == QRTR_NODE_BCAST)
		n->nid = nid;
}

static struct flow *
flow_get(struct qrtr_node *n, uint32_t node, uint32_t port, bool create)
{
	struct flow *f;

	TAILQ_FOREACH(f, &n->flows, link)
		if (f->node == node && f->port == port)
			return (f);
	if (!create)
		return (NULL);
	f = glink_zalloc(sizeof(*f));
	f->node = node;
	f->port = port;
	TAILQ_INSERT_TAIL(&n->flows, f, link);
	return (f);
}

/* With the lock: may this data go, and does it ask for a RESUME_TX? */
static int
tx_wait(struct qrtr_node *n, uint32_t node, uint32_t port, int type,
    int *confirm)
{
	struct flow *f;
	uint64_t end;

	*confirm = 0;
	if (type != QRTR_TYPE_DATA)
		return (0);
	f = flow_get(n, node, port, true);
	end = glink_now_ms() + FLOW_TIMEOUT_MS;
	while (f->pending >= FLOW_HIGH && !f->failed && !n->gone) {
		if (glink_now_ms() >= end)
			return (GLINK_TIMEDOUT);
		glink_cond_wait(&r.cond, &r.lock, 100);
	}
	if (n->gone)
		return (EPIPE);
	if (f->failed) {
		f->failed = false;
		*confirm = 1;
	} else {
		f->pending++;
		*confirm = f->pending == FLOW_LOW;
	}
	return (0);
}

/* A packet to one node's endpoint; with the lock and a reference held */
static int
node_enqueue(struct qrtr_node *n, int type, uint32_t src_node,
    uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *data,
    size_t len)
{
	struct qrtr_hdr_v1 *h;
	struct flow *f;
	size_t plen;
	int confirm, error;

	if ((error = tx_wait(n, dst_node, dst_port, type, &confirm)) != 0)
		return (error);
	plen = sizeof(*h) + roundup2(len, 4);
	h = glink_zalloc(plen);
	h->version = PROTO_VER_1;
	h->type = type;
	h->src_node_id = src_node;
	h->src_port_id = src_port;
	h->confirm_rx = confirm != 0;
	h->size = len;
	if (dst_port == QRTR_PORT_CTRL) {
		h->dst_node_id = n->nid;
		h->dst_port_id = QRTR_PORT_CTRL;
	} else {
		h->dst_node_id = dst_node;
		h->dst_port_id = dst_port;
	}
	if (len != 0)
		memcpy(h + 1, data, len);
	error = n->gone ? ENODEV : n->xmit(n->arg, h, plen);
	if (error == 0)
		n->tx++;
	glink_free(h);
	if (error != 0 && confirm &&
	    (f = flow_get(n, dst_node, dst_port, false)) != NULL)
		f->failed = true;
	return (error);
}

/* --- local ports ----------------------------------------------------------------------- */

static struct qrtr_port *
port_lookup(uint32_t id)
{
	struct qrtr_port *p;

	TAILQ_FOREACH(p, &r.ports, link)
		if (p->id == id && !p->closing)
			return (p);
	return (NULL);
}

/* To a local port, not to the sender itself; without the lock */
static int
deliver_local(uint32_t src_node, uint32_t src_port, uint32_t dst_port,
    int type, const void *data, size_t len)
{
	struct qrtr_rx_info info;
	struct qrtr_port *p;

	glink_lock(&r.lock);
	p = port_lookup(dst_port);
	if (p == NULL || (src_node == QRTR_LOCAL_NODE && src_port == dst_port)) {
		glink_unlock(&r.lock);
		return (ENODEV);
	}
	p->refs++;
	glink_unlock(&r.lock);
	info.src_node = src_node;
	info.src_port = src_port;
	info.type = type;
	p->cb(p->arg, &info, data, len);
	glink_lock(&r.lock);
	if (--p->refs == 0 && p->closing)
		glink_broadcast(&r.cond);
	glink_unlock(&r.lock);
	return (0);
}

int
qrtr_port_open(uint32_t port, qrtr_rx_cb_t *cb, void *arg,
    struct qrtr_port **pp)
{
	struct qrtr_port *p;
	uint32_t i;

	glink_lock(&r.lock);
	if (!r.up) {
		glink_unlock(&r.lock);
		return (ENXIO);
	}
	if (port == 0) {
		for (i = 0; i <= MAX_EPH_PORT - MIN_EPH_PORT; i++) {
			port = r.next_port++;
			if (r.next_port > MAX_EPH_PORT)
				r.next_port = MIN_EPH_PORT;
			if (port_lookup(port) == NULL)
				break;
		}
		if (i > MAX_EPH_PORT - MIN_EPH_PORT) {
			glink_unlock(&r.lock);
			return (EADDRINUSE);
		}
	} else if (port_lookup(port) != NULL) {
		glink_unlock(&r.lock);
		return (EADDRINUSE);
	}
	p = glink_zalloc(sizeof(*p));
	p->id = port;
	p->cb = cb;
	p->arg = arg;
	TAILQ_INSERT_TAIL(&r.ports, p, link);
	glink_unlock(&r.lock);
	*pp = p;
	return (0);
}

uint32_t
qrtr_port_id(struct qrtr_port *p)
{
	return (p->id);
}

/* The port goes; the others hear of it (DEL_CLIENT to all, as Linux) */
void
qrtr_port_close(struct qrtr_port *p)
{
	struct qrtr_ctrl_pkt pkt;
	uint32_t id = p->id;

	glink_lock(&r.lock);
	p->closing = true;
	while (p->refs > 0)
		glink_cond_wait(&r.cond, &r.lock, 100);
	TAILQ_REMOVE(&r.ports, p, link);
	glink_unlock(&r.lock);
	glink_free(p);
	if (id == QRTR_PORT_CTRL)
		return;
	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = QRTR_TYPE_DEL_CLIENT;
	pkt.client.node = QRTR_LOCAL_NODE;
	pkt.client.port = id;
	send_from(QRTR_LOCAL_NODE, id, QRTR_NODE_BCAST, QRTR_PORT_CTRL, &pkt,
	    sizeof(pkt));
}

/* --- sending ------------------------------------------------------------------------------ */

static int
send_from(uint32_t src_node, uint32_t src_port, uint32_t dst_node,
    uint32_t dst_port, const void *data, size_t len)
{
	struct qrtr_node *n, *list[16];
	int type, error = 0, i, count = 0;

	if (dst_port == QRTR_PORT_CTRL || src_port == QRTR_PORT_CTRL) {
		if (len < 4)
			return (EINVAL);
		memcpy(&type, data, 4);
	} else
		type = QRTR_TYPE_DATA;

	if (dst_node == QRTR_LOCAL_NODE)
		return (deliver_local(src_node, src_port, dst_port, type, data,
		    len));
	glink_lock(&r.lock);
	if (dst_node == QRTR_NODE_BCAST) {
		TAILQ_FOREACH(n, &r.nodes, link)
			if (!n->gone && n->nid != QRTR_NODE_BCAST &&
			    count < (int)nitems(list)) {
				n->refs++;
				list[count++] = n;
			}
	} else if ((n = node_lookup(dst_node)) != NULL) {
		n->refs++;
		list[count++] = n;
	} else
		error = EHOSTUNREACH;
	for (i = 0; i < count; i++) {
		n = list[i];
		node_enqueue(n, type, src_node, src_port, dst_node, dst_port,
		    data, len);
		if (--n->refs == 0 && n->gone)
			glink_broadcast(&r.cond);
	}
	glink_unlock(&r.lock);
	if (dst_node == QRTR_NODE_BCAST)
		deliver_local(src_node, src_port, dst_port, type, data, len);
	return (error);
}

int
qrtr_sendto(struct qrtr_port *p, uint32_t node, uint32_t port,
    const void *data, size_t len)
{
	return (send_from(QRTR_LOCAL_NODE, p->id, node, port, data, len));
}

/* --- endpoints ----------------------------------------------------------------------------- */

struct qrtr_node *
qrtr_endpoint_register(qrtr_xmit_t *xmit, void *arg)
{
	struct qrtr_node *n;

	n = glink_zalloc(sizeof(*n));
	n->xmit = xmit;
	n->arg = arg;
	n->nid = QRTR_NODE_BCAST;
	TAILQ_INIT(&n->flows);
	glink_lock(&r.lock);
	TAILQ_INSERT_TAIL(&r.nodes, n, link);
	glink_unlock(&r.lock);
	return (n);
}

int
qrtr_endpoint_post(struct qrtr_node *n, const void *data, size_t len)
{
	const struct qrtr_hdr_v1 *v1;
	const struct qrtr_hdr_v2 *v2;
	const struct qrtr_ctrl_pkt *pkt;
	struct qrtr_ctrl_pkt resume;
	uint32_t src_node, src_port, dst_node, dst_port, size;
	size_t hdrlen;
	struct flow *f;
	int type, confirm;

	if (len == 0 || (len & 3) != 0)
		return (EINVAL);
	switch (*(const uint8_t *)data) {
	case PROTO_VER_1:
		if (len < sizeof(*v1))
			return (EINVAL);
		v1 = data;
		hdrlen = sizeof(*v1);
		type = v1->type;
		src_node = v1->src_node_id;
		src_port = v1->src_port_id;
		confirm = v1->confirm_rx != 0;
		dst_node = v1->dst_node_id;
		dst_port = v1->dst_port_id;
		size = v1->size;
		break;
	case PROTO_VER_2:
		if (len < sizeof(*v2))
			return (EINVAL);
		v2 = data;
		hdrlen = sizeof(*v2) + v2->optlen;
		type = v2->type;
		confirm = (v2->flags & V2_CONFIRM_RX) != 0;
		src_node = v2->src_node_id;
		src_port = v2->src_port_id;
		dst_node = v2->dst_node_id;
		dst_port = v2->dst_port_id;
		if (src_port == (uint16_t)QRTR_PORT_CTRL)
			src_port = QRTR_PORT_CTRL;
		if (dst_port == (uint16_t)QRTR_PORT_CTRL)
			dst_port = QRTR_PORT_CTRL;
		size = v2->size;
		break;
	default:
		return (EINVAL);
	}
	if (dst_port == PORT_CTRL_LEGACY)
		dst_port = QRTR_PORT_CTRL;
	if (size == 0 || len != roundup2(size, 4) + hdrlen)
		return (EINVAL);
	pkt = (const struct qrtr_ctrl_pkt *)((const char *)data + hdrlen);
	if ((type == QRTR_TYPE_NEW_SERVER || type == QRTR_TYPE_RESUME_TX) &&
	    size < sizeof(*pkt))
		return (EINVAL);
	if (dst_port != QRTR_PORT_CTRL && type != QRTR_TYPE_DATA &&
	    type != QRTR_TYPE_RESUME_TX)
		return (EINVAL);
	(void)dst_node;

	glink_lock(&r.lock);
	n->rx++;
	if (type == QRTR_TYPE_HELLO)
		n->hellos++;
	node_assign(n, src_node);
	if (type == QRTR_TYPE_NEW_SERVER)
		node_assign(n, pkt->server.node);
	if (type == QRTR_TYPE_RESUME_TX) {
		f = flow_get(n, pkt->client.node, pkt->client.port, false);
		if (f != NULL) {
			f->pending = 0;
			glink_broadcast(&r.cond);
		}
		glink_unlock(&r.lock);
		return (0);
	}
	glink_unlock(&r.lock);

	deliver_local(src_node, src_port, dst_port, type,
	    (const char *)data + hdrlen, size);
	/* the sender waits for this once 5 are on their way (af_qrtr sends
	 * it when the packet is read; here it is delivered) */
	if (confirm && type == QRTR_TYPE_DATA) {
		memset(&resume, 0, sizeof(resume));
		resume.cmd = QRTR_TYPE_RESUME_TX;
		resume.client.node = QRTR_LOCAL_NODE;
		resume.client.port = dst_port;
		glink_lock(&r.lock);
		node_enqueue(n, QRTR_TYPE_RESUME_TX, QRTR_LOCAL_NODE, dst_port,
		    src_node, src_port, &resume, sizeof(resume));
		glink_unlock(&r.lock);
	}
	return (0);
}

/*
 * The endpoint is gone: the name service hears a BYE from every node it
 * reached (Linux's qrtr_endpoint_unregister: one per node id routed
 * through it, a node may bridge others)
 */
void
qrtr_endpoint_unregister(struct qrtr_node *n)
{
	struct qrtr_ctrl_pkt bye;
	struct nid_map *m, *next;
	struct flow *f;
	uint32_t nids[16];
	int i, count = 0;

	glink_lock(&r.lock);
	n->gone = true;
	glink_broadcast(&r.cond);
	while (n->refs > 0)
		glink_cond_wait(&r.cond, &r.lock, 100);
	for (m = TAILQ_FIRST(&r.nids); m != NULL; m = next) {
		next = TAILQ_NEXT(m, link);
		if (m->node == n) {
			if (count < (int)nitems(nids))
				nids[count++] = m->nid;
			TAILQ_REMOVE(&r.nids, m, link);
			glink_free(m);
		}
	}
	TAILQ_REMOVE(&r.nodes, n, link);
	while ((f = TAILQ_FIRST(&n->flows)) != NULL) {
		TAILQ_REMOVE(&n->flows, f, link);
		glink_free(f);
	}
	glink_unlock(&r.lock);
	glink_free(n);
	for (i = 0; i < count; i++) {
		memset(&bye, 0, sizeof(bye));
		bye.cmd = QRTR_TYPE_BYE;
		deliver_local(nids[i], QRTR_PORT_CTRL, QRTR_PORT_CTRL,
		    QRTR_TYPE_BYE, &bye, sizeof(bye));
	}
}

void
qrtr_endpoint_hello(struct qrtr_node *n)
{
	struct qrtr_ctrl_pkt hello;

	memset(&hello, 0, sizeof(hello));
	hello.cmd = QRTR_TYPE_HELLO;
	glink_lock(&r.lock);
	if (!n->gone) {
		n->refs++;
		node_enqueue(n, QRTR_TYPE_HELLO, QRTR_LOCAL_NODE, QRTR_PORT_CTRL,
		    n->nid, QRTR_PORT_CTRL, &hello, sizeof(hello));
		if (--n->refs == 0 && n->gone)
			glink_broadcast(&r.cond);
	}
	glink_unlock(&r.lock);
}

void
qrtr_nodes(qrtr_node_cb_t *cb, void *arg)
{
	struct qrtr_node_info ni;
	struct qrtr_node *n;
	struct nid_map *m;

	glink_lock(&r.lock);
	TAILQ_FOREACH(n, &r.nodes, link) {
		memset(&ni, 0, sizeof(ni));
		ni.nid = n->nid;
		TAILQ_FOREACH(m, &r.nids, link)
			if (m->node == n && ni.nnids < (int)nitems(ni.nids))
				ni.nids[ni.nnids++] = m->nid;
		ni.rx = n->rx;
		ni.tx = n->tx;
		ni.hellos = n->hellos;
		ni.gone = n->gone;
		cb(arg, &ni);
	}
	glink_unlock(&r.lock);
}

/* --- the name service ------------------------------------------------------------------------ */

static void
ns_send(uint32_t node, uint32_t port, uint32_t cmd, const struct ns_server *s,
    uint32_t cnode, uint32_t cport)
{
	struct qrtr_ctrl_pkt pkt;

	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = cmd;
	if (s != NULL) {
		pkt.server.service = s->service;
		pkt.server.instance = s->instance;
		pkt.server.node = s->node;
		pkt.server.port = s->port;
	} else if (cmd == QRTR_TYPE_BYE || cmd == QRTR_TYPE_DEL_CLIENT) {
		pkt.client.node = cnode;
		pkt.client.port = cport;
	}
	send_from(QRTR_LOCAL_NODE, QRTR_PORT_CTRL, node, port, &pkt, sizeof(pkt));
}

static bool
ns_match(const struct ns_server *s, uint32_t service, uint32_t instance)
{
	uint32_t ifilter = instance != 0 ? ~0U : 0;

	if (service != 0 && s->service != service)
		return (false);
	return ((s->instance & ifilter) == instance);
}

static void
ns_notify_lookups(const struct ns_server *s, bool new)
{
	struct ns_lookup *l;

	TAILQ_FOREACH(l, &ns.lookups, link) {
		if (l->service != 0 && l->service != s->service)
			continue;
		if (l->instance != 0 && l->instance != s->instance)
			continue;
		ns_send(l->node, l->port, new ? QRTR_TYPE_NEW_SERVER :
		    QRTR_TYPE_DEL_SERVER, s, 0, 0);
	}
}

static struct ns_server *
ns_find(uint32_t node, uint32_t port)
{
	struct ns_server *s;

	TAILQ_FOREACH(s, &ns.servers, link)
		if (s->node == node && s->port == port)
			return (s);
	return (NULL);
}

static void
ns_server_del(struct ns_server *s, bool bcast)
{
	TAILQ_REMOVE(&ns.servers, s, link);
	ns.nservers--;
	if (s->node == QRTR_LOCAL_NODE && bcast)
		ns_send(QRTR_NODE_BCAST, QRTR_PORT_CTRL, QRTR_TYPE_DEL_SERVER, s,
		    0, 0);
	ns_notify_lookups(s, false);
	glink_free(s);
}

/* To every local server: a client went away */
static void
ns_tell_local_servers(uint32_t cmd, uint32_t cnode, uint32_t cport)
{
	struct ns_server *s;

	TAILQ_FOREACH(s, &ns.servers, link)
		if (s->node == QRTR_LOCAL_NODE)
			ns_send(s->node, s->port, cmd, NULL, cnode, cport);
}

static void
ns_handle(uint32_t from_node, uint32_t from_port,
    const struct qrtr_ctrl_pkt *pkt)
{
	struct ns_server *s, *snext;
	struct ns_lookup *l, *lnext;
	uint32_t node, port;

	switch (pkt->cmd) {
	case QRTR_TYPE_HELLO:
		ns_send(from_node, from_port, QRTR_TYPE_HELLO, NULL, 0, 0);
		TAILQ_FOREACH(s, &ns.servers, link)
			if (s->node == QRTR_LOCAL_NODE)
				ns_send(from_node, from_port,
				    QRTR_TYPE_NEW_SERVER, s, 0, 0);
		break;
	case QRTR_TYPE_BYE:
		for (s = TAILQ_FIRST(&ns.servers); s != NULL; s = snext) {
			snext = TAILQ_NEXT(s, link);
			if (s->node == from_node)
				ns_server_del(s, true);
		}
		ns_tell_local_servers(QRTR_TYPE_BYE, from_node, 0);
		break;
	case QRTR_TYPE_DEL_CLIENT:
		node = pkt->client.node;
		port = pkt->client.port;
		if (from_node != node ||
		    (from_node == QRTR_LOCAL_NODE && from_port != port))
			break;
		for (l = TAILQ_FIRST(&ns.lookups); l != NULL; l = lnext) {
			lnext = TAILQ_NEXT(l, link);
			if (l->node == node && l->port == port) {
				TAILQ_REMOVE(&ns.lookups, l, link);
				ns.nlookups--;
				glink_free(l);
			}
		}
		if ((s = ns_find(node, port)) != NULL)
			ns_server_del(s, false);
		ns_tell_local_servers(QRTR_TYPE_DEL_CLIENT, node, port);
		break;
	case QRTR_TYPE_NEW_SERVER:
		node = pkt->server.node;
		port = pkt->server.port;
		if (from_node == QRTR_LOCAL_NODE) {
			node = from_node;
			port = from_port;
		}
		if (pkt->server.service == 0 || port == 0)
			break;
		if ((s = ns_find(node, port)) == NULL) {
			if (ns.nservers >= NS_MAX_SERVERS)
				break;
			s = glink_zalloc(sizeof(*s));
			TAILQ_INSERT_TAIL(&ns.servers, s, link);
			ns.nservers++;
		}
		s->service = pkt->server.service;
		s->instance = pkt->server.instance;
		s->node = node;
		s->port = port;
		if (node == QRTR_LOCAL_NODE)
			ns_send(QRTR_NODE_BCAST, QRTR_PORT_CTRL,
			    QRTR_TYPE_NEW_SERVER, s, 0, 0);
		ns_notify_lookups(s, true);
		break;
	case QRTR_TYPE_DEL_SERVER:
		node = pkt->server.node;
		port = pkt->server.port;
		if (from_node == QRTR_LOCAL_NODE) {
			node = from_node;
			port = from_port;
		}
		if ((s = ns_find(node, port)) != NULL)
			ns_server_del(s, true);
		break;
	case QRTR_TYPE_NEW_LOOKUP:
		if (from_node != QRTR_LOCAL_NODE || ns.nlookups >= NS_MAX_LOOKUPS)
			break;
		l = glink_zalloc(sizeof(*l));
		l->service = pkt->server.service;
		l->instance = pkt->server.instance;
		l->node = from_node;
		l->port = from_port;
		TAILQ_INSERT_TAIL(&ns.lookups, l, link);
		ns.nlookups++;
		TAILQ_FOREACH(s, &ns.servers, link)
			if (ns_match(s, l->service, l->instance))
				ns_send(from_node, from_port,
				    QRTR_TYPE_NEW_SERVER, s, 0, 0);
		/* the end of the list: an empty one */
		ns_send(from_node, from_port, QRTR_TYPE_NEW_SERVER, NULL, 0, 0);
		break;
	case QRTR_TYPE_DEL_LOOKUP:
		for (l = TAILQ_FIRST(&ns.lookups); l != NULL; l = lnext) {
			lnext = TAILQ_NEXT(l, link);
			if (l->node == from_node && l->port == from_port &&
			    l->service == pkt->server.service &&
			    (pkt->server.instance == 0 ||
			    l->instance == pkt->server.instance)) {
				TAILQ_REMOVE(&ns.lookups, l, link);
				ns.nlookups--;
				glink_free(l);
			}
		}
		break;
	default:
		break;
	}
}

static void
ns_work(void *arg, int pending)
{
	struct ns_pkt *q;

	for (;;) {
		glink_lock(&ns.qlock);
		if ((q = TAILQ_FIRST(&ns.queue)) != NULL)
			TAILQ_REMOVE(&ns.queue, q, link);
		glink_unlock(&ns.qlock);
		if (q == NULL)
			break;
		glink_lock(&ns.lock);
		ns_handle(q->src_node, q->src_port, &q->pkt);
		glink_unlock(&ns.lock);
		glink_free(q);
	}
}

static void
ns_rx(void *arg, const struct qrtr_rx_info *info, const void *data,
    size_t len)
{
	struct ns_pkt *q;

	if (len < 4)
		return;
	q = glink_zalloc(sizeof(*q));
	q->src_node = info->src_node;
	q->src_port = info->src_port;
	memcpy(&q->pkt, data, MIN(len, sizeof(q->pkt)));
	glink_lock(&ns.qlock);
	TAILQ_INSERT_TAIL(&ns.queue, q, link);
	glink_unlock(&ns.qlock);
	glink_work_queue(&ns.work);
}

void
qrtr_ns_servers(qrtr_server_cb_t *cb, void *arg)
{
	struct ns_server *s;

	glink_lock(&ns.lock);
	TAILQ_FOREACH(s, &ns.servers, link)
		cb(arg, s->service, s->instance, s->node, s->port);
	glink_unlock(&ns.lock);
}

/* --- setup --------------------------------------------------------------------------------- */

int
qrtr_init(void)
{
	struct qrtr_ctrl_pkt hello;
	int error;

	glink_mutex_init(&r.lock);
	glink_cond_init(&r.cond);
	TAILQ_INIT(&r.ports);
	TAILQ_INIT(&r.nodes);
	TAILQ_INIT(&r.nids);
	r.next_port = MIN_EPH_PORT;
	r.up = true;
	glink_mutex_init(&ns.lock);
	glink_mutex_init(&ns.qlock);
	TAILQ_INIT(&ns.servers);
	TAILQ_INIT(&ns.lookups);
	TAILQ_INIT(&ns.queue);
	glink_work_init(&ns.work, ns_work, NULL);
	error = qrtr_port_open(QRTR_PORT_CTRL, ns_rx, NULL, &ns.port);
	if (error != 0)
		return (error);
	/* to the nodes there are (none yet: each endpoint's own HELLO) */
	memset(&hello, 0, sizeof(hello));
	hello.cmd = QRTR_TYPE_HELLO;
	send_from(QRTR_LOCAL_NODE, QRTR_PORT_CTRL, QRTR_NODE_BCAST,
	    QRTR_PORT_CTRL, &hello, sizeof(hello));
	return (0);
}

/* With no endpoint and no other port left */
void
qrtr_fini(void)
{
	struct ns_server *s;
	struct ns_lookup *l;

	qrtr_port_close(ns.port);
	glink_work_drain(&ns.work);
	while ((s = TAILQ_FIRST(&ns.servers)) != NULL) {
		TAILQ_REMOVE(&ns.servers, s, link);
		glink_free(s);
	}
	while ((l = TAILQ_FIRST(&ns.lookups)) != NULL) {
		TAILQ_REMOVE(&ns.lookups, l, link);
		glink_free(l);
	}
	r.up = false;
}
