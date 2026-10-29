/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The RPM's GLINK edge and SMD-RPM requests (rpm.h), after Linux's
 * qcom_glink_rpm.c (the FIFOs), qcom_glink_native.c (the commands, the
 * edge without intents) and smd-rpm.c (requests and answers).
 */

#ifdef _KERNEL
#include <sys/param.h>
#include <sys/systm.h>
#include <machine/atomic.h>
#define	rpm_wmb()	wmb()
#define	rpm_rmb()	rmb()
#else
#include <errno.h>
#include <string.h>
#define	rpm_wmb()	__sync_synchronize()
#define	rpm_rmb()	__sync_synchronize()
#endif

#include "rpm.h"

#define	ALIGN8(x)	(((x) + 7u) & ~7u)
#define	ALIGN4(x)	(((x) + 3u) & ~3u)

static uint32_t
ram_rd(const struct rpm_edge *e, uint32_t off)
{
	return (e->ram[off / 4]);
}

static void
ram_wr(struct rpm_edge *e, uint32_t off, uint32_t v)
{
	e->ram[off / 4] = v;
}

#define	TAIL(f)		((f)->off)
#define	HEAD(f)		((f)->off + 4)
#define	DATA(f)		((f)->off + 8)

/* a word of a FIFO's data at off (any multiple of 4, wrapped) */
static uint32_t
fifo_rd(const struct rpm_edge *e, const struct rpm_fifo *f, uint32_t off)
{
	return (ram_rd(e, DATA(f) + off % f->len));
}

static void
fifo_wr(struct rpm_edge *e, const struct rpm_fifo *f, uint32_t off, uint32_t v)
{
	ram_wr(e, DATA(f) + off % f->len, v);
}

/*
 * Linux's channel id: the TX_DATA records of its requests still in the
 * transmit FIFO (it never clears what the RPM has read). Every 8-byte
 * offset is looked at; a request looks like this: TX_DATA with a small
 * channel id, intent 0, one chunk of a request's size, nothing left, the
 * service "req\0" with the chunk's length less its header.
 */
static void
find_lcid(struct rpm_edge *e)
{
	u_int count[64];
	uint32_t o, w, chunk, best, top = 0;

	memset(count, 0, sizeof(count));
	for (o = 0; o < e->tx.len; o += 8) {
		w = fifo_rd(e, &e->tx, o);
		chunk = fifo_rd(e, &e->tx, o + 8);
		if ((w & 0xffff) != RPM_GLINK_CMD_TX_DATA || (w >> 16) == 0 ||
		    (w >> 16) >= 64 || fifo_rd(e, &e->tx, o + 4) != 0 ||
		    chunk < 28 || chunk >= 256 ||
		    fifo_rd(e, &e->tx, o + 12) != 0 ||
		    fifo_rd(e, &e->tx, o + 16) != RPM_SERVICE_TYPE_REQUEST ||
		    fifo_rd(e, &e->tx, o + 20) != chunk - 8)
			continue;
		count[w >> 16]++;
		/* its message ids go on from Linux's */
		if (fifo_rd(e, &e->tx, o + 24) > top)
			top = fifo_rd(e, &e->tx, o + 24);
	}
	for (best = 1, o = 2; o < 64; o++)
		if (count[o] > count[best])
			best = o;
	e->lcid_seen = count[best];
	if (count[best] > 0) {
		e->lcid = (uint16_t)best;
		e->msg_id = top;
	}
}

int
rpm_edge_init(struct rpm_edge *e, volatile void *ram, uint32_t size,
    uint16_t lcid)
{
	uint32_t toc, n, i, id, off, len;
	struct rpm_fifo *f;

	memset(e, 0, sizeof(*e));
	e->ram = ram;
	e->size = size;
	if (size < RPM_TOC_SIZE || (size & 3) != 0)
		return (ENXIO);
	toc = size - RPM_TOC_SIZE;
	if (ram_rd(e, toc) != RPM_TOC_MAGIC)
		return (ENXIO);
	n = ram_rd(e, toc + 4);
	for (i = 0; i < n && 8 + 12 * (i + 1) <= RPM_TOC_SIZE; i++) {
		id = ram_rd(e, toc + 8 + 12 * i);
		off = ram_rd(e, toc + 12 + 12 * i);
		len = ram_rd(e, toc + 16 + 12 * i);
		/* Linux takes offset + size within the RAM; the data too */
		if (off > size || len > size - off || len < 8 + 64 ||
		    (off & 7) != 0 || (len & 7) != 0)
			continue;
		if (id == RPM_TX_FIFO_ID)
			f = &e->tx;
		else if (id == RPM_RX_FIFO_ID)
			f = &e->rx;
		else
			continue;
		f->off = off;
		/* Linux: the data's length is the entry's size */
		f->len = len;
		if (off + 8 + len > size)
			f->len = size - off - 8;
	}
	if (e->tx.len == 0 || e->rx.len == 0)
		return (ENXIO);
	e->lcid = 1;	/* Linux's first channel on this edge */
	find_lcid(e);
	if (lcid != 0)
		e->lcid = lcid;
	/* what the RPM sent before (Linux read it) is not ours */
	ram_wr(e, TAIL(&e->rx), ram_rd(e, HEAD(&e->rx)) % e->rx.len);
	return (0);
}

static uint32_t
tx_room(const struct rpm_edge *e)
{
	uint32_t h = ram_rd(e, HEAD(&e->tx)), t = ram_rd(e, TAIL(&e->tx));

	if (h >= e->tx.len || t >= e->tx.len)
		return (0);
	return (t <= h ? e->tx.len - h + t : t - h);
}

int
rpm_send(struct rpm_edge *e, uint32_t state, uint32_t type, uint32_t id,
    const uint32_t *kv, int npairs, uint32_t *msg_id)
{
	uint32_t m[7 + 3 * RPM_MAX_PAIRS], dlen, total, h, i;

	if (npairs < 1 || npairs > RPM_MAX_PAIRS)
		return (EINVAL);
	dlen = 4 * (7 + 3 * npairs);
	m[0] = RPM_SERVICE_TYPE_REQUEST;
	m[1] = dlen - 8;		/* the request and its pairs */
	m[2] = e->msg_id + 1;
	m[3] = state;
	m[4] = type;
	m[5] = id;
	m[6] = 12 * npairs;
	for (i = 0; i < (uint32_t)npairs; i++) {
		m[7 + 3 * i] = kv[2 * i];
		m[8 + 3 * i] = 4;
		m[9 + 3 * i] = kv[2 * i + 1];
	}
	total = ALIGN8(16 + dlen);
	/* a full FIFO would look empty: strictly less */
	if (tx_room(e) <= total)
		return (ENOSPC);
	h = ram_rd(e, HEAD(&e->tx));
	fifo_wr(e, &e->tx, h, RPM_GLINK_CMD_TX_DATA | (uint32_t)e->lcid << 16);
	fifo_wr(e, &e->tx, h + 4, 0);		/* no intent */
	fifo_wr(e, &e->tx, h + 8, dlen);	/* the chunk */
	fifo_wr(e, &e->tx, h + 12, 0);		/* nothing left */
	for (i = 0; i < dlen / 4; i++)
		fifo_wr(e, &e->tx, h + 16 + 4 * i, m[i]);
	if (total > 16 + dlen)
		fifo_wr(e, &e->tx, h + 16 + dlen, 0);
	rpm_wmb();
	ram_wr(e, HEAD(&e->tx), (h + total) % e->tx.len);
	rpm_wmb();
	e->msg_id = m[2];
	*msg_id = m[2];
	return (0);
}

/* the size of the command at the receive FIFO's tail, 0 if unknown */
static uint32_t
rx_cmd_size(const struct rpm_edge *e, uint32_t t, uint32_t w0, uint32_t w1)
{
	switch (w0 & 0xffff) {
	case RPM_GLINK_CMD_TX_DATA:
	case RPM_GLINK_CMD_TX_DATA_CONT:
		/* the header, the chunk; its size after the intent */
		return (ALIGN8(16 + fifo_rd(e, &e->rx, t + 8)));
	case RPM_GLINK_CMD_OPEN:
		/* the name, its length in param2 */
		return (8 + ALIGN8(w1 & 0xffff));
	case RPM_GLINK_CMD_INTENT:
		/* param2 intents of size and id */
		return (8 + 8 * (w1 & 0xff));
	default:
		/* VERSION, OPEN_ACK, CLOSE, RX_DONE, SIGNALS, ...: the header */
		return ((w0 & 0xffff) <= 15 ? 8 : 0);
	}
}

/* an answer's messages: 1 our msg_id acknowledged, -errno refused, 0 other */
static int
rx_answer(struct rpm_edge *e, uint32_t t, uint32_t chunk, uint32_t msg_id)
{
	uint32_t o, end, type, ml = 0, k, w;
	bool mine = false;
	int status = 1;
	char text[sizeof(e->err)];

	if (chunk < 8 || fifo_rd(e, &e->rx, t + 16) != RPM_SERVICE_TYPE_REQUEST)
		return (0);
	end = fifo_rd(e, &e->rx, t + 20);
	if (end > chunk - 8)
		end = chunk - 8;
	memset(text, 0, sizeof(text));
	for (o = 0; o + 8 <= end; o += 8 + ALIGN4(ml)) {
		type = fifo_rd(e, &e->rx, t + 24 + o);
		ml = fifo_rd(e, &e->rx, t + 28 + o);
		if (ml > end)
			break;
		if (type == RPM_MSG_TYPE_MSG_ID && ml >= 4)
			mine = fifo_rd(e, &e->rx, t + 32 + o) == msg_id;
		else if (type == RPM_MSG_TYPE_ERR) {
			for (k = 0; k < ml && k + 1 < sizeof(text); k++) {
				w = fifo_rd(e, &e->rx, t + 32 + o + (k & ~3u));
				text[k] = (char)(w >> (8 * (k & 3)));
			}
			text[k] = '\0';
			/* Linux's two kinds of refusal */
			status = strcmp(text, "resource does not exist") == 0 ?
			    -ENXIO : -EINVAL;
		}
	}
	if (!mine)
		return (0);
	if (status < 0)
		memcpy(e->err, text, sizeof(e->err));
	return (status);
}

int
rpm_poll(struct rpm_edge *e, uint32_t msg_id)
{
	uint32_t h, t, avail, w0, w1, adv;
	int r;

	for (;;) {
		h = ram_rd(e, HEAD(&e->rx));
		t = ram_rd(e, TAIL(&e->rx));
		if (h >= e->rx.len || t >= e->rx.len)
			return (0);
		avail = h >= t ? h - t : e->rx.len - t + h;
		if (avail < 8)
			return (0);
		rpm_rmb();
		w0 = fifo_rd(e, &e->rx, t);
		w1 = fifo_rd(e, &e->rx, t + 4);
		adv = rx_cmd_size(e, t, w0, w1);
		if (adv == 0 || adv > e->rx.len) {
			/* not GLINK as we know it: start over at the head */
			ram_wr(e, TAIL(&e->rx), h);
			return (0);
		}
		if (avail < adv)
			return (0);	/* the rest is still being written */
		r = 0;
		if ((w0 & 0xffff) == RPM_GLINK_CMD_TX_DATA)
			r = rx_answer(e, t, fifo_rd(e, &e->rx, t + 8), msg_id);
		ram_wr(e, TAIL(&e->rx), (t + adv) % e->rx.len);
		if (r != 0)
			return (r);
	}
}
