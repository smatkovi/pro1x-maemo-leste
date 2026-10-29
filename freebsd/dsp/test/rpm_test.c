/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * rpm_core.c on the PC against an RPM in RAM: a message RAM of the
 * SM6115's size (28 KiB) with its table of contents at the end and the two
 * FIFOs, the transmit FIFO full of requests Linux left behind (its channel
 * id, its message ids, wrapped around the FIFO's end), stale answers in the
 * receive FIFO. The fake RPM reads what rpm_core.c sends the way the RPM's
 * GLINK does and answers as smd-rpm.c expects: acknowledgements, refusals,
 * answers to other ids first, answers written in two steps, GLINK commands
 * that are not data, a full FIFO.
 *
 *   make -C freebsd/dsp/test
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpm.h"

#define	RAM_SIZE	0x7000		/* sram@45f0000 of the Pro1X's DT */
#define	TX_OFF		0x0400
#define	TX_LEN		0x0400		/* small, so that it wraps */
#define	RX_OFF		0x1000
#define	RX_LEN		0x0400

static uint32_t ram[RAM_SIZE / 4];
static int failures;

#define	CHECK(c) do {							\
	if (!(c)) {							\
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);	\
		failures++;						\
	}								\
} while (0)

static uint32_t *w(uint32_t off) { return (&ram[off / 4]); }

/* --- the RPM's side --------------------------------------------------------- */

struct fake {
	uint32_t	off, len;	/* a FIFO */
};
static struct fake ftx = { TX_OFF, TX_LEN }, frx = { RX_OFF, RX_LEN };

static uint32_t
frd(struct fake *f, uint32_t o)
{
	return (*w(f->off + 8 + o % f->len));
}

static void
fwr(struct fake *f, uint32_t o, uint32_t v)
{
	*w(f->off + 8 + o % f->len) = v;
}

static void
make_toc(void)
{
	uint32_t toc = RAM_SIZE - RPM_TOC_SIZE;

	memset(ram, 0, sizeof(ram));
	*w(toc) = RPM_TOC_MAGIC;
	*w(toc + 4) = 3;
	/* an entry of something else first */
	*w(toc + 8) = 0x12345678;
	*w(toc + 12) = 0x2000;
	*w(toc + 16) = 0x100;
	*w(toc + 20) = RPM_TX_FIFO_ID;
	*w(toc + 24) = TX_OFF;
	*w(toc + 28) = TX_LEN;
	*w(toc + 32) = RPM_RX_FIFO_ID;
	*w(toc + 36) = RX_OFF;
	*w(toc + 40) = RX_LEN;
}

/* one request as Linux writes it, at o in the transmit FIFO: its size */
static uint32_t
linux_request(uint32_t o, uint16_t lcid, uint32_t id)
{
	uint32_t dlen = 8 + 20 + 12;

	fwr(&ftx, o, RPM_GLINK_CMD_TX_DATA | (uint32_t)lcid << 16);
	fwr(&ftx, o + 4, 0);
	fwr(&ftx, o + 8, dlen);
	fwr(&ftx, o + 12, 0);
	fwr(&ftx, o + 16, RPM_SERVICE_TYPE_REQUEST);
	fwr(&ftx, o + 20, dlen - 8);
	fwr(&ftx, o + 24, id);
	fwr(&ftx, o + 28, RPM_ACTIVE_STATE);
	fwr(&ftx, o + 32, 0x6b6c63);	/* "clk", a clock vote */
	fwr(&ftx, o + 36, 2);
	fwr(&ftx, o + 40, 12);
	fwr(&ftx, o + 44, 0x5a484b);	/* "KHz" */
	fwr(&ftx, o + 48, 4);
	fwr(&ftx, o + 52, 19200);
	return ((16 + dlen + 7) & ~7u);
}

struct request {
	uint32_t	lcid, id, state, type, res, npairs;
	uint32_t	kv[2 * RPM_MAX_PAIRS];
};

/* the next request in the transmit FIFO, as the RPM reads it: 1, or 0 */
static int
rpm_read(struct request *r)
{
	uint32_t t = *w(TX_OFF), h = *w(TX_OFF + 4), w0, chunk, i;

	if (t == h)
		return (0);
	w0 = frd(&ftx, t);
	assert((w0 & 0xffff) == RPM_GLINK_CMD_TX_DATA);
	CHECK(frd(&ftx, t + 4) == 0);		/* no intent */
	chunk = frd(&ftx, t + 8);
	CHECK(frd(&ftx, t + 12) == 0);		/* in one chunk */
	CHECK(frd(&ftx, t + 16) == RPM_SERVICE_TYPE_REQUEST);
	CHECK(frd(&ftx, t + 20) == chunk - 8);
	r->lcid = w0 >> 16;
	r->id = frd(&ftx, t + 24);
	r->state = frd(&ftx, t + 28);
	r->type = frd(&ftx, t + 32);
	r->res = frd(&ftx, t + 36);
	r->npairs = frd(&ftx, t + 40) / 12;
	CHECK(frd(&ftx, t + 40) == chunk - 28);
	for (i = 0; i < r->npairs; i++) {
		r->kv[2 * i] = frd(&ftx, t + 44 + 12 * i);
		CHECK(frd(&ftx, t + 48 + 12 * i) == 4);
		r->kv[2 * i + 1] = frd(&ftx, t + 52 + 12 * i);
	}
	/* 8-byte aligned, and the head at the end of it */
	*w(TX_OFF) = (t + ((16 + chunk + 7) & ~7u)) % TX_LEN;
	return (1);
}

/* an answer into the receive FIFO: msg# id, and err text if text */
static uint32_t
rpm_answer_at(uint32_t h, uint32_t id, const char *text)
{
	uint32_t data[32], n = 0, i, tl;

	data[n++] = RPM_SERVICE_TYPE_REQUEST;
	data[n++] = 0;
	data[n++] = RPM_MSG_TYPE_MSG_ID;
	data[n++] = 4;
	data[n++] = id;
	if (text != NULL) {
		tl = strlen(text) + 1;
		data[n++] = RPM_MSG_TYPE_ERR;
		data[n++] = tl;
		memset(&data[n], 0, (tl + 3) & ~3u);
		memcpy(&data[n], text, tl);
		n += (tl + 3) / 4;
	}
	data[1] = 4 * n - 8;
	fwr(&frx, h, RPM_GLINK_CMD_TX_DATA | 7u << 16);	/* the RPM's id */
	fwr(&frx, h + 4, 0);
	fwr(&frx, h + 8, 4 * n);
	fwr(&frx, h + 12, 0);
	for (i = 0; i < n; i++)
		fwr(&frx, h + 16 + 4 * i, data[i]);
	return ((h + ((16 + 4 * n + 7) & ~7u)) % RX_LEN);
}

static void
rpm_answer(uint32_t id, const char *text)
{
	*w(RX_OFF + 4) = rpm_answer_at(*w(RX_OFF + 4), id, text);
}

/* --- tests ----------------------------------------------------------------- */

static void
test_no_toc(void)
{
	struct rpm_edge e;

	memset(ram, 0, sizeof(ram));
	CHECK(rpm_edge_init(&e, ram, RAM_SIZE, 0) == ENXIO);
	make_toc();
	*w(RAM_SIZE - RPM_TOC_SIZE + 32) = 0;	/* no receive FIFO */
	CHECK(rpm_edge_init(&e, ram, RAM_SIZE, 0) == ENXIO);
	printf("ok: no table of contents, no FIFO\n");
}

static void
test_linux_left(struct rpm_edge *e)
{
	uint32_t o, id, h;

	make_toc();
	/*
	 * Linux's requests on channel 3 all over the transmit FIFO, the
	 * newest wrapping around its end; one on channel 5 and garbage
	 */
	h = linux_request(TX_LEN - 0x18, 3, 400);
	h = (TX_LEN - 0x18 + h) % TX_LEN;
	linux_request(h, 5, 99);
	fwr(&ftx, h + 56, 0xdeadbeef);
	for (o = h + 64, id = 100; o + 56 <= TX_LEN - 0x18; id++)
		o += linux_request(o, 3, id);
	*w(TX_OFF) = h;			/* the RPM read everything */
	*w(TX_OFF + 4) = h;
	/* stale answers in the receive FIFO */
	*w(RX_OFF) = 0x10;
	*w(RX_OFF + 4) = rpm_answer_at(0x10, 7, NULL);

	CHECK(rpm_edge_init(e, ram, RAM_SIZE, 0) == 0);
	CHECK(e->lcid == 3);
	CHECK(e->lcid_seen >= 10);
	CHECK(e->msg_id == 400);
	CHECK(*w(RX_OFF) == *w(RX_OFF + 4));	/* the old answers dropped */
	printf("ok: Linux's channel %u from %u of its requests, ids from %u\n",
	    e->lcid, e->lcid_seen, e->msg_id);
}

static void
test_ack(struct rpm_edge *e)
{
	uint32_t kv[] = { RPM_KEY_SWEN, 1, RPM_KEY_UV, 400000 }, id;
	struct request r;

	CHECK(rpm_send(e, RPM_ACTIVE_STATE, RPM_RES_LDOA, 8, kv, 2, &id) == 0);
	CHECK(id == 401);
	CHECK(rpm_poll(e, id) == 0);		/* not answered yet */
	CHECK(rpm_read(&r) == 1);
	CHECK(r.lcid == 3 && r.id == id && r.state == RPM_ACTIVE_STATE);
	CHECK(r.type == RPM_RES_LDOA && r.res == 8 && r.npairs == 2);
	CHECK(r.kv[0] == RPM_KEY_SWEN && r.kv[1] == 1);
	CHECK(r.kv[2] == RPM_KEY_UV && r.kv[3] == 400000);
	CHECK(rpm_read(&r) == 0);
	rpm_answer(id, NULL);
	CHECK(rpm_poll(e, id) == 1);
	CHECK(*w(RX_OFF) == *w(RX_OFF + 4));
	printf("ok: ldoa 8 swen 1 uv 400000 sent as Linux sends it, acknowledged\n");
}

static void
test_refused(struct rpm_edge *e)
{
	uint32_t kv[] = { RPM_KEY_SWEN, 1 }, id;
	struct request r;

	CHECK(rpm_send(e, RPM_ACTIVE_STATE, RPM_RES_LDOA, 99, kv, 1, &id) == 0);
	CHECK(rpm_read(&r) == 1 && r.res == 99 && r.npairs == 1);
	rpm_answer(id, "resource does not exist");
	CHECK(rpm_poll(e, id) == -ENXIO);
	CHECK(strcmp(e->err, "resource does not exist") == 0);
	CHECK(rpm_send(e, RPM_ACTIVE_STATE, RPM_RES_LDOA, 8, kv, 1, &id) == 0);
	CHECK(rpm_read(&r) == 1);
	rpm_answer(id, "invalid key");
	CHECK(rpm_poll(e, id) == -EINVAL);
	CHECK(strcmp(e->err, "invalid key") == 0);
	printf("ok: refusals (%s)\n", e->err);
}

static void
test_others_first(struct rpm_edge *e)
{
	uint32_t kv[] = { RPM_KEY_SWEN, 0 }, id, h, end, hdr;
	struct request r;

	CHECK(rpm_send(e, RPM_ACTIVE_STATE, RPM_RES_LDOA, 16, kv, 1, &id) == 0);
	CHECK(rpm_read(&r) == 1 && r.kv[1] == 0);
	h = *w(RX_OFF + 4);
	/* a VERSION, an OPEN with its name, an INTENT, an old answer */
	fwr(&frx, h, 0 | 1u << 16);
	fwr(&frx, h + 4, 0);
	h += 8;
	fwr(&frx, h, RPM_GLINK_CMD_OPEN | 9u << 16);
	fwr(&frx, h + 4, 13);		/* "rpm_requests" with its NUL */
	h += 8 + 16;
	fwr(&frx, h, RPM_GLINK_CMD_INTENT | 9u << 16);
	fwr(&frx, h + 4, 2);
	h += 8 + 16;
	h = rpm_answer_at(h % RX_LEN, id - 1, "old");
	*w(RX_OFF + 4) = h;
	CHECK(rpm_poll(e, id) == 0);
	CHECK(*w(RX_OFF) == h);		/* all of it read */
	/* ours, written in two steps: the header first */
	end = rpm_answer_at(h, id, NULL);
	hdr = (h + 16) % RX_LEN;
	*w(RX_OFF + 4) = hdr;
	CHECK(rpm_poll(e, id) == 0);
	CHECK(*w(RX_OFF) == h);		/* nothing taken from a half */
	*w(RX_OFF + 4) = end;
	CHECK(rpm_poll(e, id) == 1);
	printf("ok: other commands and answers passed over, a half answer waited for\n");
}

static void
test_wrap_and_full(struct rpm_edge *e)
{
	uint32_t kv[] = { RPM_KEY_SWEN, 1, RPM_KEY_UV, 1800000 }, id, i, n;
	struct request r;
	int err = 0;

	/* many requests: the transmit FIFO wraps several times */
	for (i = 0; i < 100; i++) {
		CHECK(rpm_send(e, RPM_ACTIVE_STATE, RPM_RES_LDOA, 1 + i % 24,
		    kv, 2, &id) == 0);
		CHECK(rpm_read(&r) == 1);
		CHECK(r.id == id && r.res == 1 + i % 24 && r.kv[3] == 1800000);
		rpm_answer(id, NULL);
		CHECK(rpm_poll(e, id) == 1);
	}
	/* the RPM reads nothing: full after (TX_LEN - 1) / 72 of them */
	for (n = 0; n < 100 && (err = rpm_send(e, RPM_ACTIVE_STATE,
	    RPM_RES_LDOA, 1, kv, 2, &id)) == 0; n++)
		;
	CHECK(err == ENOSPC);
	CHECK(n == (TX_LEN - 1) / 72);
	while (rpm_read(&r) == 1)
		;
	CHECK(rpm_send(e, RPM_ACTIVE_STATE, RPM_RES_LDOA, 1, kv, 2, &id) == 0);
	CHECK(rpm_send(e, RPM_ACTIVE_STATE, RPM_RES_LDOA, 1, kv, 0, &id) ==
	    EINVAL);
	printf("ok: 100 requests around the FIFO's end, full after %u\n", n);
}

static void
test_lcid_given(void)
{
	struct rpm_edge e;

	make_toc();
	CHECK(rpm_edge_init(&e, ram, RAM_SIZE, 0) == 0);
	CHECK(e.lcid == 1 && e.lcid_seen == 0 && e.msg_id == 0);
	linux_request(0x100, 2, 7);
	CHECK(rpm_edge_init(&e, ram, RAM_SIZE, 4) == 0);
	CHECK(e.lcid == 4 && e.lcid_seen == 1);
	printf("ok: no requests of Linux's: channel 1; one given wins\n");
}

int
main(void)
{
	struct rpm_edge e;

	test_no_toc();
	test_linux_left(&e);
	test_ack(&e);
	test_refused(&e);
	test_others_first(&e);
	test_wrap_and_full(&e);
	test_lcid_given();
	if (failures != 0) {
		printf("%d failures\n", failures);
		return (1);
	}
	printf("rpm_test: all passed\n");
	return (0);
}
