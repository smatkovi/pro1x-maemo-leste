/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * glink_core.c against itself on the PC: two edges, A and B, over two
 * 16 KB FIFOs laid out as glink_smem's (head and tail, 8-byte aligned
 * messages, 16 bytes kept free), each with a thread that plays its
 * interrupt. It checks opening from both sides and from one side after the
 * other announced, data in offered intents, intents asked for, messages
 * split into 8 KB chunks around the FIFO's end with the writer waiting for
 * space, both directions under load, and closing.
 *
 *   make -C freebsd/dsp/test
 */

#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <unistd.h>

#include "glink.h"

#define	FIFO_LEN	16384
#define	RESERVE		16

struct fifo {
	char		buf[FIFO_LEN];
	_Atomic uint32_t head, tail;
};

struct side {
	const char	*name;
	struct fifo	*rx, *tx;
	struct side	*peer;
	struct glink_edge *edge;
	sem_t		irq;
	pthread_t	thread;
	atomic_bool	stop;
	/* what arrived */
	pthread_mutex_t	lock;
	size_t		got_bytes;
	int		got_msgs;
	uint32_t	sum;
	char		announced[32];
	int		downs;
};

static size_t
rx_avail(void *arg)
{
	struct side *s = arg;
	uint32_t head = s->rx->head, tail = s->rx->tail;

	return (head >= tail ? head - tail : FIFO_LEN - tail + head);
}

static void
rx_peek(void *arg, void *buf, size_t off, size_t len)
{
	struct side *s = arg;
	size_t tail = (s->rx->tail + off) % FIFO_LEN, n;

	n = MIN(len, FIFO_LEN - tail);
	memcpy(buf, s->rx->buf + tail, n);
	memcpy((char *)buf + n, s->rx->buf, len - n);
}

static void
rx_advance(void *arg, size_t len)
{
	struct side *s = arg;

	s->rx->tail = (s->rx->tail + len) % FIFO_LEN;
}

static size_t
tx_avail(void *arg)
{
	struct side *s = arg;
	uint32_t head = s->tx->head, tail = s->tx->tail;
	size_t avail;

	avail = tail <= head ? FIFO_LEN - head + tail : tail - head;
	return (avail < RESERVE ? 0 : avail - RESERVE);
}

static size_t
write_one(struct side *s, size_t head, const void *data, size_t len)
{
	size_t n = MIN(len, FIFO_LEN - head);

	memcpy(s->tx->buf + head, data, n);
	memcpy(s->tx->buf, (const char *)data + n, len - n);
	return ((head + len) % FIFO_LEN);
}

static void
tx_write(void *arg, const void *hdr, size_t hlen, const void *data,
    size_t dlen)
{
	struct side *s = arg;
	size_t head = s->tx->head;

	head = write_one(s, head, hdr, hlen);
	if (dlen != 0)
		head = write_one(s, head, data, dlen);
	head = roundup2(head, 8) % FIFO_LEN;
	atomic_thread_fence(memory_order_release);
	s->tx->head = head;
}

static void
tx_kick(void *arg)
{
	struct side *s = arg;

	sem_post(&s->peer->irq);
}

static size_t
tx_length(void *arg)
{
	return (FIFO_LEN);
}

static const struct glink_fifo_ops ops = {
	rx_avail, rx_peek, rx_advance, tx_avail, tx_write, tx_kick, tx_length
};

static void *
irq_thread(void *arg)
{
	struct side *s = arg;

	while (!s->stop) {
		sem_wait(&s->irq);
		if (s->edge != NULL)
			glink_edge_rx(s->edge);
	}
	return (NULL);
}

static void
announce(void *arg, const char *name)
{
	struct side *s = arg;

	pthread_mutex_lock(&s->lock);
	snprintf(s->announced, sizeof(s->announced), "%s", name);
	pthread_mutex_unlock(&s->lock);
}

static void
rx(void *arg, const void *data, size_t len)
{
	struct side *s = arg;
	const unsigned char *p = data;
	size_t i;

	pthread_mutex_lock(&s->lock);
	s->got_bytes += len;
	s->got_msgs++;
	for (i = 0; i < len; i++)
		s->sum += p[i];
	pthread_mutex_unlock(&s->lock);
}

static void
state(void *arg, bool up)
{
	struct side *s = arg;

	if (!up)
		s->downs++;
}

struct opener {
	struct side	*s;
	const char	*name;
	struct glink_channel *ch;
	int		error;
};

static void *
open_thread(void *arg)
{
	struct opener *o = arg;

	o->error = glink_open(o->s->edge, o->name, 5, 1024, rx, state, o->s,
	    3000, &o->ch);
	return (NULL);
}

static uint32_t
fill(char *buf, size_t len, unsigned seed)
{
	uint32_t sum = 0;
	size_t i;

	for (i = 0; i < len; i++) {
		buf[i] = (char)(seed * 31 + i * 7);
		sum += (unsigned char)buf[i];
	}
	return (sum);
}

static void
wait_for(struct side *s, size_t bytes, int msgs)
{
	int i;

	for (i = 0; i < 1000; i++) {
		pthread_mutex_lock(&s->lock);
		if (s->got_bytes == bytes && s->got_msgs == msgs) {
			pthread_mutex_unlock(&s->lock);
			return;
		}
		pthread_mutex_unlock(&s->lock);
		usleep(5000);
	}
	fprintf(stderr, "%s: got %zu bytes in %d messages, want %zu in %d\n",
	    s->name, s->got_bytes, s->got_msgs, bytes, msgs);
	assert(0);
}

struct loader {
	struct glink_channel *ch;
	int		n;
	unsigned	seed;
	size_t		bytes;
	uint32_t	sum;
};

static void *
load_thread(void *arg)
{
	struct loader *l = arg;
	char *buf = malloc(20000);	/* one per thread */
	size_t len;
	int i;

	for (i = 0; i < l->n; i++) {
		len = (l->seed * 7919 + i * 104729) % 3000 + 1;
		if (i % 50 == 0)
			len = 12000;	/* now and then one for an asked intent */
		l->sum += fill(buf, len, l->seed + i);
		l->bytes += len;
		assert(glink_send(l->ch, buf, len) == 0);
	}
	free(buf);
	return (NULL);
}

int
main(void)
{
	static struct fifo ab, ba;
	struct side a = { .name = "A" }, b = { .name = "B" };
	struct opener oa = { &a, "IPCRTR", NULL, 0 }, ob = { &b, "IPCRTR", NULL, 0 };
	struct glink_channel *ach, *bch, *bipc;
	struct loader la, lb;
	pthread_t t1, t2;
	static char buf[40000];
	uint32_t sum;

	a.rx = &ba; a.tx = &ab; a.peer = &b;
	b.rx = &ab; b.tx = &ba; b.peer = &a;
	pthread_mutex_init(&a.lock, NULL);
	pthread_mutex_init(&b.lock, NULL);
	sem_init(&a.irq, 0, 0);
	sem_init(&b.irq, 0, 0);
	pthread_create(&a.thread, NULL, irq_thread, &a);
	pthread_create(&b.thread, NULL, irq_thread, &b);
	a.edge = glink_edge_create("a", &ops, &a, announce, &a);
	b.edge = glink_edge_create("b", &ops, &b, announce, &b);

	/* 1. both sides open the same channel at once */
	pthread_create(&t1, NULL, open_thread, &oa);
	pthread_create(&t2, NULL, open_thread, &ob);
	pthread_join(t1, NULL);
	pthread_join(t2, NULL);
	assert(oa.error == 0 && ob.error == 0);
	printf("open from both sides: ok\n");

	/* 2. small, in an offered intent; big, in an intent asked for; one
	 *    over 8 KB chunks and around the FIFO's end */
	sum = fill(buf, 100, 1);
	assert(glink_send(oa.ch, buf, 100) == 0);
	wait_for(&b, 100, 1);
	assert(b.sum == sum);
	sum = fill(buf, 5000, 2);
	assert(glink_send(ob.ch, buf, 5000) == 0);
	wait_for(&a, 5000, 1);
	assert(a.sum == sum);
	sum = fill(buf, 37000, 3);
	assert(glink_send(oa.ch, buf, 37000) == 0);
	wait_for(&b, 37100, 2);
	assert(b.sum == fill(buf, 100, 1) + sum);
	printf("offered, asked-for and chunked intents: ok\n");

	/* 3. load in both directions at once */
	a.got_bytes = b.got_bytes = 0;
	a.got_msgs = b.got_msgs = 0;
	a.sum = b.sum = 0;
	la = (struct loader){ oa.ch, 2000, 11, 0, 0 };
	lb = (struct loader){ ob.ch, 2000, 23, 0, 0 };
	pthread_create(&t1, NULL, load_thread, &la);
	pthread_create(&t2, NULL, load_thread, &lb);
	pthread_join(t1, NULL);
	pthread_join(t2, NULL);
	wait_for(&b, la.bytes, la.n);
	wait_for(&a, lb.bytes, lb.n);
	assert(b.sum == la.sum && a.sum == lb.sum);
	printf("load: %zu and %zu bytes in %d messages each way: ok\n",
	    la.bytes, lb.bytes, la.n);

	/* 4. the remote side announces a channel, ours opens it after */
	bipc = ob.ch;
	memset(a.announced, 0, sizeof(a.announced));
	ob.name = "apr_audio_svc";
	pthread_create(&t2, NULL, open_thread, &ob);
	while (strcmp(a.announced, "apr_audio_svc") != 0)
		usleep(1000);
	assert(glink_open(a.edge, "apr_audio_svc", 2, 512, rx, state, &a, 3000,
	    &ach) == 0);
	pthread_join(t2, NULL);
	assert(ob.error == 0);
	bch = ob.ch;
	a.got_bytes = 0;
	a.got_msgs = 0;
	assert(glink_send(bch, "hello", 6) == 0);
	wait_for(&a, 6, 1);
	printf("announced, then opened: ok\n");

	/* 5. closing: the other side hears it */
	glink_close(bch);
	while (a.downs == 0)
		usleep(1000);
	assert(glink_send(ach, "x", 1) == ENOTCONN);
	glink_close(ach);
	printf("close: ok\n");

	/* 6. the remote processor goes away with a channel still held: its
	 *    user hears it, sending fails, the edge goes with the last close */
	a.stop = b.stop = true;
	sem_post(&a.irq);
	sem_post(&b.irq);
	pthread_join(a.thread, NULL);
	pthread_join(b.thread, NULL);
	a.downs = 0;
	glink_edge_abort(a.edge);
	assert(a.downs == 1);
	glink_edge_destroy(a.edge);
	assert(glink_send(oa.ch, "x", 1) == ENOTCONN);
	glink_close(oa.ch);		/* frees edge a */
	glink_edge_abort(b.edge);
	glink_close(bipc);
	glink_edge_destroy(b.edge);	/* frees edge b */
	printf("remote gone with a channel held: ok\n");
	printf("all GLINK tests passed\n");
	return (0);
}
