/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * gsi.c and ipa_hw.c on the PC against a model of the IPA v4.2 and its GSI:
 * registers that hold what is written (the read-only fields kept apart),
 * channel, event ring and GENERIC commands with their interrupts, channels
 * that take their TREs at the doorbell (the immediate commands decoded and
 * applied, the pipeline clear's tagged packet into the exception endpoint,
 * the modem's uplink frames collected), receive buffers filled by the test,
 * events with IEOB (BEI: the moderation timer's interrupt, later), and an
 * interrupt thread calling gsi_isr() as the "gsi" interrupt would.
 *
 * Checked, with values worked out by hand from Linux 7.1's ipa_main.c,
 * ipa_endpoint.c, gsi.c, ipa_mem.c, ipa_table.c and ipa_resource.c for the
 * v4.2 data: every register ipa_config() and ipa_setup() write, the GSI's
 * channel and event ring programming, the immediate commands of the
 * memory and table setup with their payloads, the table memory; then the
 * modem's data path: 1000 aggregated receive buffers (the rings wrap
 * several times), 3000 uplink frames of 1 to 3 segments, the crash path
 * (flow control, HOLB, table reset, the exception reset with its pipeline
 * clear, the modem's memory zeroed) and the teardown (every channel and
 * event ring deallocated, the modem's channels halted). Under ASan/UBSan.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>

#include "ipa_var.h"
#include "qmap.h"

#define	CHK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, \
	__LINE__); printf(__VA_ARGS__); printf("\n"); exit(1); } } while (0)

/* --- the model ---------------------------------------------------------------- */

static pthread_mutex_t mlock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t ipa_regs[0x7000 / 4], shared[0x2000 / 4], gsi[0x23000 / 4];
static uint32_t ch_state[32], ev_state[32];
static uint32_t ch_rp[32], ch_wp[32];		/* TRE indices (mod count) */
static uint32_t ev_wp[32], ev_driver[32];	/* event write, driver's last */
static uint64_t ev_total[32];
static uint32_t src_ch, src_ev, src_ieob, glob, moderated;
static int generic_ops[32][8];			/* [channel][opcode] */
static bool irq_thread_run = true;
static int verbose;

struct cmd_rec {
	uint16_t	opcode;
	uint8_t		payload[24];
};
static struct cmd_rec cmds[512];
static int ncmds;

struct tx_frame {
	uint8_t		data[4096];
	uint32_t	len;
	int		nseg;
};
static struct tx_frame txf[64];
static int ntxf;
static uint64_t txf_total;

#define	G(off)	gsi[(off) / 4]

static uint64_t
ch_base(int c)
{
	return ((uint64_t)G(GSI_CH_C_CNTXT_3(c)) << 32 | G(GSI_CH_C_CNTXT_2(c)));
}

static uint32_t
ch_count(int c)
{
	return (IPA_GET(CH_R_LENGTH, G(GSI_CH_C_CNTXT_1(c))) / 16);
}

static uint64_t
ev_base(int e)
{
	return ((uint64_t)G(GSI_EV_CH_E_CNTXT_3(e)) << 32 | G(GSI_EV_CH_E_CNTXT_2(e)));
}

static uint32_t
ev_count(int e)
{
	return (IPA_GET(EV_R_LENGTH, G(GSI_EV_CH_E_CNTXT_1(e))) / 16);
}

static int
ch_evt(int c)
{
	return (IPA_GET(ERINDEX, G(GSI_CH_C_CNTXT_0(c))));
}

/* an event for TRE index idx of channel c (mlock held) */
static void
model_event(int c, uint32_t idx, uint16_t len, bool bei)
{
	int e = ch_evt(c);
	uint32_t n = ev_count(e), pending;
	struct gsi_event *ev;

	/* the driver's doorbell names the last event it handled */
	pending = (ev_wp[e] - ((ev_driver[e] + 1) % n) + n) % n;
	CHK(pending < n - 1, "event ring %d overflows (%u pending)", e, pending);
	ev = (struct gsi_event *)(uintptr_t)(ev_base(e) + (uint64_t)ev_wp[e] * 16);
	memset(ev, 0, sizeof(*ev));
	ev->xfer_ptr = htole64(ch_base(c) + (uint64_t)idx * 16);
	ev->len = htole16(len);
	ev->chid = c;
	ev->type = 0x22;
	ev_wp[e] = (ev_wp[e] + 1) % n;
	ev_total[e]++;
	if (bei)
		moderated |= 1U << e;
	else
		src_ieob |= 1U << e;
}

/* the pipeline clear's packet: status with a tag, then its data (LAN RX) */
static void
model_tagged_packet(uint8_t src_ep, uint8_t dst_ep, uint32_t len)
{
	int c = 2;	/* AP_LAN_RX */
	uint32_t n = ch_count(c), w[8];
	struct gsi_tre *tre;
	uint8_t *buf;

	CHK(ch_state[c] == GSI_CHANNEL_STATE_STARTED, "LAN RX not started");
	CHK(ch_rp[c] != ch_wp[c], "no LAN RX buffer for the tagged packet");
	tre = (struct gsi_tre *)(uintptr_t)(ch_base(c) + (uint64_t)ch_rp[c] * 16);
	buf = (uint8_t *)(uintptr_t)le64toh(tre->addr);
	memset(w, 0, sizeof(w));
	w[0] = htole32(IPA_STATUS_OPCODE_PACKET | 0x10U << 16);	/* TAG_VALID */
	w[1] = htole32(len | (uint32_t)src_ep << 16 | (uint32_t)dst_ep << 24);
	memcpy(buf, w, 32);
	memset(buf + 32, 0, roundup2(len, 4));
	model_event(c, ch_rp[c], 32 + roundup2(len, 4), false);
	ch_rp[c] = (ch_rp[c] + 1) % n;
}

/* the TREs from the read to the write pointer of a channel toward the IPA */
static void
model_tx(int c)
{
	uint32_t n = ch_count(c), flags, len, total = 0;
	struct gsi_tre *tre;
	struct tx_frame *f = NULL;
	int nseg = 0;
	bool pkt_init = false, tag = false;
	uint8_t dest = 0;

	while (ch_rp[c] != ch_wp[c]) {
		tre = (struct gsi_tre *)(uintptr_t)(ch_base(c) +
		    (uint64_t)ch_rp[c] * 16);
		flags = le32toh(tre->flags);
		len = le16toh(tre->len_opcode);
		if (c == 1) {
			/* the command channel */
			if (IPA_GET(TRE_FLAGS_TYPE, flags) == GSI_RE_IMMD_CMD) {
				struct cmd_rec *r = &cmds[ncmds++ % 512];
				const uint8_t *pl = (const uint8_t *)(uintptr_t)
				    le64toh(tre->addr);

				r->opcode = len;
				memcpy(r->payload, pl, 24);
				if ((len & 0xff) == IPA_CMD_IP_PACKET_INIT) {
					pkt_init = true;
					dest = pl[0];
				} else if ((len & 0xff) == IPA_CMD_IP_PACKET_TAG_STATUS)
					tag = true;
				else if ((len & 0xff) == IPA_CMD_DMA_SHARED_MEM) {
					const struct ipa_cmd_hw_dma_mem_mem *d =
					    (const void *)pl;
					uint16_t sz = le16toh(d->size);
					uint16_t la = le16toh(d->local_addr);

					CHK(la + sz <= sizeof(shared), "DMA beyond "
					    "the shared memory");
					memcpy((uint8_t *)shared + la, (const void *)
					    (uintptr_t)le64toh(d->system_addr), sz);
				} else if ((len & 0xff) == IPA_CMD_REGISTER_WRITE) {
					const struct ipa_cmd_register_write *rw =
					    (const void *)pl;
					uint32_t off = le16toh(rw->offset) |
					    IPA_GET(REGISTER_WRITE_FLAGS_OFFSET_HIGH,
					    le16toh(rw->flags)) << 16;
					uint32_t m = le32toh(rw->value_mask);

					if (m != 0)
						ipa_regs[off / 4] = (ipa_regs[off / 4] &
						    ~m) | (le32toh(rw->value) & m);
				}
			} else if (pkt_init && tag) {
				/* the data of the pipeline clear */
				model_tagged_packet(6, dest, len);
				pkt_init = tag = false;
			}
		} else {
			if (f == NULL) {
				f = &txf[ntxf % 64];
				f->len = 0;
				nseg = 0;
			}
			CHK(f->len + len <= sizeof(f->data), "frame too long");
			memcpy(f->data + f->len, (const void *)(uintptr_t)
			    le64toh(tre->addr), len);
			f->len += len;
			nseg++;
		}
		total += len;
		if (flags & TRE_FLAGS_IEOT) {
			CHK((flags & TRE_FLAGS_CHAIN) == 0, "IEOT with CHAIN");
			if (f != NULL) {
				f->nseg = nseg;
				ntxf++;
				txf_total++;
				f = NULL;
			}
			model_event(c, ch_rp[c], total, (flags & TRE_FLAGS_BEI) != 0);
			total = 0;
		} else
			CHK(flags & TRE_FLAGS_CHAIN, "TRE without CHAIN or IEOT");
		ch_rp[c] = (ch_rp[c] + 1) % n;
	}
}

/* data from the modem into the next buffer of channel c; false: none */
static bool
model_rx(int c, const uint8_t *data, uint32_t len)
{
	uint32_t n;
	struct gsi_tre *tre;
	bool ok = false;

	pthread_mutex_lock(&mlock);
	n = ch_count(c);
	if (ch_state[c] == GSI_CHANNEL_STATE_STARTED && ch_rp[c] != ch_wp[c]) {
		tre = (struct gsi_tre *)(uintptr_t)(ch_base(c) +
		    (uint64_t)ch_rp[c] * 16);
		CHK(len <= le16toh(tre->len_opcode), "buffer too small");
		CHK(IPA_GET(TRE_FLAGS_TYPE, le32toh(tre->flags)) == GSI_RE_XFER &&
		    (le32toh(tre->flags) & TRE_FLAGS_IEOT) &&
		    !(le32toh(tre->flags) & TRE_FLAGS_BEI), "RX TRE flags");
		memcpy((void *)(uintptr_t)le64toh(tre->addr), data, len);
		model_event(c, ch_rp[c], len, false);
		ch_rp[c] = (ch_rp[c] + 1) % n;
		ok = true;
	}
	pthread_mutex_unlock(&mlock);
	return (ok);
}

static uint32_t
model_type_irq(void)
{
	uint32_t t = 0;

	if (src_ch & G(GSI_CNTXT_SRC_CH_IRQ_MSK))
		t |= GSI_CH_CTRL;
	if (src_ev & G(GSI_CNTXT_SRC_EV_CH_IRQ_MSK))
		t |= GSI_EV_CTRL;
	if (glob & G(GSI_CNTXT_GLOB_IRQ_EN))
		t |= GSI_GLOB_EE;
	if (src_ieob & G(GSI_CNTXT_SRC_IEOB_IRQ_MSK))
		t |= GSI_IEOB;
	return (t);
}

static uint32_t
gsi_read(uint32_t off)
{
	uint32_t i;

	if (off == GSI_STATUS)
		return (GSI_STATUS_ENABLED);
	if (off == GSI_HW_PARAM_2)
		return (IPA_FIELD(NUM_CH_PER_EE, 8) | IPA_FIELD(NUM_EV_PER_EE, 8));
	if (off == GSI_CNTXT_TYPE_IRQ)
		return (model_type_irq());
	if (off == GSI_CNTXT_SRC_CH_IRQ)
		return (src_ch);
	if (off == GSI_CNTXT_SRC_EV_CH_IRQ)
		return (src_ev);
	if (off == GSI_CNTXT_SRC_IEOB_IRQ)
		return (src_ieob);
	if (off == GSI_CNTXT_GLOB_IRQ_STTS)
		return (glob);
	for (i = 0; i < 32; i++) {
		if (off == GSI_CH_C_CNTXT_0(i))
			return ((G(off) & ~CHSTATE) | IPA_FIELD(CHSTATE, ch_state[i]));
		if (off == GSI_EV_CH_E_CNTXT_0(i))
			return ((G(off) & ~EV_CHSTATE) |
			    IPA_FIELD(EV_CHSTATE, ev_state[i]));
		if (off == GSI_EV_CH_E_CNTXT_4(i))
			return ((uint32_t)ev_base(i) + ev_wp[i] * 16);
	}
	return (G(off));
}

static void
gsi_write(uint32_t off, uint32_t v)
{
	uint32_t id, op, i;

	switch (off) {
	case GSI_CH_CMD:
		id = IPA_GET(CH_CMD_CHID, v);
		op = IPA_GET(CH_CMD_OPCODE, v);
		switch (op) {
		case GSI_CH_ALLOCATE:
			ch_state[id] = GSI_CHANNEL_STATE_ALLOCATED;
			ch_rp[id] = ch_wp[id] = 0;
			break;
		case GSI_CH_START:
			ch_state[id] = GSI_CHANNEL_STATE_STARTED;
			if (G(GSI_CH_C_CNTXT_0(id)) & CHTYPE_DIR)
				model_tx(id);
			break;
		case GSI_CH_STOP:
			ch_state[id] = GSI_CHANNEL_STATE_STOPPED;
			break;
		case GSI_CH_RESET:
			ch_state[id] = GSI_CHANNEL_STATE_ALLOCATED;
			ch_rp[id] = ch_wp[id] = 0;
			break;
		case GSI_CH_DE_ALLOC:
			ch_state[id] = GSI_CHANNEL_STATE_NOT_ALLOCATED;
			break;
		default:
			CHK(0, "channel command %#x", op);
		}
		src_ch |= 1U << id;
		return;
	case GSI_EV_CH_CMD:
		id = IPA_GET(EV_CMD_CHID, v);
		op = IPA_GET(EV_CMD_OPCODE, v);
		if (op == GSI_EVT_ALLOCATE || op == GSI_EVT_RESET) {
			ev_state[id] = GSI_EVT_RING_STATE_ALLOCATED;
			ev_wp[id] = 0;
		} else if (op == GSI_EVT_DE_ALLOC)
			ev_state[id] = GSI_EVT_RING_STATE_NOT_ALLOCATED;
		else
			CHK(0, "event ring command %#x", op);
		src_ev |= 1U << id;
		return;
	case GSI_GENERIC_CMD:
		CHK(IPA_GET(GENERIC_EE, v) == GSI_EE_MODEM, "generic command EE");
		generic_ops[IPA_GET(GENERIC_CHID, v)][IPA_GET(GENERIC_OPCODE, v)]++;
		G(GSI_CNTXT_SCRATCH_0) = (G(GSI_CNTXT_SCRATCH_0) & ~GENERIC_EE_RESULT) |
		    IPA_FIELD(GENERIC_EE_RESULT, GENERIC_EE_SUCCESS);
		glob |= GSI_GP_INT1;
		return;
	case GSI_CNTXT_SRC_CH_IRQ_CLR:
		src_ch &= ~v;
		return;
	case GSI_CNTXT_SRC_EV_CH_IRQ_CLR:
		src_ev &= ~v;
		return;
	case GSI_CNTXT_SRC_IEOB_IRQ_CLR:
		src_ieob &= ~v;
		return;
	case GSI_CNTXT_GLOB_IRQ_CLR:
		glob &= ~v;
		return;
	}
	for (i = 0; i < 32; i++) {
		if (off == GSI_CH_C_DOORBELL_0(i)) {
			uint32_t idx = (v - (uint32_t)ch_base(i)) / 16;

			CHK(idx < ch_count(i), "channel %d doorbell %#x", i, v);
			ch_wp[i] = idx;
			if (ch_state[i] == GSI_CHANNEL_STATE_STARTED &&
			    (G(GSI_CH_C_CNTXT_0(i)) & CHTYPE_DIR))
				model_tx(i);
			return;
		}
		if (off == GSI_EV_CH_E_DOORBELL_0(i)) {
			uint32_t idx = (v - (uint32_t)ev_base(i)) / 16;

			CHK(idx < ev_count(i), "event ring %d doorbell %#x", i, v);
			ev_driver[i] = idx;
			return;
		}
	}
	G(off) = v;
}

/* the "gsi" interrupt; the moderation timer for BEI events */
static void *
irq_thread(void *arg)
{
	struct ipa *ipa = arg;
	uint32_t line;
	int tick = 0;

	while (irq_thread_run) {
		pthread_mutex_lock(&mlock);
		if (++tick % 20 == 0 && moderated) {
			src_ieob |= moderated;
			moderated = 0;
		}
		line = model_type_irq() & G(GSI_CNTXT_TYPE_IRQ_MSK);
		pthread_mutex_unlock(&mlock);
		if (line)
			gsi_isr(ipa);
		usleep(50);
	}
	return (NULL);
}

/* --- the system's side ------------------------------------------------------------ */

static struct ipa IPA;

uint32_t
ipa_os_read(struct ipa *ipa, int region, uint32_t off)
{
	uint32_t v;

	pthread_mutex_lock(&mlock);
	if (region == IPA_REGION_GSI)
		v = gsi_read(off);
	else if (region == IPA_REGION_SHARED) {
		CHK(off < sizeof(shared), "shared read %#x", off);
		v = shared[off / 4];
	} else {
		CHK(off < sizeof(ipa_regs), "register read %#x", off);
		if (off == IPA_FLAVOR_0)
			/* 7 TX (consumer) endpoints from 0, 5 RX from 7 */
			v = IPA_FIELD(FLAVOR_MAX_CONS_PIPES, 7) |
			    IPA_FIELD(FLAVOR_MAX_PROD_PIPES, 5) |
			    IPA_FIELD(FLAVOR_PROD_LOWEST, 7);
		else if (off == IPA_SHARED_MEM_SIZE)
			v = IPA_FIELD(MEM_SIZE, 0x2000 / 8);
		else
			v = ipa_regs[off / 4];
	}
	pthread_mutex_unlock(&mlock);
	return (v);
}

void
ipa_os_write(struct ipa *ipa, int region, uint32_t off, uint32_t val)
{
	pthread_mutex_lock(&mlock);
	if (region == IPA_REGION_GSI) {
		CHK(off < sizeof(gsi), "GSI write %#x", off);
		gsi_write(off, val);
	} else if (region == IPA_REGION_SHARED) {
		CHK(off < sizeof(shared), "shared write %#x", off);
		shared[off / 4] = val;
	} else {
		CHK(off < sizeof(ipa_regs), "register write %#x", off);
		ipa_regs[off / 4] = val;
	}
	pthread_mutex_unlock(&mlock);
}

int
ipa_os_dma_alloc(struct ipa *ipa, struct ipa_dma *d, size_t size, size_t align,
    int flags)
{
	size_t a = MAX(align, 64);

	d->va = aligned_alloc(a, roundup2(size, a));
	if (d->va == NULL)
		return (ENOMEM);
	memset(d->va, 0, size);
	d->pa = (uint64_t)(uintptr_t)d->va;
	d->size = size;
	d->os = d->va;
	return (0);
}

void
ipa_os_dma_free(struct ipa *ipa, struct ipa_dma *d)
{
	free(d->os);
	memset(d, 0, sizeof(*d));
}

void
ipa_os_dma_sync(struct ipa *ipa, struct ipa_dma *d, int how)
{
}

void
ipa_os_log(struct ipa *ipa, const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;
	va_start(ap, fmt);
	printf("    ipa: ");
	vprintf(fmt, ap);
	va_end(ap);
}

/* the poll "task queue": a thread polling the channels asked for */
static pthread_mutex_t plock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pcond = PTHREAD_COND_INITIALIZER;
static uint32_t poll_pending;
static bool poll_run = true;

void
ipa_os_poll_schedule(struct ipa *ipa, struct gsi_channel *ch)
{
	pthread_mutex_lock(&plock);
	poll_pending |= 1U << ch->id;
	pthread_cond_broadcast(&pcond);
	pthread_mutex_unlock(&plock);
}

static void *
poll_thread(void *arg)
{
	struct ipa *ipa = arg;
	uint32_t p;
	int c;

	pthread_mutex_lock(&plock);
	while (poll_run) {
		if ((p = poll_pending) == 0) {
			pthread_cond_wait(&pcond, &plock);
			continue;
		}
		poll_pending = 0;
		pthread_mutex_unlock(&plock);
		for (c = 0; c < 32; c++)
			if (p & (1U << c))
				gsi_channel_poll(&ipa->channel[c]);
		pthread_mutex_lock(&plock);
	}
	pthread_mutex_unlock(&plock);
	return (NULL);
}

static atomic_int rx_pkts, rx_bad, tx_done_n, modem_up_n, modem_down_n;
static atomic_int tx_done_seen[4096];
static struct qmap_stats qst;

static void
rx_pkt(void *arg, const struct qmap_pkt *p)
{
	if (p->mux_id != 1 || p->version != 4 || p->csum == QMAP_CSUM_BAD)
		rx_bad++;
	rx_pkts++;
}

void
ipa_os_rx(struct ipa *ipa, struct ipa_endpoint *ep, const uint8_t *buf,
    uint32_t len)
{
	CHK(ep->endpoint_id == 9, "rx on endpoint %u", ep->endpoint_id);
	qmap_deaggregate(buf, len, true, rx_pkt, NULL, &qst);
}

void
ipa_os_tx_done(struct ipa *ipa, struct ipa_endpoint *ep, void *cookie)
{
	int i = (int)(uintptr_t)cookie - 1;

	CHK(i >= 0 && i < 4096 && tx_done_seen[i]++ == 0, "tx done %d twice", i);
	tx_done_n++;
}

void ipa_os_modem_up(struct ipa *ipa) { modem_up_n++; }
void ipa_os_modem_down(struct ipa *ipa) { modem_down_n++; }
static int modem_gone_n;
void ipa_os_modem_gone(struct ipa *ipa) { modem_gone_n++; }
bool ipa_os_powered(struct ipa *ipa) { return (true); }
void ipa_os_schedule(struct ipa *ipa, enum ipa_work w) { }

/* the QMI side is test_qmi's */
int ipa_qmi_setup(struct ipa *ipa) { return (0); }
void ipa_qmi_teardown(struct ipa *ipa) { }

/* --- checks --------------------------------------------------------------------------- */

#define	R(off)		ipa_regs[(off) / 4]
#define	EP(reg, ep)	R(IPA_ENDP(reg, ep))

static void
check_config(void)
{
	/* ipa_hardware_config() for v4.2 */
	CHK(R(IPA_BCR) == 0, "BCR");
	CHK(R(IPA_CLKON_CFG) == 0x30000000, "CLKON_CFG %#x", R(IPA_CLKON_CFG));
	CHK(R(IPA_COMP_CFG) == (IPA_BIT(15) | IPA_BIT(7) | IPA_BIT(8)), "COMP_CFG");
	CHK(R(IPA_QSB_MAX_WRITES) == 8 && R(IPA_QSB_MAX_READS) == 12, "QSB");
	CHK(R(IPA_COUNTER_CFG) == 15 << 4, "COUNTER_CFG %#x", R(IPA_COUNTER_CFG));
	CHK(R(IPA_FILT_ROUT_HASH_EN) == 0, "hashing");
	CHK(R(IPA_IDLE_INDICATION_CFG) == 256, "IDLE_INDICATION_CFG");
	/* the uC's interrupts on (ipa_uc_config) */
	CHK(R(IPA_IRQ_EN) == (IPA_BIT(2) | IPA_BIT(3)), "IRQ_EN %#x", R(IPA_IRQ_EN));
	/* ipa_resource_config(): X min | X max << 8 */
	CHK(R(IPA_SRC_RSRC_GRP_01_RSRC_TYPE(0)) == 0x3f03 &&
	    R(IPA_SRC_RSRC_GRP_01_RSRC_TYPE(1)) == 0x0303 &&
	    R(IPA_SRC_RSRC_GRP_01_RSRC_TYPE(2)) == 0x0a0a &&
	    R(IPA_SRC_RSRC_GRP_01_RSRC_TYPE(3)) == 0x0101 &&
	    R(IPA_SRC_RSRC_GRP_01_RSRC_TYPE(4)) == 0x0505, "source resources");
	CHK(R(IPA_DST_RSRC_GRP_01_RSRC_TYPE(0)) == 0x0303 &&
	    R(IPA_DST_RSRC_GRP_01_RSRC_TYPE(1)) == 0x3f01, "destination resources");
	/* canaries before the regions that have them */
	CHK(shared[(0x0290 - 4) / 4] == 0xdeadbeef &&
	    shared[(0x0290 - 8) / 4] == 0xdeadbeef &&
	    shared[(0x0bf0 - 4) / 4] == 0 &&
	    shared[(0x2000 - 4) / 4] == 0xdeadbeef, "canaries");
	printf("gsi: ipa_config's registers and canaries: ok\n");
}

static uint32_t
cmd_count(uint16_t opcode)
{
	int i, n = 0;

	for (i = 0; i < ncmds && i < 512; i++)
		n += cmds[i].opcode == opcode;
	return (n);
}

static void
check_setup(struct ipa *ipa)
{
	const struct ipa_cmd_hw_ip_fltrt_init *ti;
	const struct ipa_cmd_hw_hdr_init_local *hl;
	const struct ipa_cmd_hw_dma_mem_mem *dm;
	const uint64_t *tab = ipa->table.va;
	uint64_t f;
	int i;

	/* GSI: channels 0-3 with event rings 2, 0, 1, 3 (Linux's order) */
	CHK(G(GSI_CH_C_CNTXT_0(1)) == 0x1000000a && G(GSI_CH_C_CNTXT_0(2)) == 0x10004002 &&
	    G(GSI_CH_C_CNTXT_0(0)) == 0x1000800a && G(GSI_CH_C_CNTXT_0(3)) == 0x1000c002,
	    "CH_C_CNTXT_0");
	CHK(G(GSI_CH_C_CNTXT_1(0)) == 8192 && G(GSI_CH_C_CNTXT_1(1)) == 4096 &&
	    G(GSI_CH_C_CNTXT_1(2)) == 4096 && G(GSI_CH_C_CNTXT_1(3)) == 4096,
	    "CH_C_CNTXT_1");
	CHK(G(GSI_CH_C_QOS(1)) == 0xf && G(GSI_CH_C_QOS(0)) == 0x400 &&
	    G(GSI_CH_C_QOS(2)) == 0x400 && G(GSI_CH_C_QOS(3)) == 0x400, "QOS");
	CHK(G(GSI_CH_C_SCRATCH_2(1)) == 320U << 16 && G(GSI_CH_C_SCRATCH_2(0)) ==
	    128U << 16 && G(GSI_CH_C_SCRATCH_2(2)) == 96U << 16, "SCRATCH_2");
	CHK(G(GSI_CH_C_SCRATCH_3(0)) == 32U << 16, "SCRATCH_3");
	for (i = 0; i < 4; i++) {
		CHK(G(GSI_EV_CH_E_CNTXT_0(i)) == 0x10010002, "EV_CH_E_CNTXT_0");
		CHK(G(GSI_EV_CH_E_CNTXT_8(i)) == 0x00010020, "EV_CH_E_CNTXT_8");
		CHK(ev_state[i] == GSI_EVT_RING_STATE_ALLOCATED, "event ring state");
	}
	CHK(G(GSI_EV_CH_E_CNTXT_1(2)) == 8192, "event ring 2 length");
	/* the modem's channels 0-3, allocated by the AP */
	for (i = 0; i < 4; i++)
		CHK(generic_ops[i][GSI_GENERIC_ALLOCATE_CHANNEL] == 1,
		    "modem channel %d not allocated", i);
	CHK(G(GSI_CNTXT_INTSET) == 1, "INTSET");
	CHK(G(GSI_CNTXT_GSI_IRQ_EN) == 0xe && G(GSI_CNTXT_GLOB_IRQ_EN) == 1,
	    "GSI and global interrupts");

	/* the AP endpoints (ipa_endpoint_program) */
	CHK(EP(IPA_ENDP_INIT_CFG, 1) == 0xa && EP(IPA_ENDP_INIT_CFG, 9) == 0x4 &&
	    EP(IPA_ENDP_INIT_CFG, 6) == 0 && EP(IPA_ENDP_INIT_CFG, 8) == 0, "INIT_CFG");
	CHK(EP(IPA_ENDP_INIT_HDR, 9) == 0x002800c4 && EP(IPA_ENDP_INIT_HDR, 1) == 0x48 &&
	    EP(IPA_ENDP_INIT_HDR, 8) == 0 && EP(IPA_ENDP_INIT_HDR, 6) == 0,
	    "INIT_HDR %#x %#x", EP(IPA_ENDP_INIT_HDR, 9), EP(IPA_ENDP_INIT_HDR, 1));
	CHK(EP(IPA_ENDP_INIT_HDR_EXT, 9) == 0xb && EP(IPA_ENDP_INIT_HDR_EXT, 8) == 0x800 &&
	    EP(IPA_ENDP_INIT_HDR_EXT, 1) == 0x1 && EP(IPA_ENDP_INIT_HDR_EXT, 6) == 0,
	    "INIT_HDR_EXT");
	CHK(EP(IPA_ENDP_INIT_HDR_METADATA_MASK, 9) == 0xff000000 &&
	    EP(IPA_ENDP_INIT_HDR_METADATA_MASK, 8) == 0, "METADATA_MASK");
	CHK(EP(IPA_ENDP_INIT_MODE, 6) == 0x83 && EP(IPA_ENDP_INIT_MODE, 1) == 0, "MODE");
	CHK(EP(IPA_ENDP_INIT_AGGR, 8) == 0x4d1 && EP(IPA_ENDP_INIT_AGGR, 9) == 0x2004d1 &&
	    EP(IPA_ENDP_INIT_AGGR, 1) == 0 && EP(IPA_ENDP_INIT_AGGR, 6) == 0,
	    "INIT_AGGR %#x %#x", EP(IPA_ENDP_INIT_AGGR, 8), EP(IPA_ENDP_INIT_AGGR, 9));
	CHK(EP(IPA_ENDP_INIT_SEQ, 1) == 0x806 && EP(IPA_ENDP_INIT_SEQ, 6) == 0, "SEQ");
	CHK(EP(IPA_ENDP_STATUS, 8) == 1 && EP(IPA_ENDP_STATUS, 1) == 0x15 &&
	    EP(IPA_ENDP_STATUS, 9) == 0 && EP(IPA_ENDP_STATUS, 6) == 0, "STATUS");
	CHK(R(IPA_ROUTE) == 0x01100050, "ROUTE %#x", R(IPA_ROUTE));
	CHK(R(IPA_LOCAL_PKT_PROC_CNTXT) == 0x5f0, "LOCAL_PKT_PROC_CNTXT");

	/* the commands: memory (4) then tables (6), in Linux's order */
	CHK(ncmds == 10, "%d immediate commands", ncmds);
	CHK(cmds[0].opcode == IPA_CMD_HDR_INIT_LOCAL, "hdr init");
	hl = (const void *)cmds[0].payload;
	CHK(le64toh(hl->hdr_table_addr) == ipa->zero.pa &&
	    le32toh(hl->flags) == (0x140 | 0x4a8U << 12), "hdr init payload");
	{
		static const uint16_t z[3][2] = { { 0x5f0, 0x200 }, { 0x7f0, 0x200 },
		    { 0xbf0, 0x140c } };
		for (i = 0; i < 3; i++) {
			dm = (const void *)cmds[1 + i].payload;
			CHK(cmds[1 + i].opcode == IPA_CMD_DMA_SHARED_MEM &&
			    le16toh(dm->local_addr) == z[i][0] &&
			    le16toh(dm->size) == z[i][1] && le16toh(dm->flags) == 0 &&
			    le64toh(dm->system_addr) == ipa->zero.pa, "zeroing %d", i);
		}
	}
	CHK(cmds[4].opcode == IPA_CMD_IP_V4_ROUTING_INIT &&
	    cmds[5].opcode == IPA_CMD_IP_V6_ROUTING_INIT &&
	    cmds[6].opcode == IPA_CMD_IP_V4_FILTER_INIT &&
	    cmds[7].opcode == IPA_CMD_DMA_SHARED_MEM &&
	    cmds[8].opcode == IPA_CMD_IP_V6_FILTER_INIT &&
	    cmds[9].opcode == IPA_CMD_DMA_SHARED_MEM, "table commands");
	ti = (const void *)cmds[4].payload;
	f = (uint64_t)0x3a0 << 40 | (uint64_t)120 << 28;
	CHK(le64toh(ti->flags) == f && le64toh(ti->nhash_rules_addr) ==
	    ipa->table.pa + 16 && ti->hash_rules_addr == 0, "v4 route init");
	ti = (const void *)cmds[5].payload;
	CHK(le64toh(ti->flags) == ((uint64_t)0x428 << 40 | (uint64_t)120 << 28),
	    "v6 route init");
	ti = (const void *)cmds[6].payload;
	CHK(le64toh(ti->flags) == ((uint64_t)0x290 << 40 | (uint64_t)24 << 28) &&
	    le64toh(ti->nhash_rules_addr) == ipa->table.pa + 8, "v4 filter init");
	dm = (const void *)cmds[7].payload;
	CHK(le16toh(dm->local_addr) == 0x2a8 && le16toh(dm->size) == 0x60,
	    "v4 filter zeroing");
	dm = (const void *)cmds[9].payload;
	CHK(le16toh(dm->local_addr) == 0x330 && le16toh(dm->size) == 0x60,
	    "v6 filter zeroing");
	/* the table memory: zero rule, bitmap (endpoints 1 and 4) << 1, addresses */
	CHK(tab[0] == 0 && tab[1] == 0x24, "table head");
	for (i = 2; i < 17; i++)
		CHK(tab[i] == ipa->table.pa, "table entry %d", i);
	/* the buffers of the exception endpoint posted */
	CHK(ch_wp[2] == ipa->name_map[IPA_EP_AP_LAN_RX]->rxbuf_count, "LAN RX "
	    "buffers %u", ch_wp[2]);
	printf("gsi: ipa_setup's GSI programming, endpoints, commands, tables: ok\n");
}

/* an aggregated buffer of three UDP/IPv4 packets for mux id 1 */
static size_t
rx_buffer(uint8_t *b, int seq)
{
	size_t n = 0;
	int k, i;

	for (k = 0; k < 3; k++) {
		uint8_t ip[128];
		uint32_t s = 0;
		size_t len = 28 + 16 + (seq + k) % 5, pad;

		memset(ip, 0, sizeof(ip));
		ip[0] = 0x45;
		ip[2] = len >> 8;
		ip[3] = len;
		ip[8] = 64;
		ip[9] = 17;
		ip[12] = 10; ip[15] = 2; ip[16] = 1; ip[19] = 1;
		for (i = 0; i < 20; i += 2)
			s += ip[i] << 8 | ip[i + 1];
		while (s >> 16)
			s = (s & 0xffff) + (s >> 16);
		ip[10] = ~s >> 8;
		ip[11] = ~s;
		ip[24] = (len - 20) >> 8;	/* UDP length; checksum 0 */
		ip[25] = len - 20;
		pad = (4 - len % 4) % 4;
		b[n] = pad;
		b[n + 1] = 1;
		b[n + 2] = (len + pad) >> 8;
		b[n + 3] = len + pad;
		memcpy(b + n + 4, ip, len);
		memset(b + n + 4 + len, 0, pad + 8);
		b[n + 4 + len + pad + 1] = 1;		/* valid trailer */
		n += 4 + len + pad + 8;
	}
	return (n);
}

static void
wait_for(atomic_int *v, int want, const char *what)
{
	int i;

	for (i = 0; i < 4000 && *v < want; i++)
		usleep(500);
	CHK(*v >= want, "%s: %d of %d", what, *v, want);
}

static void
test_data(struct ipa *ipa)
{
	static uint8_t seg[3][4096];
	uint8_t buf[1024];
	struct gsi_seg gs[3];
	size_t n;
	int i, k, nseg, tries;

	CHK(ipa_modem_start(ipa) == 0 && modem_up_n == 1, "modem start");
	CHK(ch_state[0] == GSI_CHANNEL_STATE_STARTED &&
	    ch_state[3] == GSI_CHANNEL_STATE_STARTED, "modem endpoints started");
	CHK(ch_wp[3] == ipa->name_map[IPA_EP_AP_MODEM_RX]->rxbuf_count, "posted");
	/* downlink: 1000 buffers through 128 posted buffers, a 256-TRE ring */
	for (i = 0; i < 1000; i++) {
		n = rx_buffer(buf, i);
		for (tries = 0; !model_rx(3, buf, n); tries++) {
			CHK(tries < 4000, "no receive buffer after 2 s (%d)", i);
			usleep(500);
		}
	}
	wait_for(&rx_pkts, 3000, "packets received");
	CHK(rx_bad == 0 && qst.csum_skipped == 3000, "received packets bad");
	printf("gsi: 1000 aggregated downlink buffers, 3000 packets: ok\n");

	/* uplink: 3000 frames of 1 to 3 segments (the ring has 512 TREs) */
	for (i = 0; i < 3000; i++) {
		nseg = 1 + i % 3;
		for (k = 0; k < nseg; k++) {
			memset(seg[k], (i + k) & 0xff, 100);
			gs[k].pa = (uintptr_t)seg[k];
			gs[k].len = 100 + k;
			gs[k].opcode = IPA_CMD_NONE;
		}
		for (tries = 0; ipa_modem_tx(ipa, gs, nseg, (void *)(uintptr_t)
		    (1 + i % 4096), false) == EBUSY; tries++) {
			CHK(tries < 4000, "uplink ring full after 2 s (%d)", i);
			usleep(500);
		}
		/* the done flags are per cookie: wait every 4096 */
		if (i % 4096 == 4095)
			wait_for(&tx_done_n, i + 1, "uplink done");
	}
	wait_for(&tx_done_n, 3000, "uplink done");
	CHK(txf_total == 3000, "%ju frames", (uintmax_t)txf_total);
	printf("gsi: 3000 uplink frames of 1-3 segments, each completed once: ok\n");
}

static void
check_crash(struct ipa *ipa)
{
	int before = ncmds, i;

	ipa_modem_crashed(ipa);
	CHK(modem_down_n == 1 && ipa->modem_state == IPA_MODEM_STATE_STOPPED &&
	    modem_gone_n == 1, "modem stop, the SMP2P answer reset");
	/* flow control on, then off, for the modem's TX channels (0 and 1) */
	for (i = 0; i <= 1; i++)
		CHK(generic_ops[i][GSI_GENERIC_ENABLE_FLOW_CONTROL] == 1 &&
		    generic_ops[i][GSI_GENERIC_DISABLE_FLOW_CONTROL] == 1,
		    "modem channel %d flow control", i);
	/* the modem's RX endpoints (10, 11): HOLB on with a 0 timer */
	CHK(EP(IPA_ENDP_INIT_HOL_BLOCK_EN, 10) == 1 &&
	    EP(IPA_ENDP_INIT_HOL_BLOCK_EN, 11) == 1, "HOLB");
	/* the exception reset: ENDP_STATUS of endpoints 4 and 5 written 0 */
	CHK(EP(IPA_ENDP_STATUS, 4) == 0 && EP(IPA_ENDP_STATUS, 5) == 0, "status");
	CHK(cmd_count(IPA_CMD_IP_PACKET_TAG_STATUS) == 1 &&
	    cmd_count(IPA_CMD_IP_PACKET_INIT) == 1, "pipeline clear");
	/* table reset (2 filter, 2 route), exception reset (2 status writes,
	 * 3 commands of the pipeline clear and its transfer), zeroing (3) */
	CHK(ncmds - before == 2 + 2 + 5 + 3, "%d crash commands", ncmds - before);
	printf("gsi: the crash path (flow control, HOLB, tables, exception "
	    "reset with its pipeline clear, zeroing): ok\n");
}

int
main(int argc, char **argv)
{
	struct ipa *ipa = &IPA;
	pthread_t it, pt;
	int i;

	verbose = argc > 1;
	ipa->mem_size = 0x2000;
	ipa->rx_buffers_max = 128;
	CHK(ipa_init(ipa) == 0, "ipa_init");
	pthread_create(&it, NULL, irq_thread, ipa);
	pthread_create(&pt, NULL, poll_thread, ipa);
	CHK(ipa_config(ipa) == 0, "ipa_config");
	check_config();
	CHK(ipa_setup(ipa) == 0, "ipa_setup");
	check_setup(ipa);
	test_data(ipa);
	check_crash(ipa);
	/* and up again (a modem restart), then down for good */
	CHK(ipa_modem_start(ipa) == 0 && modem_up_n == 2, "modem start again");
	ipa_teardown(ipa);
	for (i = 0; i < 4; i++) {
		CHK(ch_state[i] == GSI_CHANNEL_STATE_NOT_ALLOCATED, "channel %d "
		    "state %u after teardown", i, ch_state[i]);
		CHK(ev_state[i] == GSI_EVT_RING_STATE_NOT_ALLOCATED, "event ring");
		CHK(generic_ops[i][GSI_GENERIC_HALT_CHANNEL] == 1, "halt %d", i);
	}
	ipa_deconfig(ipa);
	irq_thread_run = false;
	pthread_join(it, NULL);
	pthread_mutex_lock(&plock);
	poll_run = false;
	pthread_cond_broadcast(&pcond);
	pthread_mutex_unlock(&plock);
	pthread_join(pt, NULL);
	ipa_exit(ipa);
	printf("gsi: teardown (channels and event rings deallocated, the "
	    "modem's channels halted): ok; %ju command timeouts\n",
	    (uintmax_t)ipa->n_cmd_timeouts);
	return (0);
}
