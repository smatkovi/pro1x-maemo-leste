/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * qrtr_core.c on the PC, against a remote processor played by the test:
 * node 5 behind an endpoint whose packets the test reads (what we send it)
 * and writes (qrtr_endpoint_post: what it sends us). Checks the name
 * service (lookups and their end marker, HELLO, servers from the remote
 * and from here, DEL_CLIENT, BYE), data both ways with the v1 header, and
 * flow control: the 5th packet asks for RESUME_TX, the 11th waits for it,
 * a received confirm_rx is answered.
 */

#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/queue.h>

#include "qrtr.h"

struct hdr {
	uint32_t	version, type, src_node, src_port, confirm_rx, size,
			dst_node, dst_port;
};

/* --- the remote processor, node 5 --------------------------------------------- */

struct sent {
	TAILQ_ENTRY(sent) link;
	struct hdr	h;
	char		data[256];
};
static TAILQ_HEAD(, sent) sent = TAILQ_HEAD_INITIALIZER(sent);
static pthread_mutex_t sent_lock = PTHREAD_MUTEX_INITIALIZER;
static struct qrtr_node *node5;

static int
xmit(void *arg, const void *pkt, size_t len)
{
	struct sent *s = calloc(1, sizeof(*s));

	assert(len >= sizeof(struct hdr) && len % 4 == 0);
	memcpy(&s->h, pkt, sizeof(s->h));
	assert(s->h.version == 1 && len == sizeof(s->h) + roundup2(s->h.size, 4));
	memcpy(s->data, (const char *)pkt + sizeof(s->h), MIN(s->h.size,
	    sizeof(s->data)));
	pthread_mutex_lock(&sent_lock);
	TAILQ_INSERT_TAIL(&sent, s, link);
	pthread_mutex_unlock(&sent_lock);
	return (0);
}

/* The next packet we sent to node 5 (waits for it) */
static struct sent *
next_sent(void)
{
	struct sent *s;
	int i;

	for (i = 0; i < 400; i++) {
		pthread_mutex_lock(&sent_lock);
		if ((s = TAILQ_FIRST(&sent)) != NULL)
			TAILQ_REMOVE(&sent, s, link);
		pthread_mutex_unlock(&sent_lock);
		if (s != NULL)
			return (s);
		usleep(5000);
	}
	assert(!"nothing sent to node 5");
	return (NULL);
}

static int
sent_count(void)
{
	struct sent *s;
	int n = 0;

	pthread_mutex_lock(&sent_lock);
	TAILQ_FOREACH(s, &sent, link)
		n++;
	pthread_mutex_unlock(&sent_lock);
	return (n);
}

static void
from5(int type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port,
    int confirm, const void *data, size_t len)
{
	char buf[512];
	struct hdr h = { 1, type, 5, src_port, confirm, len, dst_node, dst_port };

	memset(buf, 0, sizeof(buf));
	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), data, len);
	assert(qrtr_endpoint_post(node5, buf, sizeof(h) + roundup2(len, 4)) == 0);
}

static void
ctrl5(uint32_t cmd, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	struct qrtr_ctrl_pkt p = { cmd, { { a, b, c, d } } };

	from5(cmd, QRTR_PORT_CTRL, QRTR_LOCAL_NODE, QRTR_PORT_CTRL, 0, &p,
	    sizeof(p));
}

/* --- local clients -------------------------------------------------------------- */

struct client {
	struct qrtr_port *port;
	pthread_mutex_t	lock;
	struct {
		uint32_t	node, port;
		struct qrtr_ctrl_pkt pkt;
		size_t		len;
	} rx[64];
	int		n;
};

static void
client_rx(void *arg, const struct qrtr_rx_info *info, const void *data,
    size_t len)
{
	struct client *c = arg;

	pthread_mutex_lock(&c->lock);
	assert(c->n < 64);
	c->rx[c->n].node = info->src_node;
	c->rx[c->n].port = info->src_port;
	memcpy(&c->rx[c->n].pkt, data, MIN(len, sizeof(c->rx[0].pkt)));
	c->rx[c->n].len = len;
	c->n++;
	pthread_mutex_unlock(&c->lock);
}

static void
client_open(struct client *c)
{
	memset(c, 0, sizeof(*c));
	pthread_mutex_init(&c->lock, NULL);
	assert(qrtr_port_open(0, client_rx, c, &c->port) == 0);
}

static void
wait_rx(struct client *c, int n)
{
	int i;

	for (i = 0; i < 400 && c->n < n; i++)
		usleep(5000);
	assert(c->n >= n);
}

static void
ctrl(struct client *c, uint32_t cmd, uint32_t service, uint32_t instance)
{
	struct qrtr_ctrl_pkt p = { cmd, { { service, instance, 0, 0 } } };

	assert(qrtr_sendto(c->port, QRTR_LOCAL_NODE, QRTR_PORT_CTRL, &p,
	    sizeof(p)) == 0);
}

struct burst {
	struct client	*c;
	int		n;
};

static void *
burst_thread(void *arg)
{
	struct burst *b = arg;
	int i;

	for (i = 0; i < b->n; i++)
		assert(qrtr_sendto(b->c->port, 5, 9, "data", 4) == 0);
	return (NULL);
}

int
main(void)
{
	struct client a, b, c;
	struct burst burst;
	struct sent *s;
	pthread_t t;
	int i;

	assert(qrtr_init() == 0);
	node5 = qrtr_endpoint_register(xmit, NULL);
	client_open(&a);
	client_open(&b);
	client_open(&c);

	/* 1. a lookup of a service nobody has: only the end marker */
	ctrl(&a, QRTR_TYPE_NEW_LOOKUP, 400, 0);
	wait_rx(&a, 1);
	assert(a.rx[0].node == QRTR_LOCAL_NODE && a.rx[0].port == QRTR_PORT_CTRL);
	assert(a.rx[0].pkt.cmd == QRTR_TYPE_NEW_SERVER &&
	    a.rx[0].pkt.server.service == 0);
	printf("lookup with the end marker: ok\n");

	/* 2. node 5 says HELLO, the name service answers */
	ctrl5(QRTR_TYPE_HELLO, 0, 0, 0, 0);
	s = next_sent();
	assert(s->h.type == QRTR_TYPE_HELLO && s->h.dst_node == 5 &&
	    s->h.dst_port == QRTR_PORT_CTRL && s->h.src_port == QRTR_PORT_CTRL);
	free(s);
	printf("HELLO answered: ok\n");

	/* 3. node 5 announces service 400 on its port 9: the lookup hears */
	ctrl5(QRTR_TYPE_NEW_SERVER, 400, 1, 5, 9);
	wait_rx(&a, 2);
	assert(a.rx[1].pkt.cmd == QRTR_TYPE_NEW_SERVER &&
	    a.rx[1].pkt.server.service == 400 && a.rx[1].pkt.server.node == 5 &&
	    a.rx[1].pkt.server.port == 9);
	ctrl(&c, QRTR_TYPE_NEW_LOOKUP, 400, 1);
	wait_rx(&c, 2);
	assert(c.rx[0].pkt.server.port == 9 && c.rx[1].pkt.server.service == 0);
	printf("remote server announced, both lookups see it: ok\n");

	/* 4. data to it: the v1 header; the 5th asks for RESUME_TX, the 11th
	 *    waits for one */
	burst.c = &a;
	burst.n = 11;
	pthread_create(&t, NULL, burst_thread, &burst);
	for (i = 1; i <= 10; i++) {
		s = next_sent();
		assert(s->h.type == QRTR_TYPE_DATA && s->h.src_node == 1 &&
		    s->h.src_port == qrtr_port_id(a.port) && s->h.dst_node == 5 &&
		    s->h.dst_port == 9 && s->h.size == 4 &&
		    memcmp(s->data, "data", 4) == 0);
		assert(s->h.confirm_rx == (i == 5));
		free(s);
	}
	usleep(100000);
	assert(sent_count() == 0);	/* the 11th waits */
	ctrl5(QRTR_TYPE_RESUME_TX, 1, qrtr_port_id(a.port), 0, 0);
	from5(QRTR_TYPE_RESUME_TX, 9, 1, qrtr_port_id(a.port), 0,
	    &(struct qrtr_ctrl_pkt){ QRTR_TYPE_RESUME_TX, .client = { 5, 9 } },
	    sizeof(struct qrtr_ctrl_pkt));
	pthread_join(t, NULL);
	s = next_sent();
	assert(s->h.type == QRTR_TYPE_DATA);
	free(s);
	printf("data with flow control: ok\n");

	/* 5. data from node 5 asking for a confirmation */
	from5(QRTR_TYPE_DATA, 9, 1, qrtr_port_id(b.port), 1, "pong", 4);
	wait_rx(&b, 1);
	assert(b.rx[0].node == 5 && b.rx[0].port == 9 && b.rx[0].len == 4);
	s = next_sent();
	assert(s->h.type == QRTR_TYPE_RESUME_TX && s->h.dst_node == 5 &&
	    s->h.dst_port == 9);
	free(s);
	printf("received data confirmed: ok\n");

	/* 6. a server here: announced to node 5 */
	ctrl(&b, QRTR_TYPE_NEW_SERVER, 15, 2);
	s = next_sent();
	assert(s->h.type == QRTR_TYPE_NEW_SERVER);
	assert(((struct qrtr_ctrl_pkt *)s->data)->server.service == 15 &&
	    ((struct qrtr_ctrl_pkt *)s->data)->server.node == 1 &&
	    ((struct qrtr_ctrl_pkt *)s->data)->server.port ==
	    qrtr_port_id(b.port));
	free(s);
	printf("local server announced: ok\n");

	/* 7. a client goes: DEL_CLIENT to node 5 and to the local server */
	i = qrtr_port_id(a.port);
	qrtr_port_close(a.port);
	s = next_sent();
	assert(s->h.type == QRTR_TYPE_DEL_CLIENT &&
	    ((struct qrtr_ctrl_pkt *)s->data)->client.node == 1);
	free(s);
	wait_rx(&b, 2);
	assert(b.rx[1].port == QRTR_PORT_CTRL &&
	    b.rx[1].pkt.cmd == QRTR_TYPE_DEL_CLIENT &&
	    b.rx[1].pkt.client.port == (uint32_t)i);
	printf("closed port reported: ok\n");

	/* 8. node 5 goes: its server leaves the lookups, the local server
	 *    hears a BYE */
	qrtr_endpoint_unregister(node5);
	wait_rx(&c, 3);
	assert(c.rx[2].pkt.cmd == QRTR_TYPE_DEL_SERVER &&
	    c.rx[2].pkt.server.service == 400);
	wait_rx(&b, 3);
	assert(b.rx[2].port == QRTR_PORT_CTRL && b.rx[2].pkt.cmd == QRTR_TYPE_BYE &&
	    b.rx[2].pkt.client.node == 5);
	printf("remote node gone: ok\n");

	qrtr_port_close(b.port);
	qrtr_port_close(c.port);
	qrtr_fini();
	printf("all QRTR tests passed\n");
	return (0);
}
