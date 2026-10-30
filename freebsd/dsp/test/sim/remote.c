/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The simulation's remote processors (ADSP, modem, CDSP): what their
 * firmware does on the apps processor's side of SMEM, written apart from
 * FreeBSD's code (so the two do not share mistakes), after Linux 7.1 as
 * the reference for the other end:
 *
 * - SMP2P: its item (version 2, SSR acknowledgement) with the entry
 *   "slave-kernel": ready (bit 1) and handover (2) after the start, stop
 *   acknowledged (3) when our "master-kernel" bit 0 asks, fatal (0) with a
 *   reason in its SMEM crash item; a restart toggles RESTART_DONE.
 * - GLINK (Linux's qcom_glink_native as the remote): VERSION, OPEN of
 *   IPCRTR (and the ADSP's apr_audio_svc), an OPEN answered at once with
 *   OPEN_ACK and an OPEN of its own, five 1 KiB intents once open, intent
 *   requests granted, data chunked into the peer's intents, RX_DONE with
 *   reuse for its own.
 * - QRTR: a HELLO as soon as it has an intent of ours, its servers after
 *   node 1's HELLO (and again, its old view of node 1 dropped, when a
 *   node it knows says HELLO: a peer that started again, the assumption
 *   the takeover relies on), RESUME_TX when asked.
 * - QMI: the modem's DMS (service 2) answers "get revision" (0x23); the
 *   modem needs rmtfs (14) announced by node 1 within 2 s of its HELLO
 *   (RQ_NEEDS_RMTFS) or it fails with "rmtfs not found", and opens
 *   modem_fs1 there.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sim.h"

#undef printf
#define	printf	sim_printf
#define	MIN(a, b)	((a) < (b) ? (a) : (b))

/* GLINK commands (Linux's qcom_glink_native.c) */
enum { G_VERSION, G_VERSION_ACK, G_OPEN, G_CLOSE, G_OPEN_ACK, G_INTENT,
    G_RX_DONE, G_RX_INTENT_REQ, G_RX_INTENT_REQ_ACK, G_TX_DATA,
    G_CLOSE_ACK = 11, G_TX_DATA_CONT, G_READ_NOTIF, G_RX_DONE_W_REUSE,
    G_SIGNALS };
/* QRTR */
enum { Q_DATA = 1, Q_HELLO, Q_BYE, Q_NEW_SERVER, Q_DEL_SERVER, Q_DEL_CLIENT,
    Q_RESUME_TX, Q_EXIT, Q_PING, Q_NEW_LOOKUP, Q_DEL_LOOKUP };
#define	QCTRL		0xfffffffeu
#define	SMP2P_MAGIC	0x504d5324

struct smp2p_item {
	uint32_t	magic, version_features;
	uint16_t	local_pid, remote_pid, total_entries, valid_entries;
	uint32_t	flags;
	struct {
		char		name[16];
		uint32_t	value;
	} entries[16];
} __attribute__((__packed__));

struct pintent {		/* an intent of the peer's */
	uint32_t	id, size;
	bool		used, valid;
};

struct rchan {
	char		name[32];
	uint16_t	lcid, rcid;	/* ours, the peer's */
	bool		used, open_sent, open_acked, peer_open, intents_sent;
	struct pintent	pi[24];
	struct {
		uint32_t	id, size, off;
		char		buf[8192];
		bool		valid;
	} li[16];
	int		nli;
	uint32_t	next_liid;
	char		pend[4][2048];	/* waiting for an intent */
	size_t		pendlen[4];
	int		npend;
	bool		intent_asked;
	uint32_t	linux_iids_below;	/* adopted: ids of Linux's */
};

struct remote {
	const char	*name;
	int		pas_id, host, pid;
	unsigned	item_out, item_in;	/* its SMP2P item, ours */
	int		irq_smp2p, irq_glink;
	int		bell_smp2p, bell_glink;
	uint32_t	node;			/* QRTR */
	unsigned	crash_item;
	int		quirks;
	/* the run */
	pthread_t	thread;
	pthread_mutex_t	lock;
	pthread_cond_t	cond;
	int		kicks;
	bool		running, stop_req;
	int		boots;
	uint64_t	t_boot, t_hello;
	struct smp2p_item *out;
	uint32_t	*desc;
	char		*rxf, *txf;	/* FIFO 479 (ours, it reads), 480 */
	size_t		rxlen, txlen;
	struct rchan	ch[6];
	bool		hello_sent, peer_known, servers_sent;
	bool		hello_wait;	/* ours sent, its answer not yet in */
	uint32_t	peer_servers[16][2];	/* service, port of node 1 */
	int		npeer;
	bool		rmtfs_asked;
	struct remote_stats st;
};

static struct remote remotes[] = {
	/* the device tree: smp2p-adsp (qcom,smem = <443 429>, SPI 279,
	 * mboxes <&apcs_glb 10>), remoteproc@a400000's glink-edge (SPI 277,
	 * <&apcs_glb 8>), Linux's crash_reason_smem 423; QRTR node 5 */
	{ "adsp", 1, 2, 2, 443, 429, SIM_SPI(279), SIM_SPI(277), 10, 8, 5, 423 },
	/* smp2p-mpss <435 428>, SPI 70, bit 14; glink-edge SPI 68, bit 12;
	 * 421; node 0 */
	{ "modem", 4, 1, 1, 435, 428, SIM_SPI(70), SIM_SPI(68), 14, 12, 0, 421 },
	/* smp2p-cdsp <94 432>, SPI 263, bit 30; glink-edge SPI 261, bit 28;
	 * 601; node 10 */
	{ "cdsp", 18, 5, 5, 94, 432, SIM_SPI(263), SIM_SPI(261), 30, 28, 10, 601 },
};

static void __attribute__((constructor))
remotes_init(void)
{
	size_t i;

	for (i = 0; i < sizeof(remotes) / sizeof(remotes[0]); i++) {
		pthread_mutex_init(&remotes[i].lock, NULL);
		pthread_cond_init(&remotes[i].cond, NULL);
	}
}

static uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

struct remote *
sim_remote(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(remotes) / sizeof(remotes[0]); i++)
		if (strcmp(remotes[i].name, name) == 0)
			return (&remotes[i]);
	return (NULL);
}

void
sim_remote_set_quirks(struct remote *r, int quirks)
{
	r->quirks = quirks;
}

uint32_t
sim_remote_qrtr_node(struct remote *r)
{
	return (r->node);
}

void
sim_remote_stats(struct remote *r, struct remote_stats *st)
{
	pthread_mutex_lock(&r->lock);
	*st = r->st;
	pthread_mutex_unlock(&r->lock);
}

static void
kick_cb(void *arg, int bit)
{
	struct remote *r = arg;

	pthread_mutex_lock(&r->lock);
	r->kicks++;
	pthread_cond_signal(&r->cond);
	pthread_mutex_unlock(&r->lock);
}

/* --- SMP2P --------------------------------------------------------------------------- */

static void
smp2p_set(struct remote *r, uint32_t mask, uint32_t val, bool irq)
{
	uint32_t v = r->out->entries[0].value;

	r->out->entries[0].value = (v & ~mask) | (val & mask);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (irq)
		sim_irq_raise(r->irq_smp2p);
}

/* Our "master-kernel" entry in our item, if it is there */
static uint32_t
smp2p_ours(struct remote *r)
{
	struct smp2p_item *in;
	int i;

	in = sim_smem_get(r->host, r->item_in, NULL);
	if (in == NULL || in->magic != SMP2P_MAGIC)
		return (0);
	for (i = 0; i < in->valid_entries && i < 16; i++)
		if (strncmp(in->entries[i].name, "master-kernel", 16) == 0)
			return (in->entries[i].value);
	return (0);
}

static void
crash(struct remote *r, const char *why)
{
	char *msg;

	msg = sim_smem_alloc(-1, r->crash_item, 64);
	snprintf(msg, 64, "%s", why);
	r->st.crashed = true;
	smp2p_set(r, 1, 1, true);
	printf("sim %s: fatal error: %s\n", r->name, why);
}

/* --- GLINK: its FIFO out, ours in ------------------------------------------------- */

static size_t
tx_space(struct remote *r)
{
	uint32_t head = r->desc[3], tail = r->desc[2];
	size_t a = tail <= head ? r->txlen - head + tail : tail - head;

	return (a > 16 ? a - 16 : 0);
}

static void
g_write(struct remote *r, const void *hdr, size_t hl, const void *data,
    size_t dl)
{
	uint32_t head;
	size_t i, n = hl + dl, need = (n + 7) & ~7ul;
	const char *h = hdr, *d = data;
	int spins = 0;

	while (tx_space(r) < need) {
		if (++spins > 2000) {
			r->st.errors++;
			printf("sim %s: our FIFO stays full\n", r->name);
			return;
		}
		sim_irq_raise(r->irq_glink);
		usleep(1000);
	}
	head = r->desc[3];
	for (i = 0; i < n; i++)
		r->txf[(head + i) % r->txlen] = i < hl ? h[i] : d[i - hl];
	head = (head + need) % r->txlen;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	r->desc[3] = head;
	sim_irq_raise(r->irq_glink);
}

static void
g_cmd(struct remote *r, uint16_t cmd, uint16_t p1, uint32_t p2)
{
	uint16_t m[4] = { cmd, p1, (uint16_t)p2, (uint16_t)(p2 >> 16) };

	g_write(r, m, 8, NULL, 0);
}

static struct rchan *
ch_name(struct remote *r, const char *name, bool create)
{
	int i;

	for (i = 0; i < 6; i++)
		if (r->ch[i].used && strcmp(r->ch[i].name, name) == 0)
			return (&r->ch[i]);
	if (!create)
		return (NULL);
	for (i = 0; i < 6; i++)
		if (!r->ch[i].used) {
			memset(&r->ch[i], 0, sizeof(r->ch[i]));
			r->ch[i].used = true;
			snprintf(r->ch[i].name, 32, "%s", name);
			r->ch[i].lcid = 7 + i * 2;	/* the remote's numbering */
			r->ch[i].next_liid = 1;
			return (&r->ch[i]);
		}
	return (NULL);
}

static struct rchan *
ch_lcid(struct remote *r, uint16_t lcid)
{
	int i;

	for (i = 0; i < 6; i++)
		if (r->ch[i].used && r->ch[i].lcid == lcid)
			return (&r->ch[i]);
	return (NULL);
}

static struct rchan *
ch_rcid(struct remote *r, uint16_t rcid)
{
	int i;

	for (i = 0; i < 6; i++)
		if (r->ch[i].used && r->ch[i].rcid == rcid && rcid != 0)
			return (&r->ch[i]);
	return (NULL);
}

static void
g_open(struct remote *r, struct rchan *c)
{
	char m[8 + 32];
	uint32_t nl = strlen(c->name) + 1;

	memset(m, 0, sizeof(m));
	((uint16_t *)m)[0] = G_OPEN;
	((uint16_t *)m)[1] = c->lcid;
	((uint32_t *)m)[1] = nl;
	memcpy(m + 8, c->name, nl);
	g_write(r, m, (8 + nl + 7) & ~7u, NULL, 0);
	c->open_sent = true;
}

static bool
advertise(struct remote *r, struct rchan *c, uint32_t size)
{
	uint32_t m[4];

	if (c->nli == 16 || size > sizeof(c->li[0].buf))
		return (false);
	c->li[c->nli].id = c->next_liid++;
	c->li[c->nli].size = size;
	c->li[c->nli].off = 0;
	c->li[c->nli].valid = true;
	((uint16_t *)m)[0] = G_INTENT;
	((uint16_t *)m)[1] = c->lcid;
	m[1] = 1;
	m[2] = size;
	m[3] = c->li[c->nli].id;
	c->nli++;
	g_write(r, m, 16, NULL, 0);
	return (true);
}

static bool
ch_up(struct rchan *c)
{
	return (c->peer_open && c->open_acked);
}

static void
maybe_up(struct remote *r, struct rchan *c)
{
	int i;

	if (!ch_up(c) || c->intents_sent)
		return;
	/* Linux's qcom_glink_announce_create: five intents of 1 KiB */
	c->intents_sent = true;
	for (i = 0; i < 5; i++)
		advertise(r, c, 1024);
	if (strcmp(c->name, "IPCRTR") == 0)
		r->st.ipcrtr_open = true;
}

/* One message on a channel into an intent of the peer's, or wait for one */
static bool
send_now(struct remote *r, struct rchan *c, const void *d, size_t len)
{
	struct pintent *best = NULL;
	uint32_t m[4];
	int i;

	for (i = 0; i < 24; i++)
		if (c->pi[i].valid && !c->pi[i].used && c->pi[i].size >= len &&
		    (best == NULL || c->pi[i].size < best->size))
			best = &c->pi[i];
	if (best == NULL)
		return (false);
	best->used = true;
	if (best->id < c->linux_iids_below)
		r->st.bounced_tx++;
	((uint16_t *)m)[0] = G_TX_DATA;
	((uint16_t *)m)[1] = c->lcid;
	m[1] = best->id;
	m[2] = len;
	m[3] = 0;
	g_write(r, m, 16, d, len);
	return (true);
}

static void
ch_send(struct remote *r, struct rchan *c, const void *d, size_t len)
{
	if (c->npend == 0 && send_now(r, c, d, len))
		return;
	if (c->npend == 4 || len > sizeof(c->pend[0])) {
		r->st.errors++;
		return;
	}
	memcpy(c->pend[c->npend], d, len);
	c->pendlen[c->npend++] = len;
	if (!c->intent_asked) {
		c->intent_asked = true;
		g_cmd(r, G_RX_INTENT_REQ, c->lcid, len);
	}
}

static void
flush_pending(struct remote *r, struct rchan *c)
{
	while (c->npend > 0 && send_now(r, c, c->pend[0], c->pendlen[0])) {
		memmove(c->pend[0], c->pend[1], sizeof(c->pend[0]) * 3);
		memmove(&c->pendlen[0], &c->pendlen[1], sizeof(size_t) * 3);
		c->npend--;
	}
	if (c->npend > 0 && !c->intent_asked) {
		c->intent_asked = true;
		g_cmd(r, G_RX_INTENT_REQ, c->lcid, c->pendlen[0]);
	}
}

/* --- QRTR and QMI ------------------------------------------------------------------- */

static void
q_send(struct remote *r, int type, uint32_t sport, uint32_t dnode,
    uint32_t dport, const void *d, size_t len)
{
	struct rchan *c = ch_name(r, "IPCRTR", false);
	uint8_t pkt[2048];
	uint32_t *h = (uint32_t *)pkt;

	if (c == NULL || !ch_up(c) || len + 32 > sizeof(pkt))
		return;
	memset(pkt, 0, sizeof(pkt));
	h[0] = 1;
	h[1] = type;
	h[2] = r->node;
	h[3] = sport;
	h[4] = 0;
	h[5] = len;
	h[6] = dnode;
	h[7] = dport;
	memcpy(pkt + 32, d, len);
	ch_send(r, c, pkt, 32 + ((len + 3) & ~3ul));
}

static void
q_ctrl(struct remote *r, uint32_t cmd, uint32_t a, uint32_t b, uint32_t c,
    uint32_t d)
{
	uint32_t p[5] = { cmd, a, b, c, d };

	q_send(r, cmd, QCTRL, 1, QCTRL, p, sizeof(p));
}

/* A HELLO of its own: the next one from node 1 is the answer */
static void
say_hello(struct remote *r)
{
	r->hello_sent = true;
	r->hello_wait = true;
	q_ctrl(r, Q_HELLO, 0, 0, 0, 0);
}

/* Its servers: service, instance, port */
static const uint32_t adsp_servers[][3] = { { 43, 0x14, 2 }, { 66, 0x1, 3 } };
static const uint32_t modem_servers[][3] = { { 2, 1, 51 }, { 3, 1, 39 },
    { 5, 1, 55 }, { 9, 1, 38 }, { 11, 1, 48 }, { 69, 0, 30 } };

static void
announce(struct remote *r)
{
	size_t i;

	if (strcmp(r->name, "modem") == 0) {
		for (i = 0; i < sizeof(modem_servers) / sizeof(modem_servers[0]); i++)
			q_ctrl(r, Q_NEW_SERVER, modem_servers[i][0],
			    modem_servers[i][1], r->node, modem_servers[i][2]);
	} else if (strcmp(r->name, "adsp") == 0) {
		for (i = 0; i < sizeof(adsp_servers) / sizeof(adsp_servers[0]); i++)
			q_ctrl(r, Q_NEW_SERVER, adsp_servers[i][0],
			    adsp_servers[i][1], r->node, adsp_servers[i][2]);
	}
	r->servers_sent = true;
}

static void
qmi_tlv(uint8_t **p, uint8_t type, const void *v, uint16_t len)
{
	*(*p)++ = type;
	memcpy(*p, &len, 2);
	*p += 2;
	memcpy(*p, v, len);
	*p += len;
}

/* rmtfs's OPEN (QMI_RMTFS_OPEN 0x0001, TLV 1: the path) to node 1 */
static void
rmtfs_open(struct remote *r, uint32_t port)
{
	uint8_t m[128], *p = m + 7;
	const char *path = "/boot/modem_fs1";
	uint16_t len;

	m[0] = 0;
	m[1] = 1; m[2] = 0;		/* transaction 1 */
	m[3] = 0x01; m[4] = 0;		/* message 1 */
	qmi_tlv(&p, 0x01, path, strlen(path));
	len = p - m - 7;
	memcpy(m + 5, &len, 2);
	q_send(r, Q_DATA, 0x77, 1, port, m, p - m);
	r->rmtfs_asked = true;
}

/* A QMI request to one of its services: DMS's "get revision" answered */
static void
qmi_rx(struct remote *r, uint32_t snode, uint32_t sport, uint32_t dport,
    const uint8_t *d, size_t len)
{
	uint8_t m[128], *p = m + 7;
	const char *rev = "MPSS.SIM.1.0 (freebsd/dsp/test/sim)";
	uint16_t msg, l, res[2] = { 0, 0 };

	if (len < 7)
		return;
	memcpy(&msg, d + 3, 2);
	if (d[0] == 2 && dport == 0x77 && snode == 1) {
		/* rmtfs's answer to our OPEN */
		r->st.rmtfs_opened++;
		return;
	}
	if (d[0] != 0)
		return;
	r->st.qmi_req++;
	if (!(strcmp(r->name, "modem") == 0 && dport == 51 && msg == 0x23))
		return;
	m[0] = 2;
	m[1] = d[1]; m[2] = d[2];
	m[3] = 0x23; m[4] = 0;
	qmi_tlv(&p, 0x02, res, 4);
	qmi_tlv(&p, 0x01, rev, strlen(rev));
	l = p - m - 7;
	memcpy(m + 5, &l, 2);
	q_send(r, Q_DATA, dport, snode, sport, m, p - m);
	r->st.qmi_resp++;
}

static void
q_rx(struct remote *r, const uint8_t *pkt, size_t len)
{
	const uint32_t *h = (const uint32_t *)pkt, *c;
	uint32_t type, size, i;

	if (len < 32 || h[0] != 1) {
		r->st.errors++;
		return;
	}
	type = h[1];
	size = h[5];
	if (32 + size > len) {
		r->st.errors++;
		return;
	}
	c = (const uint32_t *)(pkt + 32);
	if (h[4] != 0 && type == Q_DATA) {
		/* the sender wants to hear it was read: RESUME_TX */
		uint32_t p[5] = { Q_RESUME_TX, r->node, h[7], 0, 0 };

		q_send(r, Q_RESUME_TX, h[7], h[2], h[3], p, sizeof(p));
	}
	switch (type) {
	case Q_HELLO:
		r->st.hello_rx++;
		if (h[2] != 1)
			break;
		if (r->hello_wait) {
			/* the answer to ours */
			r->hello_wait = false;
		} else if (r->peer_known) {
			/* node 1 started again: what it had is gone */
			r->npeer = 0;
			r->rmtfs_asked = false;
			printf("sim %s: node 1 said HELLO again: started again\n",
			    r->name);
			say_hello(r);
		} else
			say_hello(r);
		r->peer_known = true;
		r->t_hello = now_ms();
		announce(r);
		break;
	case Q_NEW_SERVER:
		if (size < 20 || c[3] != 1)
			break;
		r->st.servers_rx++;
		if (r->st.nservers_seen < 16)
			r->st.servers_seen[r->st.nservers_seen++] = c[1];
		for (i = 0; i < (uint32_t)r->npeer; i++)
			if (r->peer_servers[i][0] == c[1])
				break;
		if (i == (uint32_t)r->npeer && r->npeer < 16)
			r->npeer++;
		r->peer_servers[i][0] = c[1];
		r->peer_servers[i][1] = c[4];
		if (c[1] == 14 && strcmp(r->name, "modem") == 0)
			rmtfs_open(r, c[4]);
		break;
	case Q_DEL_SERVER:
	case Q_BYE:
	case Q_DEL_CLIENT:
	case Q_RESUME_TX:
		break;
	case Q_DATA:
		qmi_rx(r, h[2], h[3], h[7], pkt + 32, size);
		break;
	default:
		break;
	}
}

/* --- GLINK: what the peer sent ------------------------------------------------------ */

static size_t
rx_avail(struct remote *r)
{
	uint32_t head = r->desc[1], tail = r->desc[0];

	if (r->rxf == NULL) {
		r->rxf = sim_smem_get(r->host, 479, &r->rxlen);
		if (r->rxf == NULL)
			return (0);
	}
	return (head >= tail ? head - tail : r->rxlen - tail + head);
}

static void
rx_peek(struct remote *r, void *buf, size_t off, size_t len)
{
	size_t i, t = r->desc[0];

	for (i = 0; i < len; i++)
		((char *)buf)[i] = r->rxf[(t + off + i) % r->rxlen];
}

static void
rx_advance(struct remote *r, size_t len)
{
	r->desc[0] = (r->desc[0] + ((len + 7) & ~7ul)) % r->rxlen;
}

static void
glink_rx(struct remote *r)
{
	uint16_t m[4];
	uint32_t p2, w[4];
	struct rchan *c;
	size_t avail;
	char name[40];
	int i, k;

	while ((avail = rx_avail(r)) >= 8) {
		rx_peek(r, m, 0, 8);
		p2 = m[2] | (uint32_t)m[3] << 16;
		switch (m[0]) {
		case G_VERSION:
			r->st.glink_version = true;
			if (m[1] != 0)
				g_cmd(r, G_VERSION_ACK, 1, p2 & 1);
			rx_advance(r, 8);
			break;
		case G_VERSION_ACK:
			r->st.glink_version = true;
			rx_advance(r, 8);
			break;
		case G_OPEN:
			if (avail < 8 + ((p2 & 0xffff) + 7) / 8 * 8)
				return;
			memset(name, 0, sizeof(name));
			rx_peek(r, name, 8, MIN(p2 & 0xffff, 32));
			c = ch_name(r, name, true);
			c->rcid = m[1];
			c->peer_open = true;
			/* Linux: OPEN_ACK at once, then an OPEN of ours */
			g_cmd(r, G_OPEN_ACK, m[1], 0);
			if (!c->open_sent)
				g_open(r, c);
			maybe_up(r, c);
			rx_advance(r, 8 + (p2 & 0xffff));
			break;
		case G_OPEN_ACK:
			if ((c = ch_lcid(r, m[1])) != NULL) {
				c->open_acked = true;
				maybe_up(r, c);
			}
			rx_advance(r, 8);
			break;
		case G_CLOSE:
			g_cmd(r, G_CLOSE_ACK, m[1], 0);
			if ((c = ch_rcid(r, m[1])) != NULL) {
				c->peer_open = false;
				c->rcid = 0;
				memset(c->pi, 0, sizeof(c->pi));
			}
			rx_advance(r, 8);
			break;
		case G_CLOSE_ACK:
			rx_advance(r, 8);
			break;
		case G_INTENT:
			if (avail < 8 + p2 * 8)
				return;
			c = ch_rcid(r, m[1]);
			for (i = 0; c != NULL && i < (int)p2; i++) {
				rx_peek(r, w, 8 + i * 8, 8);
				for (k = 0; k < 24 && c->pi[k].valid; k++)
					;
				if (k == 24)
					break;
				c->pi[k].valid = true;
				c->pi[k].used = false;
				c->pi[k].size = w[0];
				c->pi[k].id = w[1];
			}
			if (c != NULL) {
				c->intent_asked = false;
				flush_pending(r, c);
				if (strcmp(c->name, "IPCRTR") == 0 &&
				    !r->hello_sent && ch_up(c) &&
				    (r->quirks & RQ_HELLO_AT_ONCE)) {
					/* the moment it can: HELLO */
					say_hello(r);
				}
			}
			rx_advance(r, 8 + p2 * 8);
			break;
		case G_RX_DONE:
		case G_RX_DONE_W_REUSE:
			if ((c = ch_rcid(r, m[1])) != NULL)
				for (k = 0; k < 24; k++)
					if (c->pi[k].valid && c->pi[k].id == p2) {
						c->pi[k].used = false;
						if (m[0] == G_RX_DONE)
							c->pi[k].valid = false;
					}
			if (c != NULL)
				flush_pending(r, c);
			rx_advance(r, 8);
			break;
		case G_RX_INTENT_REQ:
			if ((c = ch_rcid(r, m[1])) != NULL) {
				/* Linux's qcom_glink_handle_intent_req: granted
				 * only with the intent */
				g_cmd(r, G_RX_INTENT_REQ_ACK, c->lcid,
				    advertise(r, c, p2) ? 1 : 0);
			}
			rx_advance(r, 8);
			break;
		case G_RX_INTENT_REQ_ACK:
			rx_advance(r, 8);
			break;
		case G_TX_DATA:
		case G_TX_DATA_CONT: {
			uint32_t hd[4], chunk, left;

			if (avail < 16)
				return;
			rx_peek(r, hd, 0, 16);
			chunk = hd[2];
			left = hd[3];
			if (avail < 16 + chunk)
				return;
			c = ch_rcid(r, m[1]);
			for (k = 0; c != NULL && k < c->nli; k++)
				if (c->li[k].valid && c->li[k].id == p2)
					break;
			if (c == NULL || k == c->nli ||
			    c->li[k].off + chunk > c->li[k].size) {
				r->st.errors++;
				printf("sim %s: data for no intent of mine (%u)\n",
				    r->name, p2);
				rx_advance(r, 16 + chunk);
				break;
			}
			rx_peek(r, c->li[k].buf + c->li[k].off, 16, chunk);
			c->li[k].off += chunk;
			rx_advance(r, 16 + chunk);
			if (left == 0) {
				size_t n = c->li[k].off;

				c->li[k].off = 0;
				g_cmd(r, G_RX_DONE_W_REUSE, c->lcid, p2);
				if (strcmp(c->name, "IPCRTR") == 0)
					q_rx(r, (uint8_t *)c->li[k].buf, n);
			}
			break;
		}
		case G_READ_NOTIF:
			sim_irq_raise(r->irq_glink);
			rx_advance(r, 8);
			break;
		case G_SIGNALS:
			rx_advance(r, 8);
			break;
		default:
			r->st.errors++;
			printf("sim %s: unknown GLINK command %u: stopped\n",
			    r->name, m[0]);
			r->desc[0] = r->desc[1];
			return;
		}
	}
}

/* --- the processor's life ------------------------------------------------------------ */

static void *
run(void *arg)
{
	struct remote *r = arg;
	struct rchan *c;
	bool stopped = false;
	uint64_t t;

	pthread_mutex_lock(&r->lock);
	while (!r->stop_req) {
		pthread_mutex_unlock(&r->lock);
		glink_rx(r);
		if (!stopped && (smp2p_ours(r) & 1)) {
			/* stop requested */
			stopped = true;
			r->st.stopped = true;
			smp2p_set(r, 1U << 3, 1U << 3, true);
		}
		/* the HELLO, once IPCRTR is open and an intent of ours is there */
		c = ch_name(r, "IPCRTR", false);
		if (c != NULL && ch_up(c) && !r->hello_sent) {
			int k;

			for (k = 0; k < 24 && !c->pi[k].valid; k++)
				;
			if (k < 24)
				say_hello(r);
		}
		t = now_ms();
		if ((r->quirks & RQ_NEEDS_RMTFS) && !r->st.crashed &&
		    r->t_hello != 0 && t > r->t_hello + 2000 && r->npeer >= 0) {
			int k;

			for (k = 0; k < r->npeer && r->peer_servers[k][0] != 14; k++)
				;
			if (k == r->npeer)
				crash(r, "efs: rmtfs not found (simulated)");
		}
		if ((r->quirks & RQ_CRASH_AFTER) && !r->st.crashed &&
		    t > r->t_boot + 400)
			crash(r, "err_fatal: simulated crash after 400 ms");
		pthread_mutex_lock(&r->lock);
		if (r->kicks == 0) {
			struct timespec ts;

			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += 5 * 1000000;
			if (ts.tv_nsec >= 1000000000) {
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000;
			}
			pthread_cond_timedwait(&r->cond, &r->lock, &ts);
		}
		r->kicks = 0;
	}
	r->running = false;
	pthread_mutex_unlock(&r->lock);
	return (NULL);
}

static void
glink_mem(struct remote *r)
{
	size_t sz;

	r->desc = sim_smem_alloc(r->host, 478, 32);
	r->txf = sim_smem_alloc(r->host, 480, 16384);
	sim_smem_get(r->host, 480, &sz);
	r->txlen = sz;
	r->rxf = NULL;
	r->rxlen = 0;
}

/* After TrustZone's AUTH_AND_RESET: the firmware boots */
static void *
boot(void *arg)
{
	struct remote *r = arg;
	struct smp2p_item *it;
	bool early = (r->quirks & (RQ_READY_BEFORE_IRQ | RQ_NO_SMP2P_IRQ)) != 0;

	usleep(20000);
	r->boots++;
	r->t_boot = now_ms();
	/* SMP2P: its item, valid entries, version last */
	it = sim_smem_alloc(r->host, r->item_out, sizeof(*it));
	if (it->magic == 0) {
		memset(it, 0, sizeof(*it));
		it->magic = SMP2P_MAGIC;
		it->local_pid = r->pid;
		it->remote_pid = 0;
		it->total_entries = 16;
		snprintf(it->entries[0].name, 16, "slave-kernel");
		it->valid_entries = 1;
	}
	it->entries[0].value = early ? (1U << 1 | 1U << 2) : 0;
	/* a start after the first: RESTART_DONE toggles */
	it->flags = (it->flags & ~1u) | ((r->boots - 1) & 1);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	it->version_features = 2 | 1 << 8;
	r->out = it;
	r->st.booted = true;
	/* GLINK: the descriptor, its FIFO; the transport from the start
	 * (all four indices: a restart does not read the old session's) */
	glink_mem(r);
	memset(r->desc, 0, 16);
	memset(r->ch, 0, sizeof(r->ch));
	r->hello_sent = r->peer_known = r->servers_sent = false;
	r->hello_wait = false;
	r->npeer = 0;
	r->t_hello = 0;
	if (!(r->quirks & RQ_NO_SMP2P_IRQ))
		sim_irq_raise(r->irq_smp2p);
	usleep(10000);
	if (!early)
		smp2p_set(r, 1U << 1 | 1U << 2, 1U << 1 | 1U << 2, true);
	else if (!(r->quirks & RQ_NO_SMP2P_IRQ) && (r->quirks & RQ_READY_BEFORE_IRQ))
		sim_irq_raise(r->irq_smp2p);
	r->st.ready = true;
	printf("sim %s: booted (start %d), ready\n", r->name, r->boots);
	g_cmd(r, G_VERSION, 1, 1);
	g_open(r, ch_name(r, "IPCRTR", true));
	if (strcmp(r->name, "adsp") == 0)
		g_open(r, ch_name(r, "apr_audio_svc", true));
	return (run(r));
}

static void
start_thread(struct remote *r, void *(*fn)(void *))
{
	pthread_mutex_lock(&r->lock);
	r->stop_req = false;
	r->running = true;
	pthread_mutex_unlock(&r->lock);
	pthread_create(&r->thread, NULL, fn, r);
}

void
sim_remote_boot(int pas_id)
{
	size_t i;

	for (i = 0; i < sizeof(remotes) / sizeof(remotes[0]); i++) {
		struct remote *r = &remotes[i];

		if (r->pas_id != pas_id)
			continue;
		sim_doorbell_listen(r->bell_smp2p, kick_cb, r);
		sim_doorbell_listen(r->bell_glink, kick_cb, r);
		memset(&r->st, 0, sizeof(r->st));
		start_thread(r, boot);
	}
}

/* TrustZone's SHUTDOWN: the processor is held in reset */
void
sim_remote_shutdown(int pas_id)
{
	size_t i;

	for (i = 0; i < sizeof(remotes) / sizeof(remotes[0]); i++) {
		struct remote *r = &remotes[i];

		if (r->pas_id != pas_id || !r->running)
			continue;
		pthread_mutex_lock(&r->lock);
		r->stop_req = true;
		pthread_cond_signal(&r->cond);
		pthread_mutex_unlock(&r->lock);
		pthread_join(r->thread, NULL);
		printf("sim %s: in reset\n", r->name);
	}
}

/* --- the kexec case: as Linux left it ------------------------------------------------- */

static void
hist_write(char *fifo, size_t len, uint32_t *head, const void *hdr, size_t hl,
    const void *d, size_t dl)
{
	size_t i, n = hl + dl;

	for (i = 0; i < n; i++)
		fifo[(*head + i) % len] = i < hl ? ((const char *)hdr)[i] :
		    ((const char *)d)[i - hl];
	*head = (*head + ((n + 7) & ~7ul)) % len;
}

static void
hist_open(char *f, size_t len, uint32_t *head, uint16_t id, const char *name)
{
	char m[40];
	uint32_t nl = strlen(name) + 1;

	memset(m, 0, sizeof(m));
	((uint16_t *)m)[0] = G_OPEN;
	((uint16_t *)m)[1] = id;
	((uint32_t *)m)[1] = nl;
	memcpy(m + 8, name, nl);
	hist_write(f, len, head, m, (8 + nl + 7) & ~7u, NULL, 0);
}

static void
hist_cmd(char *f, size_t len, uint32_t *head, uint16_t cmd, uint16_t p1,
    uint32_t p2)
{
	uint16_t m[4] = { cmd, p1, (uint16_t)p2, (uint16_t)(p2 >> 16) };

	hist_write(f, len, head, m, 8, NULL, 0);
}

static void
hist_qrtr(char *f, size_t len, uint32_t *head, uint16_t lcid, uint32_t iid,
    uint32_t type, uint32_t snode, uint32_t sport, uint32_t dnode,
    uint32_t dport, const uint32_t *ctl, int nctl)
{
	uint32_t pkt[8 + 5], dh[4];
	size_t pl = 32 + nctl * 4;

	memset(pkt, 0, sizeof(pkt));
	pkt[0] = 1;
	pkt[1] = type;
	pkt[2] = snode;
	pkt[3] = sport;
	pkt[5] = nctl * 4;
	pkt[6] = dnode;
	pkt[7] = dport;
	memcpy(&pkt[8], ctl, nctl * 4);
	((uint16_t *)dh)[0] = G_TX_DATA;
	((uint16_t *)dh)[1] = lcid;
	dh[1] = iid;
	dh[2] = pl;
	dh[3] = 0;
	hist_write(f, len, head, dh, 16, pkt, pl);
}

/*
 * SMEM, SMP2P and GLINK as Linux leaves them at the kexec with the modem
 * running: both SMP2P items (ours with "master-kernel" 0 and the "ipa"
 * bits Linux set), the GLINK FIFOs' past (VERSION, the opens of IPCRTR
 * with Linux's id 3 and the modem's 7, intents, QRTR traffic; a channel
 * DATA5_CNTL with other data), one packet from the modem Linux never read,
 * and the modem's own state to match. rmtfs's region belongs to HLOS and
 * the modem's VMIDs (Linux's rmtfs_mem).
 */
void
sim_remote_linux_left_running(struct remote *r)
{
	struct smp2p_item *it, *ours;
	struct rchan *c, *c2;
	uint32_t th = 0, rh = 0, k;
	const uint32_t hello[5] = { Q_HELLO, 0, 0, 0, 0 };
	const uint32_t srv14[5] = { Q_NEW_SERVER, 14, 1, 1, 0x4001 };
	const uint32_t srv2[5] = { Q_NEW_SERVER, 2, 1, 0, 51 };
	const uint32_t data[5] = { 0x00010000, 0x0007, 0, 0, 0 };
	uint32_t noise[8] = { 0xdeadbeef, 3, 0, 0, 0, 0, 0, 0 };

	r->boots = 1;
	r->t_boot = now_ms();
	/* its SMP2P item: ready, handover */
	it = sim_smem_alloc(r->host, r->item_out, sizeof(*it));
	memset(it, 0, sizeof(*it));
	it->magic = SMP2P_MAGIC;
	it->version_features = 2 | 1 << 8;
	it->local_pid = r->pid;
	it->total_entries = 16;
	snprintf(it->entries[0].name, 16, "slave-kernel");
	it->entries[0].value = 1U << 1 | 1U << 2;
	snprintf(it->entries[1].name, 16, "ipa");
	it->valid_entries = 2;
	r->out = it;
	/* Linux's item */
	ours = sim_smem_alloc(r->host, r->item_in, sizeof(*ours));
	memset(ours, 0, sizeof(*ours));
	ours->magic = SMP2P_MAGIC;
	ours->version_features = 2 | 1 << 8;
	ours->remote_pid = r->pid;
	ours->total_entries = 16;
	snprintf(ours->entries[0].name, 16, "master-kernel");
	snprintf(ours->entries[1].name, 16, "ipa");
	ours->entries[1].value = 0x3;
	ours->valid_entries = 2;
	/* GLINK */
	glink_mem(r);
	r->rxf = sim_smem_alloc(r->host, 479, 16384);
	r->rxlen = 16384;
	/* Linux's FIFO (479), all read by the modem */
	hist_cmd(r->rxf, r->rxlen, &th, G_VERSION, 1, 1);
	hist_open(r->rxf, r->rxlen, &th, 3, "IPCRTR");
	hist_cmd(r->rxf, r->rxlen, &th, G_OPEN_ACK, 7, 0);
	for (k = 1; k <= 5; k++) {
		uint32_t m[4] = { G_INTENT | 3 << 16, 1, 1024, k };

		hist_write(r->rxf, r->rxlen, &th, m, 16, NULL, 0);
	}
	hist_qrtr(r->rxf, r->rxlen, &th, 3, 1, Q_HELLO, 1, QCTRL, 0, QCTRL,
	    hello, 5);
	hist_qrtr(r->rxf, r->rxlen, &th, 3, 2, Q_NEW_SERVER, 1, QCTRL, 0, QCTRL,
	    srv14, 5);
	hist_open(r->rxf, r->rxlen, &th, 4, "DATA5_CNTL");
	{
		uint32_t dh[4] = { G_TX_DATA | 4 << 16, 1, 32, 0 };

		hist_write(r->rxf, r->rxlen, &th, dh, 16, noise, 32);
	}
	r->desc[1] = th;
	r->desc[0] = th;
	/* the modem's FIFO (480): read by Linux but for the last packet */
	hist_cmd(r->txf, r->txlen, &rh, G_VERSION_ACK, 1, 1);
	hist_open(r->txf, r->txlen, &rh, 7, "IPCRTR");
	hist_cmd(r->txf, r->txlen, &rh, G_OPEN_ACK, 3, 0);
	for (k = 1; k <= 5; k++) {
		uint32_t m[4] = { G_INTENT | 7 << 16, 1, 1024, k };

		hist_write(r->txf, r->txlen, &rh, m, 16, NULL, 0);
	}
	hist_qrtr(r->txf, r->txlen, &rh, 7, 1, Q_HELLO, 0, QCTRL, 1, QCTRL,
	    hello, 5);
	hist_qrtr(r->txf, r->txlen, &rh, 7, 2, Q_NEW_SERVER, 0, QCTRL, 1, QCTRL,
	    srv2, 5);
	{
		uint32_t dh[4] = { G_TX_DATA | 9 << 16, 1, 32, 0 };

		hist_write(r->txf, r->txlen, &rh, dh, 16, noise, 32);
	}
	r->desc[2] = rh;
	hist_qrtr(r->txf, r->txlen, &rh, 7, 3, Q_DATA, 0, 51, 1, 0x4005, data, 5);
	r->desc[3] = rh;
	/* the modem's view */
	memset(r->ch, 0, sizeof(r->ch));
	c = ch_name(r, "IPCRTR", true);
	c->lcid = 7;
	c->rcid = 3;
	c->open_sent = c->open_acked = c->peer_open = c->intents_sent = true;
	for (k = 0; k < 5; k++) {
		c->pi[k].valid = true;
		c->pi[k].id = k + 1;
		c->pi[k].size = 1024;
		c->li[k].valid = true;
		c->li[k].id = k + 1;
		c->li[k].size = 1024;
	}
	c->nli = 5;
	c->next_liid = 6;
	c->linux_iids_below = 0x10000;
	c2 = ch_name(r, "DATA5_CNTL", true);
	c2->lcid = 9;
	c2->rcid = 4;
	c2->open_sent = c2->open_acked = c2->peer_open = c2->intents_sent = true;
	r->hello_sent = r->peer_known = r->servers_sent = true;
	r->peer_servers[0][0] = 14;
	r->peer_servers[0][1] = 0x4001;
	r->npeer = 1;
	r->rmtfs_asked = true;
	memset(&r->st, 0, sizeof(r->st));
	r->st.booted = r->st.ready = r->st.ipcrtr_open = true;
	r->st.rmtfs_opened = 1;		/* Linux's rmtfs, before */
	sim_doorbell_listen(r->bell_smp2p, kick_cb, r);
	sim_doorbell_listen(r->bell_glink, kick_cb, r);
	start_thread(r, run);
	printf("sim %s: running as Linux left it (IPCRTR 3/7)\n", r->name);
}
