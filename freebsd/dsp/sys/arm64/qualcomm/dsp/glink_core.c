/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The GLINK protocol (glink.h), after Linux's qcom_glink_native.c. Every
 * message starts with { u16 cmd; u16 param1; u32 param2 } and takes a
 * multiple of 8 bytes in the FIFO.
 *
 * One mutex per edge guards everything; the receive context (glink_edge_rx)
 * and the callers of glink_open/send/close take it. Whoever waits (for FIFO
 * space, an acknowledgement, an intent) waits on the edge's condition with
 * the mutex released, and looks again at least every 10 ms, so that the
 * receive context may itself wait for FIFO space.
 */

#include "glink.h"

#ifdef _KERNEL
MALLOC_DEFINE(M_GLINK, "glink", "Qualcomm GLINK");
#else
#include <sys/queue.h>
#endif
#ifndef TAILQ_FOREACH_SAFE
#define	TAILQ_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = TAILQ_FIRST((head));				\
	    (var) && ((tvar) = TAILQ_NEXT((var), field), 1);		\
	    (var) = (tvar))
#endif

#define	NAME_SIZE		32
#define	VERSION_1		1
#define	FEATURE_INTENT_REUSE	0x1

#define	CMD_VERSION		0
#define	CMD_VERSION_ACK		1
#define	CMD_OPEN		2
#define	CMD_CLOSE		3
#define	CMD_OPEN_ACK		4
#define	CMD_INTENT		5
#define	CMD_RX_DONE		6
#define	CMD_RX_INTENT_REQ	7
#define	CMD_RX_INTENT_REQ_ACK	8
#define	CMD_TX_DATA		9
#define	CMD_CLOSE_ACK		11
#define	CMD_TX_DATA_CONT	12
#define	CMD_READ_NOTIF		13
#define	CMD_RX_DONE_W_REUSE	14
#define	CMD_SIGNALS		15

#define	CHUNK_MAX		8192
#define	POLL_MS			10
#define	TX_TIMEOUT_MS		10000
#define	INTENT_TIMEOUT_MS	10000

struct glink_msg {
	uint16_t	cmd;
	uint16_t	param1;
	uint32_t	param2;
} __attribute__((__packed__));

struct glink_data_hdr {
	struct glink_msg msg;
	uint32_t	chunk_size;
	uint32_t	left_size;
} __attribute__((__packed__));

/* A buffer we offer the remote for receiving */
struct lintent {
	TAILQ_ENTRY(lintent) link;
	uint32_t	id;
	size_t		size;
	size_t		offset;		/* of the message being received */
	bool		reuse;
	char		*data;
};

/* A buffer the remote offers us */
struct rintent {
	TAILQ_ENTRY(rintent) link;
	uint32_t	id;
	size_t		size;
	bool		in_use;
};

struct glink_channel {
	TAILQ_ENTRY(glink_channel) link;
	struct glink_edge *edge;
	char		name[NAME_SIZE];
	u_int		lcid;		/* ours, 0: not opened here */
	u_int		rcid;		/* the remote's, 0: not opened there */
	bool		open_acked;	/* the remote acknowledged our open */
	bool		acked_remote;	/* we acknowledged the remote's */
	bool		user;		/* glink_open holds it */
	bool		up;
	glink_rx_cb_t	*rx;
	glink_state_cb_t *state;
	void		*arg;
	uint32_t	next_liid;
	TAILQ_HEAD(, lintent) lintents;
	TAILQ_HEAD(, rintent) rintents;
	int		intent_req;	/* -1 asked, 0 refused, 1 granted */
	bool		intent_received;
	bool		adopted;	/* Linux's, taken over */
	uint32_t	hint;		/* adopted: glink_adopt_channel's */
	char		*bounce;	/* adopted: data into Linux's intents */
	size_t		bounce_size, bounce_off;
	uint32_t	bounce_iid;
	u_int		rx_msgs, tx_msgs, bounced;
};

struct glink_edge {
	char		label[32];
	const struct glink_fifo_ops *ops;
	void		*ops_arg;
	glink_announce_cb_t *announce;
	void		*announce_arg;
	glink_mutex_t	lock;
	glink_cond_t	cond;
	TAILQ_HEAD(, glink_channel) channels;
	u_int		next_lcid;
	uint32_t	features;
	bool		aborted;
	bool		destroy;	/* freed with the last channel */
	bool		read_notify_sent;
	bool		bad;		/* an unknown command: the FIFO is lost */
	bool		adopted;	/* Linux's edge, taken over */
	bool		remote_version;	/* VERSION or VERSION_ACK seen */
	u_int		rx_msgs, tx_msgs;
};

/* --- the transport --------------------------------------------------------------- */

static size_t
align8(size_t n)
{
	return (roundup2(n, 8));
}

/* Write a message, waiting for FIFO space; with the lock held */
static int
tx(struct glink_edge *e, const void *hdr, size_t hlen, const void *data,
    size_t dlen)
{
	struct glink_msg notif;
	uint64_t end;
	size_t len = hlen + dlen;

	if (len >= e->ops->tx_length(e->ops_arg))
		return (EINVAL);
	end = glink_now_ms() + TX_TIMEOUT_MS;
	while (e->ops->tx_avail(e->ops_arg) < align8(len)) {
		if (e->aborted)
			return (EIO);
		if (glink_now_ms() >= end)
			return (GLINK_TIMEDOUT);
		if (!e->read_notify_sent) {
			/* the FIFO reserves room for this one */
			memset(&notif, 0, sizeof(notif));
			notif.cmd = CMD_READ_NOTIF;
			e->ops->tx_write(e->ops_arg, &notif, sizeof(notif), NULL, 0);
			e->ops->tx_kick(e->ops_arg);
			e->read_notify_sent = true;
		}
		glink_cond_wait(&e->cond, &e->lock, POLL_MS);
	}
	e->read_notify_sent = false;
	e->ops->tx_write(e->ops_arg, hdr, hlen, data, dlen);
	e->ops->tx_kick(e->ops_arg);
	e->tx_msgs++;
	return (0);
}

static int
tx_cmd(struct glink_edge *e, uint16_t cmd, uint16_t p1, uint32_t p2)
{
	struct glink_msg m = { cmd, p1, p2 };

	return (tx(e, &m, sizeof(m), NULL, 0));
}

static int
send_version(struct glink_edge *e, uint16_t cmd)
{
	return (tx_cmd(e, cmd, VERSION_1, e->features));
}

static int
send_open(struct glink_edge *e, struct glink_channel *ch)
{
	struct {
		struct glink_msg msg;
		char name[NAME_SIZE];
	} __attribute__((__packed__)) req;
	size_t nlen = strlen(ch->name) + 1;

	memset(&req, 0, sizeof(req));
	req.msg.cmd = CMD_OPEN;
	req.msg.param1 = ch->lcid;
	req.msg.param2 = nlen;
	memcpy(req.name, ch->name, nlen);
	return (tx(e, &req, align8(sizeof(req.msg) + nlen), NULL, 0));
}

static int
advertise(struct glink_edge *e, struct glink_channel *ch, struct lintent *li)
{
	struct {
		struct glink_msg msg;
		uint32_t size;
		uint32_t iid;
	} __attribute__((__packed__)) cmd;

	cmd.msg.cmd = CMD_INTENT;
	cmd.msg.param1 = ch->lcid;
	cmd.msg.param2 = 1;
	cmd.size = li->size;
	cmd.iid = li->id;
	return (tx(e, &cmd, sizeof(cmd), NULL, 0));
}

/* --- channels ----------------------------------------------------------------------- */

static struct glink_channel *
ch_by_name(struct glink_edge *e, const char *name)
{
	struct glink_channel *ch;

	TAILQ_FOREACH(ch, &e->channels, link)
		if (strcmp(ch->name, name) == 0)
			return (ch);
	return (NULL);
}

static struct glink_channel *
ch_by_rcid(struct glink_edge *e, u_int rcid)
{
	struct glink_channel *ch;

	TAILQ_FOREACH(ch, &e->channels, link)
		if (ch->rcid == rcid && rcid != 0)
			return (ch);
	return (NULL);
}

static struct glink_channel *
ch_by_lcid(struct glink_edge *e, u_int lcid)
{
	struct glink_channel *ch;

	TAILQ_FOREACH(ch, &e->channels, link)
		if (ch->lcid == lcid && lcid != 0)
			return (ch);
	return (NULL);
}

static struct glink_channel *
ch_new(struct glink_edge *e, const char *name)
{
	struct glink_channel *ch;

	ch = glink_zalloc(sizeof(*ch));
	ch->edge = e;
	strncpy(ch->name, name, NAME_SIZE - 1);
	ch->next_liid = 1;
	TAILQ_INIT(&ch->lintents);
	TAILQ_INIT(&ch->rintents);
	TAILQ_INSERT_TAIL(&e->channels, ch, link);
	return (ch);
}

static void
ch_flush_rintents(struct glink_channel *ch)
{
	struct rintent *ri;

	while ((ri = TAILQ_FIRST(&ch->rintents)) != NULL) {
		TAILQ_REMOVE(&ch->rintents, ri, link);
		glink_free(ri);
	}
}

static void
ch_flush_lintents(struct glink_channel *ch)
{
	struct lintent *li;

	while ((li = TAILQ_FIRST(&ch->lintents)) != NULL) {
		TAILQ_REMOVE(&ch->lintents, li, link);
		glink_free(li->data);
		glink_free(li);
	}
}

/* Gone on both sides and unused: forget it */
static void
ch_maybe_free(struct glink_edge *e, struct glink_channel *ch)
{
	if (ch->user || ch->lcid != 0 || ch->rcid != 0)
		return;
	TAILQ_REMOVE(&e->channels, ch, link);
	ch_flush_rintents(ch);
	ch_flush_lintents(ch);
	glink_free(ch->bounce);
	glink_free(ch);
}

static struct lintent *
lintent_new(struct glink_channel *ch, size_t size, bool reuse)
{
	struct lintent *li;

	li = glink_zalloc(sizeof(*li));
	li->data = glink_zalloc(size);
	li->size = size;
	li->reuse = reuse;
	li->id = ch->next_liid++;
	TAILQ_INSERT_TAIL(&ch->lintents, li, link);
	return (li);
}

/* --- receiving ------------------------------------------------------------------------ */

static void
rx_open(struct glink_edge *e, u_int rcid, const char *name)
{
	struct glink_channel *ch;

	if ((ch = ch_by_name(e, name)) == NULL)
		ch = ch_new(e, name);
	ch->rcid = rcid;
	if (ch->user && !ch->acked_remote) {
		ch->acked_remote = true;
		tx_cmd(e, CMD_OPEN_ACK, ch->rcid, 0);
	}
	glink_broadcast(&e->cond);
	if (!ch->user && e->announce != NULL)
		e->announce(e->announce_arg, ch->name);
}

static void
rx_close(struct glink_edge *e, u_int rcid)
{
	struct glink_channel *ch;
	bool was_up;

	if ((ch = ch_by_rcid(e, rcid)) == NULL)
		return;
	tx_cmd(e, CMD_CLOSE_ACK, ch->rcid, 0);
	was_up = ch->up;
	ch->rcid = 0;
	ch->acked_remote = false;
	ch->up = false;
	ch_flush_rintents(ch);
	glink_broadcast(&e->cond);
	if (was_up && ch->user && ch->state != NULL)
		ch->state(ch->arg, false);
	ch_maybe_free(e, ch);
}

static void
rx_close_ack(struct glink_edge *e, u_int lcid)
{
	struct glink_channel *ch;

	if ((ch = ch_by_lcid(e, lcid)) == NULL)
		return;
	ch->lcid = 0;
	ch->open_acked = false;
	glink_broadcast(&e->cond);
	ch_maybe_free(e, ch);
}

static void
rx_intent_req(struct glink_edge *e, u_int rcid, size_t size)
{
	struct glink_channel *ch;
	struct lintent *li;

	if ((ch = ch_by_rcid(e, rcid)) == NULL || ch->lcid == 0)
		return;
	li = lintent_new(ch, size, false);
	advertise(e, ch, li);
	tx_cmd(e, CMD_RX_INTENT_REQ_ACK, ch->lcid, 1);
}

static void
rx_intents(struct glink_edge *e, u_int rcid, u_int count)
{
	struct glink_channel *ch;
	struct rintent *ri;
	uint32_t pair[2];
	u_int i;

	ch = ch_by_rcid(e, rcid);
	for (i = 0; ch != NULL && i < count; i++) {
		e->ops->rx_peek(e->ops_arg, pair, sizeof(struct glink_msg) +
		    i * sizeof(pair), sizeof(pair));
		ri = glink_zalloc(sizeof(*ri));
		ri->size = pair[0];
		ri->id = pair[1];
		TAILQ_INSERT_TAIL(&ch->rintents, ri, link);
	}
	if (ch != NULL) {
		ch->intent_received = true;
		glink_broadcast(&e->cond);
	}
}

static void
rx_done(struct glink_edge *e, u_int rcid, uint32_t iid, bool reuse)
{
	struct glink_channel *ch;
	struct rintent *ri;

	if ((ch = ch_by_rcid(e, rcid)) == NULL)
		return;
	TAILQ_FOREACH(ri, &ch->rintents, link)
		if (ri->id == iid)
			break;
	if (ri == NULL)
		return;
	ri->in_use = false;
	if (!reuse) {
		TAILQ_REMOVE(&ch->rintents, ri, link);
		glink_free(ri);
	} else
		ch->intent_received = true;
	glink_broadcast(&e->cond);
}

/* TX_DATA(_CONT): 0, or EAGAIN when the chunk is not all there yet */
static int
rx_data(struct glink_edge *e, size_t avail)
{
	struct glink_data_hdr h;
	struct glink_channel *ch;
	struct lintent *li = NULL;

	if (avail < sizeof(h))
		return (EAGAIN);
	e->ops->rx_peek(e->ops_arg, &h, 0, sizeof(h));
	if (avail < sizeof(h) + h.chunk_size)
		return (EAGAIN);
	if ((ch = ch_by_rcid(e, h.msg.param1)) != NULL) {
		TAILQ_FOREACH(li, &ch->lintents, link)
			if (li->id == h.msg.param2)
				break;
	}
	if (li == NULL && ch != NULL && ch->adopted && ch->bounce != NULL &&
	    h.msg.param2 < GLINK_ADOPT_LIID) {
		/* an intent Linux offered: through the bounce buffer */
		if (ch->bounce_iid != h.msg.param2) {
			ch->bounce_iid = h.msg.param2;
			ch->bounce_off = 0;
		}
		if (ch->bounce_off + h.chunk_size <= ch->bounce_size)
			e->ops->rx_peek(e->ops_arg, ch->bounce + ch->bounce_off,
			    sizeof(h), h.chunk_size);
		ch->bounce_off += h.chunk_size;
		if (h.left_size == 0) {
			if (ch->bounce_off > ch->bounce_size)
				glink_log("%s: %s: %zu bytes into Linux's intent "
				    "%u: too long, dropped\n", e->label, ch->name,
				    ch->bounce_off, h.msg.param2);
			else if (ch->user && ch->rx != NULL) {
				ch->rx_msgs++;
				ch->bounced++;
				ch->rx(ch->arg, ch->bounce, ch->bounce_off);
			}
			tx_cmd(e, CMD_RX_DONE, ch->lcid, h.msg.param2);
			ch->bounce_off = 0;
			ch->bounce_iid = 0;
		}
		goto out;
	}
	if (li == NULL || li->size - li->offset < h.chunk_size) {
		glink_log("%s: data for no or a too small intent (%u)\n",
		    e->label, h.msg.param2);
		goto out;
	}
	e->ops->rx_peek(e->ops_arg, li->data + li->offset, sizeof(h),
	    h.chunk_size);
	li->offset += h.chunk_size;
	if (h.left_size != 0)
		goto out;
	ch->rx_msgs++;
	if (ch->user && ch->rx != NULL)
		ch->rx(ch->arg, li->data, li->offset);
	li->offset = 0;
	if (li->reuse)
		tx_cmd(e, CMD_RX_DONE_W_REUSE, ch->lcid, li->id);
	else {
		tx_cmd(e, CMD_RX_DONE, ch->lcid, li->id);
		TAILQ_REMOVE(&ch->lintents, li, link);
		glink_free(li->data);
		glink_free(li);
	}
out:
	e->ops->rx_advance(e->ops_arg, align8(sizeof(h) + h.chunk_size));
	return (0);
}

void
glink_edge_rx(struct glink_edge *e)
{
	struct glink_msg m;
	char name[NAME_SIZE];
	size_t avail, need;
	u_int nlen;

	glink_lock(&e->lock);
	/* writers waiting for FIFO space look again */
	glink_broadcast(&e->cond);
	while (!e->bad && !e->aborted) {
		avail = e->ops->rx_avail(e->ops_arg);
		if (avail < sizeof(m))
			break;
		e->ops->rx_peek(e->ops_arg, &m, 0, sizeof(m));
		need = sizeof(m);
		e->rx_msgs++;
		switch (m.cmd) {
		case CMD_VERSION:
			e->remote_version = true;
			if (m.param1 == VERSION_1)
				e->features &= m.param2;
			if (m.param1 != 0)
				send_version(e, CMD_VERSION_ACK);
			break;
		case CMD_VERSION_ACK:
			e->remote_version = true;
			if (m.param1 == VERSION_1 && m.param2 == e->features)
				break;
			if (m.param1 == VERSION_1)
				e->features &= m.param2;
			if (m.param1 != 0)
				send_version(e, CMD_VERSION);
			break;
		case CMD_OPEN:
			nlen = m.param2 & 0xffff;
			need = align8(sizeof(m) + nlen);
			if (avail < need)
				goto wait;
			memset(name, 0, sizeof(name));
			e->ops->rx_peek(e->ops_arg, name, sizeof(m),
			    MIN(nlen, NAME_SIZE - 1));
			rx_open(e, m.param1, name);
			break;
		case CMD_CLOSE:
			rx_close(e, m.param1);
			break;
		case CMD_CLOSE_ACK:
			rx_close_ack(e, m.param1);
			break;
		case CMD_OPEN_ACK: {
			struct glink_channel *ch = ch_by_lcid(e, m.param1);

			if (ch != NULL) {
				ch->open_acked = true;
				glink_broadcast(&e->cond);
			}
			break;
		}
		case CMD_INTENT:
			need = align8(sizeof(m) + (size_t)m.param2 * 8);
			if (avail < need)
				goto wait;
			rx_intents(e, m.param1, m.param2);
			break;
		case CMD_RX_DONE:
		case CMD_RX_DONE_W_REUSE:
			rx_done(e, m.param1, m.param2,
			    m.cmd == CMD_RX_DONE_W_REUSE);
			break;
		case CMD_RX_INTENT_REQ:
			rx_intent_req(e, m.param1, m.param2);
			break;
		case CMD_RX_INTENT_REQ_ACK: {
			struct glink_channel *ch = ch_by_rcid(e, m.param1);

			if (ch != NULL) {
				ch->intent_req = m.param2 != 0;
				glink_broadcast(&e->cond);
			}
			break;
		}
		case CMD_TX_DATA:
		case CMD_TX_DATA_CONT:
			if (rx_data(e, avail) == EAGAIN) {
				e->rx_msgs--;	/* counted when it is all there */
				goto wait;
			}
			continue;	/* rx_data advanced */
		case CMD_READ_NOTIF:
			e->ops->tx_kick(e->ops_arg);
			break;
		case CMD_SIGNALS:
			break;
		default:
			glink_log("%s: unknown command %u, stopping\n",
			    e->label, m.cmd);
			e->bad = true;
			goto wait;
		}
		e->ops->rx_advance(e->ops_arg, need);
	}
wait:
	glink_unlock(&e->lock);
}

/* --- the edge ---------------------------------------------------------------------------- */

static struct glink_edge *
edge_new(const char *label, const struct glink_fifo_ops *ops,
    void *ops_arg, glink_announce_cb_t *announce, void *announce_arg)
{
	struct glink_edge *e;

	e = glink_zalloc(sizeof(*e));
	strncpy(e->label, label, sizeof(e->label) - 1);
	e->ops = ops;
	e->ops_arg = ops_arg;
	e->announce = announce;
	e->announce_arg = announce_arg;
	e->features = FEATURE_INTENT_REUSE;
	e->next_lcid = 1;
	TAILQ_INIT(&e->channels);
	glink_mutex_init(&e->lock);
	glink_cond_init(&e->cond);
	return (e);
}

struct glink_edge *
glink_edge_create(const char *label, const struct glink_fifo_ops *ops,
    void *ops_arg, glink_announce_cb_t *announce, void *announce_arg)
{
	struct glink_edge *e;

	e = edge_new(label, ops, ops_arg, announce, announce_arg);
	glink_lock(&e->lock);
	send_version(e, CMD_VERSION);
	glink_unlock(&e->lock);
	return (e);
}

/* Linux's edge: its negotiation stands, nothing is sent */
struct glink_edge *
glink_edge_adopt(const char *label, const struct glink_fifo_ops *ops,
    void *ops_arg, glink_announce_cb_t *announce, void *announce_arg)
{
	struct glink_edge *e;

	e = edge_new(label, ops, ops_arg, announce, announce_arg);
	e->adopted = true;
	return (e);
}

/*
 * A channel open on both sides with Linux's ids. Its users find it as one
 * the remote opened (the announcement); new channels of ours get ids above
 * Linux's.
 */
int
glink_adopt_channel(struct glink_edge *e, const char *name, u_int lcid,
    u_int rcid, size_t bounce, uint32_t hint)
{
	struct glink_channel *ch;
	char *buf;

	if (strlen(name) >= NAME_SIZE || lcid == 0 || rcid == 0)
		return (EINVAL);
	buf = bounce != 0 ? glink_zalloc(bounce) : NULL;
	glink_lock(&e->lock);
	if (ch_by_name(e, name) != NULL || ch_by_lcid(e, lcid) != NULL ||
	    ch_by_rcid(e, rcid) != NULL) {
		glink_unlock(&e->lock);
		glink_free(buf);
		return (EEXIST);
	}
	ch = ch_new(e, name);
	ch->lcid = lcid;
	ch->rcid = rcid;
	ch->open_acked = true;
	ch->acked_remote = true;
	ch->adopted = true;
	ch->hint = hint;
	ch->bounce = buf;
	ch->bounce_size = bounce;
	ch->next_liid = GLINK_ADOPT_LIID;
	if (e->next_lcid <= lcid)
		e->next_lcid = lcid + 1;
	glink_unlock(&e->lock);
	if (e->announce != NULL)
		e->announce(e->announce_arg, name);
	return (0);
}

void
glink_edge_abort(struct glink_edge *e)
{
	struct glink_channel *ch;

	glink_lock(&e->lock);
	e->aborted = true;
	TAILQ_FOREACH(ch, &e->channels, link) {
		if (ch->up && ch->user && ch->state != NULL)
			ch->state(ch->arg, false);
		ch->up = false;
		/* nothing more comes from the remote */
		ch->rcid = 0;
		ch->acked_remote = false;
		ch_flush_rintents(ch);
	}
	glink_broadcast(&e->cond);
	glink_unlock(&e->lock);
}

static void
edge_free(struct glink_edge *e)
{
	glink_cond_destroy(&e->cond);
	glink_mutex_destroy(&e->lock);
	glink_free(e);
}

/*
 * After glink_edge_abort: the edge goes now, or with the last of its
 * channels that a user still holds (glink_close).
 */
void
glink_edge_destroy(struct glink_edge *e)
{
	struct glink_channel *ch, *next;
	bool empty;

	glink_lock(&e->lock);
	e->destroy = true;
	TAILQ_FOREACH_SAFE(ch, &e->channels, link, next) {
		if (ch->user)
			continue;
		TAILQ_REMOVE(&e->channels, ch, link);
		ch_flush_rintents(ch);
		ch_flush_lintents(ch);
		glink_free(ch->bounce);
		glink_free(ch);
	}
	empty = TAILQ_EMPTY(&e->channels);
	glink_unlock(&e->lock);
	if (empty)
		edge_free(e);
}

const char *
glink_edge_label(struct glink_edge *e)
{
	return (e->label);
}

/* --- users --------------------------------------------------------------------------------- */

int
glink_open(struct glink_edge *e, const char *name, int nintents,
    size_t intent_size, glink_rx_cb_t *rx, glink_state_cb_t *state, void *arg,
    u_int timeout_ms, struct glink_channel **chp)
{
	struct glink_channel *ch;
	uint64_t end;
	int i, error;

	if (strlen(name) >= NAME_SIZE)
		return (EINVAL);
	glink_lock(&e->lock);
	if (e->aborted) {
		error = EIO;
		goto out;
	}
	ch = ch_by_name(e, name);
	if (ch != NULL && ch->adopted && !ch->user && ch->lcid != 0 &&
	    ch->rcid != 0) {
		/* Linux's channel: open as it is, only our intents are new */
		ch->user = true;
		ch->rx = rx;
		ch->state = state;
		ch->arg = arg;
		for (i = 0; i < nintents; i++)
			advertise(e, ch, lintent_new(ch, intent_size, true));
		ch->up = true;
		*chp = ch;
		glink_unlock(&e->lock);
		return (0);
	}
	if (ch != NULL && (ch->user || ch->lcid != 0)) {
		error = EBUSY;
		goto out;
	}
	if (ch == NULL)
		ch = ch_new(e, name);
	ch->user = true;
	ch->rx = rx;
	ch->state = state;
	ch->arg = arg;
	ch->lcid = e->next_lcid++;
	ch->open_acked = false;
	if (ch->rcid != 0 && !ch->acked_remote) {
		/* the remote opened it first */
		ch->acked_remote = true;
		tx_cmd(e, CMD_OPEN_ACK, ch->rcid, 0);
	}
	if ((error = send_open(e, ch)) != 0)
		goto fail;
	end = glink_now_ms() + timeout_ms;
	while (!(ch->open_acked && ch->rcid != 0 && ch->acked_remote)) {
		if (e->aborted) {
			error = EIO;
			goto fail;
		}
		if (glink_now_ms() >= end) {
			error = GLINK_TIMEDOUT;
			goto fail;
		}
		glink_cond_wait(&e->cond, &e->lock, POLL_MS);
	}
	for (i = 0; i < nintents; i++)
		advertise(e, ch, lintent_new(ch, intent_size, true));
	ch->up = true;
	*chp = ch;
	glink_unlock(&e->lock);
	return (0);
fail:
	ch->user = false;
	ch->rx = NULL;
	ch->state = NULL;
	if (ch->lcid != 0 && !e->aborted)
		tx_cmd(e, CMD_CLOSE, ch->lcid, 0);
	else
		ch->lcid = 0;
	ch_maybe_free(e, ch);
out:
	glink_unlock(&e->lock);
	return (error);
}

/* The smallest free intent of the remote that takes len bytes */
static struct rintent *
pick_intent(struct glink_channel *ch, size_t len)
{
	struct rintent *ri, *best = NULL;

	TAILQ_FOREACH(ri, &ch->rintents, link)
		if (!ri->in_use && ri->size >= len &&
		    (best == NULL || ri->size < best->size))
			best = ri;
	return (best);
}

int
glink_send(struct glink_channel *ch, const void *data, size_t len)
{
	struct glink_edge *e = ch->edge;
	struct glink_data_hdr h;
	struct rintent *ri;
	uint64_t end;
	size_t off, chunk;
	int error = 0;

	glink_lock(&e->lock);
	end = glink_now_ms() + INTENT_TIMEOUT_MS;
	while ((ri = pick_intent(ch, len)) == NULL) {
		if (!ch->up || e->aborted) {
			error = ENOTCONN;
			goto out;
		}
		/* ask for one, then wait for the answer and the intent */
		ch->intent_req = -1;
		ch->intent_received = false;
		if ((error = tx_cmd(e, CMD_RX_INTENT_REQ, ch->lcid, len)) != 0)
			goto out;
		while (ch->up && !e->aborted && ch->intent_req != 0 &&
		    !(ch->intent_req == 1 && ch->intent_received) &&
		    pick_intent(ch, len) == NULL) {
			if (glink_now_ms() >= end) {
				error = GLINK_TIMEDOUT;
				goto out;
			}
			glink_cond_wait(&e->cond, &e->lock, POLL_MS);
		}
		if (ch->intent_req == 0 && pick_intent(ch, len) == NULL) {
			error = EAGAIN;
			goto out;
		}
	}
	ri->in_use = true;
	for (off = 0; off < len || (len == 0 && off == 0); off += chunk) {
		chunk = MIN(len - off, CHUNK_MAX);
		h.msg.cmd = off == 0 ? CMD_TX_DATA : CMD_TX_DATA_CONT;
		h.msg.param1 = ch->lcid;
		h.msg.param2 = ri->id;
		h.chunk_size = chunk;
		h.left_size = len - off - chunk;
		error = tx(e, &h, sizeof(h), (const char *)data + off, chunk);
		if (error != 0) {
			ri->in_use = false;
			break;
		}
		if (len == 0)
			break;
	}
	if (error == 0)
		ch->tx_msgs++;
out:
	glink_unlock(&e->lock);
	return (error);
}

void
glink_close(struct glink_channel *ch)
{
	struct glink_edge *e = ch->edge;

	glink_lock(&e->lock);
	ch->user = false;
	ch->rx = NULL;
	ch->state = NULL;
	ch->up = false;
	ch->acked_remote = false;
	if (ch->lcid != 0 && !e->aborted)
		tx_cmd(e, CMD_CLOSE, ch->lcid, 0);
	else
		ch->lcid = 0;
	ch_flush_lintents(ch);
	if (e->destroy) {
		/* the edge is gone: no close acknowledgement comes */
		ch->lcid = 0;
		ch->rcid = 0;
	}
	ch_maybe_free(e, ch);
	if (e->destroy && TAILQ_EMPTY(&e->channels)) {
		glink_unlock(&e->lock);
		edge_free(e);
		return;
	}
	glink_unlock(&e->lock);
}

const char *
glink_channel_name(struct glink_channel *ch)
{
	return (ch->name);
}

bool
glink_channel_adopted(struct glink_channel *ch)
{
	return (ch->adopted);
}

uint32_t
glink_channel_hint(struct glink_channel *ch)
{
	return (ch->hint);
}

void
glink_edge_info(struct glink_edge *e, struct glink_edge_info *ei,
    glink_channel_info_cb_t *cb, void *arg)
{
	struct glink_channel_info ci;
	struct glink_channel *ch;
	struct lintent *li;
	struct rintent *ri;

	glink_lock(&e->lock);
	memset(ei, 0, sizeof(*ei));
	ei->label = e->label;
	ei->adopted = e->adopted;
	ei->remote_version = e->remote_version;
	ei->bad = e->bad;
	ei->aborted = e->aborted;
	ei->features = e->features;
	ei->rx_msgs = e->rx_msgs;
	ei->tx_msgs = e->tx_msgs;
	TAILQ_FOREACH(ch, &e->channels, link) {
		ei->nchannels++;
		if (cb == NULL)
			continue;
		memset(&ci, 0, sizeof(ci));
		ci.name = ch->name;
		ci.lcid = ch->lcid;
		ci.rcid = ch->rcid;
		ci.user = ch->user;
		ci.up = ch->up;
		ci.open_acked = ch->open_acked;
		ci.acked_remote = ch->acked_remote;
		ci.adopted = ch->adopted;
		TAILQ_FOREACH(li, &ch->lintents, link)
			ci.lintents++;
		TAILQ_FOREACH(ri, &ch->rintents, link)
			ci.rintents++;
		ci.rx_msgs = ch->rx_msgs;
		ci.tx_msgs = ch->tx_msgs;
		ci.bounced = ch->bounced;
		cb(arg, &ci);
	}
	glink_unlock(&e->lock);
}

/* A user registered after the remote opened NAME: tell it now */
void
glink_edge_reannounce(struct glink_edge *e, const char *name)
{
	struct glink_channel *ch;
	bool tell;

	glink_lock(&e->lock);
	ch = ch_by_name(e, name);
	tell = ch != NULL && !ch->user && ch->rcid != 0 && !e->aborted;
	glink_unlock(&e->lock);
	if (tell && e->announce != NULL)
		e->announce(e->announce_arg, name);
}
