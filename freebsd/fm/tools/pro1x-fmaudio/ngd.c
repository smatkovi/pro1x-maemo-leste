/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The LPASS SLIMbus NGD ("non-ported generic device") as the ADSP's
 * satellite, from user space, and the WCN3950's FM stream on that bus
 * (freebsd/fm/README.md, stage 3 step 1).
 *
 * The ADSP is the bus's manager; the application processor's NGD sends it
 * "user" messages (Linux's qcom-ngd-ctrl.c is followed step by step, its
 * functions are named below). Where Linux moves the messages through two
 * BAM DMA pipes, this uses the NGD's own message registers, the path of
 * the downstream slim-msm-ngd.c without message queues (msm_send_msg_buf
 * writes NGD_TX_MSG word by word, the interrupt handler reads NGD_RX_MSG):
 * no DMA, so no SMMU stream and no BAM driver. NGD_CFG then has ENABLE
 * without the two MSGQ_EN bits (ngd_slim_setup). Nothing is
 * interrupt-driven: NGD_INT_STAT is polled.
 *
 * The bus side, as Linux's SLIMbus core and the NGD send it:
 *   power up: the capability exchange (qcom_slim_ngd_power_up,
 *     qcom_slim_ngd_master_worker): wait for the manager's MASTER_
 *     CAPABILITY, answer REPORT_SATELLITE (magic D9 C5, version, protocol)
 *   logical addresses: ADDR_QUERY with the enumeration address,
 *     ADDR_REPLY (qcom_slim_ngd_get_laddr)
 *   value elements: CHANGE_VALUE / REQUEST_VALUE (slim_xfer_msg)
 *   the stream: CONNECT_SOURCE per port (slim_stream_prepare, sent as the
 *     user message CONNECT_SRC), DEF_ACT_CHAN and RECONFIG_NOW
 *     (qcom_slim_ngd_enable_stream); its end as the downstream NGD
 *     removes channels (CHAN_CTRL remove, RECONFIG_NOW) and the core
 *     disconnects ports (DISCONNECT_PORT)
 * Every user message carries a transaction id and is acknowledged by a
 * GENERIC_ACK with it (success: bit 0x20 of its code, downstream
 * ngd_slim_rx).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "fmaudio.h"

#define	INIT_MX_RETRIES		10	/* qcom-ngd-ctrl.c */
#define	DEF_RETRY_MS		10
#define	SLIM_CH_REMOVE		2	/* downstream enum slim_ch_control */

static uint32_t
ngd_rd(struct ngd *n, uint32_t reg)
{
	return (mmio_rd(n->m, NGD_BASE + reg));
}

static void
ngd_wr(struct ngd *n, uint32_t reg, uint32_t v)
{
	mmio_wr(n->m, NGD_BASE + reg, v);
}

static uint8_t
next_tid(struct ngd *n)
{
	/* slim_alloc_txn_tid: 1 to 255 */
	n->tid = n->tid == 255 ? 1 : n->tid + 1;
	return (n->tid);
}

static void
rx_queue(struct ngd *n, const uint8_t *b, unsigned len)
{
	struct slim_rxmsg *r;

	if (n->rxn == NGD_RXQ) {
		memmove(&n->rxq[0], &n->rxq[1], sizeof(n->rxq[0]) * (NGD_RXQ - 1));
		n->rxn--;
		n->dropped++;
	}
	r = &n->rxq[n->rxn++];
	memset(r, 0, sizeof(*r));
	r->len = (uint8_t)len;
	r->mt = (b[0] >> 5) & 0x7;	/* SLIM_HEADER_GET_MT */
	r->mc = b[1] & 0x7f;		/* SLIM_HEADER_GET_MC */
	memcpy(r->buf, b, len);
}

/*
 * One look at the interrupt status (ngd_slim_interrupt's register part):
 * a received message is read from NGD_RX_MSG and queued.
 */
static uint32_t
ngd_poll(struct ngd *n)
{
	uint32_t stat, w[10];
	uint8_t b[40];
	unsigned len, i;

	stat = ngd_rd(n, NGD_INT_STAT);
	if (stat & NGD_INT_RX_MSG_RCVD) {
		w[0] = ngd_rd(n, NGD_RX_MSG);
		len = w[0] & 0x1f;
		for (i = 1; i < ((len + 3) >> 2) && i < 10; i++)
			w[i] = ngd_rd(n, NGD_RX_MSG + 4 * i);
		ngd_wr(n, NGD_INT_CLR, NGD_INT_RX_MSG_RCVD);
		for (i = 0; i < len && i < sizeof(b); i++)
			b[i] = (uint8_t)(w[i / 4] >> (8 * (i % 4)));
		n->rx++;
		if (len < 2) {
			fma_log("slim: received a message of %u bytes", len);
			n->dropped++;
		} else {
			fma_dbg("slim: rx mt %u mc %#x len %u: %02x %02x %02x "
			    "%02x %02x %02x", (b[0] >> 5) & 7, b[1] & 0x7f, len,
			    b[0], b[1], len > 2 ? b[2] : 0, len > 3 ? b[3] : 0,
			    len > 4 ? b[4] : 0, len > 5 ? b[5] : 0);
			rx_queue(n, b, len);
		}
	}
	if (stat & (NGD_INT_RECFG_DONE | NGD_INT_IE_VE_CHG)) {
		/* the satellite only notes them (ngd_slim_interrupt) */
		ngd_wr(n, NGD_INT_CLR, stat & (NGD_INT_RECFG_DONE |
		    NGD_INT_IE_VE_CHG));
		fma_dbg("slim: %s%s", stat & NGD_INT_RECFG_DONE ?
		    "reconfiguration done " : "", stat & NGD_INT_IE_VE_CHG ?
		    "IE/VE changed" : "");
	}
	return (stat);
}

/* the queued message mt/mc (with transaction id tid at byte 3, or any) */
static int
ngd_wait(struct ngd *n, int mt, uint8_t mc, int tid, struct slim_rxmsg *out,
    unsigned timeout_ms)
{
	uint64_t until = fma_now_ms() + timeout_ms;
	unsigned i;

	for (;;) {
		for (i = 0; i < n->rxn; i++) {
			struct slim_rxmsg *r = &n->rxq[i];

			if ((mt < 0 || r->mt == mt) && r->mc == mc &&
			    (tid < 0 || (r->len > 3 && r->buf[3] == tid))) {
				if (out != NULL)
					*out = *r;
				memmove(&n->rxq[i], &n->rxq[i + 1],
				    sizeof(*r) * (n->rxn - i - 1));
				n->rxn--;
				return (0);
			}
		}
		if (fma_now_ms() > until)
			return (-ETIMEDOUT);
		ngd_poll(n);
		fma_sleep_ms(0);
	}
}

/*
 * A message to logical address la: the header (SLIM_MSG_ASM_FIRST_WORD
 * with the length of the whole message, which is what Linux's
 * txn->rl - 1 comes to, DT logical address), then the payload.
 */
size_t
ngd_build(uint8_t *msg, uint8_t mt, uint8_t mc, uint8_t la,
    const uint8_t *payload, size_t plen)
{
	uint32_t w0 = SLIM_MSG_ASM_FIRST_WORD(3 + plen, mt, mc, 0, la);

	msg[0] = (uint8_t)w0;
	msg[1] = (uint8_t)(w0 >> 8);
	msg[2] = (uint8_t)(w0 >> 16);
	memcpy(msg + 3, payload, plen);
	return (3 + plen);
}

/* msm_send_msg_buf without message queues, then TX_MSG_SENT or an error */
static int
ngd_tx(struct ngd *n, const uint8_t *msg, size_t len)
{
	uint32_t w[16], stat;
	uint64_t until;
	unsigned i;

	if (len > SLIM_MSGQ_BUF_LEN || len < 3)
		return (-EINVAL);
	memset(w, 0, sizeof(w));
	for (i = 0; i < len; i++)
		w[i / 4] |= (uint32_t)msg[i] << (8 * (i % 4));
	fma_dbg("slim: tx mt %u mc %#x la %#x len %zu", msg[0] >> 5,
	    msg[1] & 0x7f, msg[2], len);
	/* an earlier message's status must not count for this one */
	ngd_wr(n, NGD_INT_CLR, NGD_INT_TX_MSG_SENT | NGD_INT_ERRORS);
	for (i = 0; i < (len + 3) >> 2; i++)
		ngd_wr(n, NGD_TX_MSG + 4 * i, w[i]);
	until = fma_now_ms() + n->timeout_ms;
	for (;;) {
		stat = ngd_poll(n);
		if (stat & NGD_INT_ERRORS) {
			/* ngd_slim_interrupt: TX_INVAL is -EINVAL, else -EIO */
			ngd_wr(n, NGD_INT_CLR, stat & (NGD_INT_ERRORS |
			    NGD_INT_TX_MSG_SENT));
			n->last_err_stat = stat;
			if (stat & NGD_INT_TX_NACKED_2)
				n->nacks++;
			else
				n->errors++;
			fma_log("slim: tx mc %#x: error interrupt %#x", msg[1] &
			    0x7f, stat);
			return ((stat & NGD_INT_MSG_TX_INVAL) ? -EINVAL : -EIO);
		}
		if (stat & NGD_INT_TX_MSG_SENT) {
			ngd_wr(n, NGD_INT_CLR, NGD_INT_TX_MSG_SENT);
			n->tx++;
			return (0);
		}
		if (fma_now_ms() > until) {
			fma_log("slim: tx mc %#x timed out: int_stat %#x status "
			    "%#x cfg %#x", msg[1] & 0x7f, stat,
			    ngd_rd(n, NGD_STATUS), ngd_rd(n, NGD_CFG));
			return (-ETIMEDOUT);
		}
		fma_sleep_ms(0);
	}
}

static int
ngd_send(struct ngd *n, uint8_t mt, uint8_t mc, uint8_t la,
    const uint8_t *payload, size_t plen)
{
	uint8_t msg[SLIM_MSGQ_BUF_LEN];

	if (3 + plen > sizeof(msg))
		return (-EINVAL);
	return (ngd_tx(n, msg, ngd_build(msg, mt, mc, la, payload, plen)));
}

/* a user message to the manager with tid in it, and its GENERIC_ACK */
static int
ngd_usr_acked(struct ngd *n, uint8_t mc, const uint8_t *payload, size_t plen,
    uint8_t tid, const char *what)
{
	struct slim_rxmsg m;
	int r;

	r = ngd_send(n, SLIM_MSG_MT_DEST_REFERRED_USER, mc, SLIM_LA_MGR,
	    payload, plen);
	if (r != 0) {
		fma_log("slim: %s: not sent (%s)", what, strerror(-r));
		return (r);
	}
	r = ngd_wait(n, SLIM_MSG_MT_SRC_REFERRED_USER, SLIM_USR_MC_GENERIC_ACK,
	    tid, &m, n->timeout_ms);
	if (r != 0) {
		fma_log("slim: %s: no acknowledgement", what);
		return (r);
	}
	if (m.len < 5 || !(m.buf[4] & MSM_SAT_SUCCSS)) {
		fma_log("slim: %s: NACK, code %#x", what, m.len > 4 ?
		    m.buf[4] : 0);
		return (-EIO);
	}
	fma_dbg("slim: %s: acknowledged (%#x)", what, m.buf[4]);
	return (0);
}

void
ngd_dump(struct ngd *n, const char *when)
{
	fma_log("slim: NGD %s: base %#x cfg %#x status %#x rx_msgq %#x "
	    "int_en %#x int_stat %#x ie %#x ve %#x", when, mmio_rd(n->m, 0),
	    ngd_rd(n, NGD_CFG), ngd_rd(n, NGD_STATUS),
	    ngd_rd(n, NGD_RX_MSGQ_CFG), ngd_rd(n, NGD_INT_EN),
	    ngd_rd(n, NGD_INT_STAT), ngd_rd(n, NGD_IE_STAT),
	    ngd_rd(n, NGD_VE_STAT));
}

/* ngd_slim_setup without message queues: ENABLE alone */
static void
ngd_setup(struct ngd *n)
{
	if (ngd_rd(n, NGD_CFG) != NGD_CFG_ENABLE)
		ngd_wr(n, NGD_CFG, NGD_CFG_ENABLE);
}

/*
 * qcom_slim_ngd_power_up, after the QMI power request (the caller's):
 * only then may the registers be read.
 */
int
ngd_power_up(struct ngd *n)
{
	struct slim_rxmsg m;
	static const uint8_t sat[] = { SAT_MAGIC_LSB, SAT_MAGIC_MSB,
	    SAT_MSG_VER, SAT_MSG_PROT };
	uint32_t rx;
	int i, r = -ETIMEDOUT;

	if (n->timeout_ms == 0)
		n->timeout_ms = 1000;
	/* the version in the controller's 16 high bits */
	n->ver = mmio_rd(n->m, 0) >> 16;
	fma_log("slim: controller version %#x", n->ver);
	ngd_dump(n, "before");
	if (ngd_rd(n, NGD_STATUS) & NGD_LADDR) {
		/* retained: enumerated, the satellite known (downstream) */
		if (ngd_rd(n, NGD_INT_EN) == 0)
			ngd_wr(n, NGD_INT_EN, DEF_NGD_INT_MASK);
		rx = ngd_rd(n, NGD_RX_MSGQ_CFG);
		ngd_wr(n, NGD_RX_MSGQ_CFG, rx | SLIM_RX_MSGQ_TIMEOUT_VAL);
		ngd_setup(n);
		fma_log("slim: the NGD has its logical address already");
		n->up = true;
		return (0);
	}
	for (i = 0; i < INIT_MX_RETRIES && r != 0; i++) {
		ngd_wr(n, NGD_INT_EN, DEF_NGD_INT_MASK);
		rx = ngd_rd(n, NGD_RX_MSGQ_CFG);
		ngd_wr(n, NGD_RX_MSGQ_CFG, rx | SLIM_RX_MSGQ_TIMEOUT_VAL);
		ngd_setup(n);
		r = ngd_wait(n, SLIM_MSG_MT_SRC_REFERRED_USER,
		    SLIM_USR_MC_MASTER_CAPABILITY, -1, &m, n->timeout_ms);
		if (r != 0)
			fma_log("slim: capability timed out (%d): status %#x "
			    "cfg %#x int_stat %#x", i, ngd_rd(n, NGD_STATUS),
			    ngd_rd(n, NGD_CFG), ngd_rd(n, NGD_INT_STAT));
	}
	if (r != 0) {
		ngd_dump(n, "without the manager's capability");
		return (r);
	}
	fma_log("slim: the manager's capability (%u bytes)", m.len);
	/* qcom_slim_ngd_master_worker: NACKs retried */
	for (i = 0; ; i++) {
		r = ngd_send(n, SLIM_MSG_MT_SRC_REFERRED_USER,
		    SLIM_USR_MC_REPORT_SATELLITE, SLIM_LA_MGR, sat, sizeof(sat));
		if (r != -EIO || i >= INIT_MX_RETRIES)
			break;
		fma_log("slim: capability message NACKed, retrying");
		fma_sleep_ms(DEF_RETRY_MS);
	}
	if (r != 0) {
		fma_log("slim: capability TX failed: %s", strerror(-r));
		return (r);
	}
	n->up = true;
	fma_log("slim: satellite reported; NGD status %#x", ngd_rd(n,
	    NGD_STATUS));
	return (0);
}

/* qcom_slim_ngd_get_laddr: 0, -ENXIO (the manager does not know it) */
int
ngd_get_laddr(struct ngd *n, const struct slim_ea *ea, uint8_t *la)
{
	static const uint8_t failed[6];
	struct slim_rxmsg m;
	uint8_t p[7];
	int r;

	p[0] = next_tid(n);
	p[1] = ea->instance;
	p[2] = ea->dev_index;
	le16enc_(p + 3, ea->prod_code);
	le16enc_(p + 5, ea->manf_id);
	r = ngd_send(n, SLIM_MSG_MT_DEST_REFERRED_USER, SLIM_USR_MC_ADDR_QUERY,
	    SLIM_LA_MGR, p, sizeof(p));
	if (r != 0)
		return (r);
	r = ngd_wait(n, SLIM_MSG_MT_SRC_REFERRED_USER, SLIM_USR_MC_ADDR_REPLY,
	    p[0], &m, n->timeout_ms);
	if (r != 0)
		return (r);
	if (m.len < 11 || memcmp(m.buf + 4, failed, 6) == 0)
		return (-ENXIO);
	*la = m.buf[10];
	return (0);
}

/* slim_slicesize: the slice size code of a value element access */
static uint16_t
slim_slicesize(unsigned count)
{
	static const uint8_t sizetocode[16] = {
		0, 1, 2, 3, 3, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7
	};

	if (count < 1)
		count = 1;
	if (count > 16)
		count = 16;
	return (sizetocode[count - 1]);
}

static uint16_t
slim_ec(uint16_t addr, unsigned count)
{
	return ((uint16_t)(slim_slicesize(count) | (1 << 3) |
	    ((addr & 0xfff) << 4)));
}

/* slim_write: CHANGE_VALUE, its element code, the bytes; not answered */
int
ngd_write_ve(struct ngd *n, uint8_t la, uint16_t addr, const uint8_t *val,
    unsigned count)
{
	uint8_t p[2 + 16];

	if (count < 1 || count > 16 || addr + count > 0xc00)
		return (-EINVAL);
	le16enc_(p, slim_ec(addr, count));
	memcpy(p + 2, val, count);
	return (ngd_send(n, SLIM_MSG_MT_CORE, SLIM_MSG_MC_CHANGE_VALUE, la, p,
	    2 + count));
}

/* slim_read: REQUEST_VALUE with a transaction id, REPLY_VALUE */
int
ngd_read_ve(struct ngd *n, uint8_t la, uint16_t addr, uint8_t *val,
    unsigned count)
{
	struct slim_rxmsg m;
	uint8_t p[3];
	int r;

	if (count < 1 || count > 16 || addr + count > 0xc00)
		return (-EINVAL);
	p[0] = next_tid(n);
	le16enc_(p + 1, slim_ec(addr, count));
	r = ngd_send(n, SLIM_MSG_MT_CORE, SLIM_MSG_MC_REQUEST_VALUE, la, p, 3);
	if (r != 0)
		return (r);
	r = ngd_wait(n, -1, SLIM_MSG_MC_REPLY_VALUE, p[0], &m, n->timeout_ms);
	if (r != 0)
		return (r);
	if (m.len < 4 + count)
		return (-EIO);
	memcpy(val, m.buf + 4, count);
	return (0);
}

/* --- the FM stream ---------------------------------------------------- */

/*
 * qcom_slim_calc_coef for the channel rate multiplier: C x 2^E with C 1
 * or 3; returns 1 for C = 3 (bit 5 of the DEF_ACT_CHAN parameters), 0 for
 * C = 1, < 0 out of range.
 */
int
slim_coef(unsigned ratem, int *exp)
{
	unsigned coef = ratem;

	*exp = 0;
	if (coef == 0)
		return (-1);
	for (;;) {
		while ((coef & 1) != 1) {
			coef >>= 1;
			(*exp)++;
		}
		if (coef <= 3)
			break;
		coef++;
	}
	if (coef == 1)
		return (*exp > 9 ? -1 : 0);
	return (*exp > 8 ? -1 : 1);
}

/* slim_presence_rate_table (stream.c): the index of the rate */
static int
slim_prate_code(unsigned rate)
{
	static const unsigned table[] = {
		0, 12000, 24000, 48000, 96000, 192000, 384000, 768000, 0,
		11025, 22050, 44100, 88200, 176400, 352800, 705600, 4000,
		8000, 16000, 32000, 64000, 128000, 256000, 512000,
	};
	unsigned i;

	for (i = 1; i < sizeof(table) / sizeof(table[0]); i++)
		if (table[i] == rate)
			return ((int)i);
	return (-1);
}

/*
 * The WCN3950's FM ports (wcn399x_fm_ports of patch 0019, downstream
 * btfm_slim_slave_enable_port): on: both ports in one multi-channel
 * group, over/underrun recovery, enabled with watermark 8; off: the
 * watermark alone. Through the interface device.
 */
int
fm_ports(struct ngd *n, struct slim_fm *fm, bool on)
{
	uint8_t v;
	unsigned i;
	int r;

	for (i = 0; i < fm->nports; i++) {
		uint8_t p = fm->port[i];

		if (on) {
			v = (uint8_t)((1u << fm->port[0]) |
			    (fm->nports > 1 ? 1u << fm->port[1] : 0));
			if ((r = ngd_write_ve(n, fm->ifd_la,
			    WCN399X_TX_PORT_MULTI_CHNL_0(p), &v, 1)) != 0)
				goto fail;
			v = WCN399X_OVERRUN_RECOVERY | WCN399X_UNDERRUN_RECOVERY;
			if ((r = ngd_write_ve(n, fm->ifd_la,
			    WCN399X_TX_PORT_OR_UR_CFG(p), &v, 1)) != 0)
				goto fail;
		}
		v = (on ? WCN399X_PORT_ENABLE : 0) | WCN399X_PORT_WM_L8;
		if ((r = ngd_write_ve(n, fm->ifd_la, WCN399X_TX_PORT_CFG(p), &v,
		    1)) != 0)
			goto fail;
	}
	fm->ports_on = on;
	fma_log("slim: WCN3950 FM ports %u%s%u %s", fm->port[0],
	    fm->nports > 1 ? "," : "", fm->nports > 1 ? fm->port[1] : 0,
	    on ? "enabled" : "disabled");
	return (0);
fail:
	fma_log("slim: WCN3950 port %u registers: %s", fm->port[i],
	    strerror(-r));
	return (r);
}

/*
 * slim_stream_prepare (CONNECT_SOURCE per port, which the NGD sends as
 * CONNECT_SRC {device, port, channel, tid}) and
 * qcom_slim_ngd_enable_stream (DEF_ACT_CHAN, RECONFIG_NOW). 48 kHz on a
 * bus whose superframe runs at 4000 Hz (rootfreq 24.576 MHz / 8 / 768):
 * isochronous, rate multiplier 12 = 3 x 2^2, presence rate 48 kHz
 * (code 3) with the frequency-locked bit.
 */
int
fm_stream_enable(struct ngd *n, struct slim_fm *fm)
{
	const unsigned superfreq = (24576000 >> 3) / (6144 >> 3);
	uint8_t p[SLIM_MSGQ_BUF_LEN], tid;
	unsigned i, ratem, prot, len;
	char what[64];
	int coef, exp, prrate, r;

	prrate = slim_prate_code(fm->rate);
	if (prrate < 0)
		return (-EINVAL);
	/* SLIM_PROTO_ISO if the rate is a multiple of the superframe's */
	prot = (fm->rate % superfreq) ? 2 /* SLIM_PROTO_PULL, capture */ : 0;
	ratem = fm->rate / superfreq;
	if (ratem * superfreq < fm->rate)
		ratem++;
	if ((coef = slim_coef(ratem, &exp)) < 0)
		return (-EIO);

	for (i = 0; i < fm->nports; i++) {
		tid = next_tid(n);
		p[0] = fm->pgd_la;
		p[1] = fm->port[i];
		p[2] = fm->ch[i];
		p[3] = tid;
		snprintf(what, sizeof(what), "connect port %u to channel %u",
		    fm->port[i], fm->ch[i]);
		if ((r = ngd_usr_acked(n, SLIM_USR_MC_CONNECT_SRC, p, 4, tid,
		    what)) != 0)
			return (r);
	}
	fm->connected = true;

	len = 0;
	tid = next_tid(n);
	p[len++] = fm->pgd_la;
	/* bits / 4, aux format 0 << 6, the coefficient's bit */
	p[len++] = (uint8_t)((fm->bps >> 2) | (coef ? 1u << 5 : 0));
	p[len++] = (uint8_t)(exp << 4 | prot);
	p[len++] = (uint8_t)(prrate | (prot == 0 ? 0x80 : 0)); /* FL */
	p[len++] = tid;
	for (i = 0; i < fm->nports; i++)
		p[len++] = fm->ch[i];
	snprintf(what, sizeof(what), "define and activate channels %u,%u",
	    fm->ch[0], fm->nports > 1 ? fm->ch[1] : 0);
	if ((r = ngd_usr_acked(n, SLIM_USR_MC_DEF_ACT_CHAN, p, len, tid,
	    what)) != 0)
		return (r);
	tid = next_tid(n);
	p[0] = tid;
	p[1] = fm->pgd_la;
	if ((r = ngd_usr_acked(n, SLIM_USR_MC_RECONFIG_NOW, p, 2, tid,
	    "reconfigure now")) != 0)
		return (r);
	fm->active = true;
	fma_log("slim: FM stream active: %u Hz, %u bit, channels %u,%u "
	    "(rate multiplier %u, protocol %u)", fm->rate, fm->bps, fm->ch[0],
	    fm->nports > 1 ? fm->ch[1] : 0, ratem, prot);
	return (0);
}

/*
 * The stream's end: the channels removed as the downstream NGD does it
 * (CHAN_CTRL {remove << 6 | device, tid, channels}, RECONFIG_NOW; Linux's
 * NGD drops the core's deactivate/remove messages), then the ports
 * disconnected (slim_stream_unprepare: DISCONNECT_PORT {device, port,
 * tid}). Every step is tried.
 */
int
fm_stream_disable(struct ngd *n, struct slim_fm *fm)
{
	uint8_t p[SLIM_MSGQ_BUF_LEN], tid;
	unsigned i, len;
	char what[64];
	int r, rc = 0;

	if (fm->active) {
		len = 0;
		tid = next_tid(n);
		p[len++] = (uint8_t)(SLIM_CH_REMOVE << 6 | (fm->pgd_la & 0x1f));
		p[len++] = tid;
		for (i = 0; i < fm->nports; i++)
			p[len++] = fm->ch[i];
		if ((r = ngd_usr_acked(n, SLIM_USR_MC_CHAN_CTRL, p, len, tid,
		    "remove channels")) != 0)
			rc = r;
		tid = next_tid(n);
		p[0] = tid;
		p[1] = fm->pgd_la;
		if ((r = ngd_usr_acked(n, SLIM_USR_MC_RECONFIG_NOW, p, 2, tid,
		    "reconfigure now")) != 0)
			rc = r;
		fm->active = false;
	}
	if (fm->connected) {
		for (i = 0; i < fm->nports; i++) {
			tid = next_tid(n);
			p[0] = fm->pgd_la;
			p[1] = fm->port[i];
			p[2] = tid;
			snprintf(what, sizeof(what), "disconnect port %u",
			    fm->port[i]);
			if ((r = ngd_usr_acked(n, SLIM_USR_MC_DISCONNECT_PORT, p,
			    3, tid, what)) != 0)
				rc = r;
		}
		fm->connected = false;
	}
	return (rc);
}
