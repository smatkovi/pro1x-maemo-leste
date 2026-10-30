/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Host test of pro1x-audiod (freebsd/audio/README.md, "Tests on the PC"):
 *
 * 1. its packets byte for byte against Linux's packed structures, filled
 *    as q6asm.c, q6adm.c and q6voice (q6mvm.c, q6cvp.c) fill them;
 * 2. the PCM path against a simulated ADSP (ASM, ADM, AFE, core) and a
 *    simulated snd_pro1x.ko: the order of every command of a start, the
 *    blocks in flight never beyond what sound(4) has filled, their
 *    addresses, the stop, the back end's idle power-down, capture, a
 *    refused COPP (everything undone, the kernel told), a stream that
 *    starts again;
 * 3. the sequences: the parser (every line kind, errors with their line),
 *    the runner against simulated register windows, SoundWire, I2C,
 *    GPIOs, rails, clocks and votes, $vol_db;
 * 4. the SoundWire master against a simulated one with a WCD9370 behind
 *    it: init, the enumeration's id, command packing, reads, a stream's
 *    port registers and the bank switch's broadcast;
 * 5. the voice session's six commands up, five down, the handles from
 *    the answers' source ports, a refused CVP undone.
 */

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/pro1x-audiod/audiod.h"

static int failures;
#define	CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, \
	__LINE__, #c); failures++; } } while (0)

/* --- Linux's structures (q6asm.c, q6adm.c, q6voice), packed ---------------- */

struct apr_hdr {
	uint16_t hdr_field, pkt_size;
	uint8_t src_svc, src_domain;
	uint16_t src_port;
	uint8_t dest_svc, dest_domain;
	uint16_t dest_port;
	uint32_t token, opcode;
} __attribute__((packed));

struct avs_cmd_shared_mem_map_regions {
	uint16_t mem_pool_id, num_regions;
	uint32_t property_flag;
} __attribute__((packed));
struct avs_shared_map_region_payload {
	uint32_t shm_addr_lsw, shm_addr_msw, mem_size_bytes;
} __attribute__((packed));
struct asm_stream_cmd_open_write_v3 {
	uint32_t mode_flags;
	uint16_t sink_endpointype, bits_per_sample;
	uint32_t postprocopo_id, dec_fmt_id;
} __attribute__((packed));
struct asm_stream_cmd_open_read_v3 {
	uint32_t mode_flags, src_endpointype, preprocopo_id, enc_cfg_id;
	uint16_t bits_per_sample, reserved;
} __attribute__((packed));
struct asm_multi_channel_pcm_fmt_blk_v2 {
	uint32_t fmt_blk_size;
	uint16_t num_channels, bits_per_sample;
	uint32_t sample_rate;
	uint16_t is_signed, reserved;
	uint8_t channel_mapping[8];
} __attribute__((packed));
struct asm_multi_channel_pcm_enc_cfg_v2 {
	uint32_t param_id, param_size, frames_per_buf, enc_cfg_blk_size;
	uint16_t num_channels, bits_per_sample;
	uint32_t sample_rate;
	uint16_t is_signed, reserved;
	uint8_t channel_mapping[8];
} __attribute__((packed));
struct asm_data_cmd_write_v2 {
	uint32_t buf_addr_lsw, buf_addr_msw, mem_map_handle, buf_size, seq_id,
	    timestamp_lsw, timestamp_msw, flags;
} __attribute__((packed));
struct asm_data_cmd_read_v2 {
	uint32_t buf_addr_lsw, buf_addr_msw, mem_map_handle, buf_size, seq_id;
} __attribute__((packed));
struct q6adm_cmd_device_open_v5 {
	uint16_t flags, mode_of_operation, endpoint_id_1, endpoint_id_2;
	uint32_t topology_id;
	uint16_t dev_num_channel, bit_width;
	uint32_t sample_rate;
	uint8_t dev_channel_mapping[8];
} __attribute__((packed));
struct q6adm_cmd_matrix_map_routings_v5 {
	uint32_t matrix_id, num_sessions;
} __attribute__((packed));
struct q6adm_session_map_node_v5 {
	uint16_t session_id, num_copps;
} __attribute__((packed));
struct cvp_create_v2 {	/* vss_ivocproc_cmd_create_full_control_session_v2 */
	uint16_t direction, tx_port_id;
	uint32_t tx_topology_id;
	uint16_t rx_port_id;
	uint32_t rx_topology_id, profile_id, vocproc_mode;
	uint16_t ec_ref_port_id;
	char name[20];
} __attribute__((packed));

/* apr_send_pkt's completion: service both ends, apps -> ADSP */
static void
lhdr(struct apr_hdr *h, uint8_t svc, uint16_t sp, uint16_t dp, uint32_t tok,
    uint32_t op, uint16_t size)
{
	h->hdr_field = 2 << 8 | 5 << 4;		/* APR_SEQ_CMD_HDR_FIELD */
	h->pkt_size = size;
	h->src_svc = h->dest_svc = svc;
	h->src_domain = 5;
	h->dest_domain = 4;
	h->src_port = sp;
	h->dest_port = dp;
	h->token = tok;
	h->opcode = op;
}

static void
test_packets(void)
{
	uint8_t p[256], l[256];
	struct apr_hdr *h = (struct apr_hdr *)l;
	size_t n;
	struct voice v = { .rx_port = 0xb030, .tx_port = 0xb021, .mvm = 0x11,
	    .cvp = 0x22 };

	/* __q6asm_memory_map_regions, session 1, playback (dir 0) */
	memset(l, 0, sizeof(l));
	{
		struct avs_cmd_shared_mem_map_regions *c = (void *)(l + 20);
		struct avs_shared_map_region_payload *r = (void *)(l + 28);

		lhdr(h, 7, 0, 0, 1 << 8 | 0, 0x10d92, 40);
		c->mem_pool_id = 3;
		c->num_regions = 1;
		r->shm_addr_lsw = 0x9a3f0000;
		r->shm_addr_msw = 1;
		r->mem_size_bytes = 61440;
	}
	n = asm_pkt_map(p, 1, 0, 0x19a3f0000ull, 61440);
	CHECK(n == 40 && memcmp(p, l, n) == 0);
	/* ALIGN(buf_sz, 4096) */
	n = asm_pkt_map(p, 2, 1, 0x1000, 30720);
	CHECK(le32dec_(p + 36) == 32768 && le32dec_(p + 12) == (2 << 8 | 1));

	/* q6asm_open_write: session 1, stream 1 */
	memset(l, 0, sizeof(l));
	{
		struct asm_stream_cmd_open_write_v3 *o = (void *)(l + 20);

		lhdr(h, 7, 0x101, 0x101, 1, 0x10db3, 36);
		o->sink_endpointype = 0;
		o->bits_per_sample = 16;
		o->postprocopo_id = 0x10c68;
		o->dec_fmt_id = 0x10da5;
	}
	n = asm_pkt_open_write(p, 1, 16);
	CHECK(n == 36 && memcmp(p, l, n) == 0);

	memset(l, 0, sizeof(l));
	{
		struct asm_stream_cmd_open_read_v3 *o = (void *)(l + 20);

		lhdr(h, 7, 0x201, 0x201, 2, 0x10db4, 40);
		o->src_endpointype = 0;
		o->preprocopo_id = 0x10c68;
		o->enc_cfg_id = 0x10da5;
		o->bits_per_sample = 16;
	}
	n = asm_pkt_open_read(p, 2, 16);
	CHECK(n == 40 && memcmp(p, l, n) == 0);

	memset(l, 0, sizeof(l));
	{
		struct asm_multi_channel_pcm_fmt_blk_v2 *f = (void *)(l + 20);

		lhdr(h, 7, 0x101, 0x101, 1, 0x10d98, 44);
		f->fmt_blk_size = sizeof(*f) - 4;
		f->num_channels = 2;
		f->bits_per_sample = 16;
		f->sample_rate = 48000;
		f->is_signed = 1;
		f->channel_mapping[0] = 1;
		f->channel_mapping[1] = 2;
	}
	n = asm_pkt_media_fmt(p, 1, 48000, 2, 16);
	CHECK(n == 44 && memcmp(p, l, n) == 0);

	memset(l, 0, sizeof(l));
	{
		struct asm_multi_channel_pcm_enc_cfg_v2 *e = (void *)(l + 20);

		lhdr(h, 7, 0x201, 0x201, 2, 0x10c10, 56);
		e->param_id = 0x10da3;
		e->param_size = sizeof(*e) - 8;
		e->frames_per_buf = 0;
		e->enc_cfg_blk_size = e->param_size - 8;
		e->num_channels = 1;
		e->bits_per_sample = 16;
		e->sample_rate = 48000;
		e->is_signed = 1;
		e->channel_mapping[0] = 3;		/* mono: FC */
	}
	n = asm_pkt_enc_cfg(p, 2, 48000, 1, 16);
	CHECK(n == 56 && memcmp(p, l, n) == 0);

	memset(l, 0, sizeof(l));
	lhdr(h, 7, 0x101, 0x101, 1, 0x10daa, 32);
	n = asm_pkt_run(p, 1);
	CHECK(n == 32 && memcmp(p, l, n) == 0);
	memset(l, 0, sizeof(l));
	lhdr(h, 7, 0x101, 0x101, 1, 0x10bcd, 20);
	n = asm_pkt_cmd(p, 1, ASM_STREAM_CMD_CLOSE);
	CHECK(n == 20 && memcmp(p, l, n) == 0);

	/* q6asm_write_async: token = dsp_buf | len << 16 */
	memset(l, 0, sizeof(l));
	{
		struct asm_data_cmd_write_v2 *w = (void *)(l + 20);

		lhdr(h, 7, 0x101, 0x101, 3 | 7680u << 16, 0x10dab, 52);
		w->buf_addr_lsw = 0x9a3f0000 + 3 * 7680;
		w->buf_addr_msw = 1;
		w->mem_map_handle = 0x55;
		w->buf_size = 7680;
		w->seq_id = 3;
	}
	n = asm_pkt_write(p, 1, 0x19a3f0000ull + 3 * 7680, 0x55, 7680, 3);
	CHECK(n == 52 && memcmp(p, l, n) == 0);

	memset(l, 0, sizeof(l));
	{
		struct asm_data_cmd_read_v2 *r = (void *)(l + 20);

		lhdr(h, 7, 0x201, 0x201, 5, 0x10dac, 40);
		r->buf_addr_lsw = 0x1000 + 5 * 3840;
		r->mem_map_handle = 0x66;
		r->buf_size = 3840;
		r->seq_id = 5;
	}
	n = asm_pkt_read(p, 2, 0x1000 + 5 * 3840, 0x66, 3840, 5);
	CHECK(n == 40 && memcmp(p, l, n) == 0);

	/* q6adm_device_open: RX_CODEC_DMA_RX_0, index 113, copp 0 */
	memset(l, 0, sizeof(l));
	{
		struct q6adm_cmd_device_open_v5 *o = (void *)(l + 20);

		lhdr(h, 8, 0xb030, 0xb030, 113 << 16 | 0, 0x10326, 48);
		o->flags = 0;
		o->mode_of_operation = 1;
		o->endpoint_id_1 = 0xb030;
		o->topology_id = 0x10312;
		o->dev_num_channel = 2;
		o->bit_width = 16;
		o->sample_rate = 48000;
		o->dev_channel_mapping[0] = 1;
		o->dev_channel_mapping[1] = 2;
	}
	n = adm_pkt_open(p, 0xb030, 113, 0, ADM_PATH_PLAYBACK, 48000, 2, 16);
	CHECK(n == 48 && memcmp(p, l, n) == 0);

	/* q6adm_matrix_map: pkt_size with a u32 per COPP, the ids as u16 */
	memset(l, 0, sizeof(l));
	{
		struct q6adm_cmd_matrix_map_routings_v5 *m = (void *)(l + 20);
		struct q6adm_session_map_node_v5 *nd = (void *)(l + 28);
		uint16_t *copps = (void *)(l + 32);

		lhdr(h, 8, 0, 0, 0, 0x10325, 20 + 8 + 4 + 4);
		m->matrix_id = 1;			/* TX */
		m->num_sessions = 1;
		nd->session_id = 2;
		nd->num_copps = 1;
		copps[0] = 7;
	}
	n = adm_pkt_map(p, ADM_PATH_LIVE_REC, 2, 7);
	CHECK(n == 36 && memcmp(p, l, n) == 0);

	/* q6adm_device_close from q6adm_free_copp: src the port's index */
	memset(l, 0, sizeof(l));
	lhdr(h, 8, 113, 7, 113 << 16 | 0, 0x10327, 20);
	n = adm_pkt_close(p, 0xb030, 113, 0, 7);
	CHECK(n == 20 && memcmp(p, l, n) == 0);

	/* q6voice: MVM create "11C05000", path 6, handle 0 */
	memset(l, 0, sizeof(l));
	lhdr(h, 9, 6, 0, 0, 0x110ff, 40);
	strcpy((char *)l + 20, "11C05000");
	n = voice_pkt(p, 0, &v);
	CHECK(n == 40 && memcmp(p, l, n) == 0);
	memset(l, 0, sizeof(l));
	lhdr(h, 9, 6, 0x11, 0, 0x11327, 21);
	l[20] = 1;
	n = voice_pkt(p, 1, &v);
	CHECK(n == 21 && memcmp(p, l, n) == 0);
	memset(l, 0, sizeof(l));
	{
		struct cvp_create_v2 *c = (void *)(l + 20);

		lhdr(h, 0xb, 6, 0, 0, 0x112bf, 64);
		c->direction = 2;
		c->tx_port_id = 0xb021;
		c->tx_topology_id = 0x10f70;
		c->rx_port_id = 0xb030;
		c->rx_topology_id = 0x10f77;
		c->profile_id = 0x1135e;
		c->vocproc_mode = 0x10f7c;
		c->ec_ref_port_id = 0xffff;
	}
	n = voice_pkt(p, 2, &v);
	CHECK(n == 64 && memcmp(p, l, n) == 0);
	memset(l, 0, sizeof(l));
	lhdr(h, 9, 6, 0x11, 0, 0x1123e, 22);
	l[20] = 0x22;
	n = voice_pkt(p, 4, &v);
	CHECK(n == 22 && memcmp(p, l, n) == 0);
	memset(l, 0, sizeof(l));
	lhdr(h, 0xb, 6, 0x22, 0, 0x1003c, 20);
	n = voice_pkt(p, 9, &v);
	CHECK(n == 20 && memcmp(p, l, n) == 0);
	printf("packets: done\n");
}

/* --- a simulated ADSP -------------------------------------------------------- */

#define	SIMQ	512
static struct {
	uint8_t q[SIMQ][256];
	size_t qlen[SIMQ];
	unsigned head, n;
	/* what was sent, by opcode, in order */
	uint32_t ops[4096];
	uint16_t svcs[4096];
	unsigned nops;
	/* the writes/reads the ADSP holds (block indices) */
	uint32_t pending[64], npending;
	uint32_t refuse_op;		/* answer this opcode with status 0x12 */
	uint64_t last_addr;
	uint32_t last_handle, last_len;
	unsigned writes, reads, inflight_max;
} A;

static void
sim_push(const uint8_t *p, size_t len)
{
	assert(A.n < SIMQ);
	memcpy(A.q[(A.head + A.n) % SIMQ], p, len);
	A.qlen[(A.head + A.n) % SIMQ] = len;
	A.n++;
}

/* an answer from service svc to the apps processor's port dp */
static void
sim_answer(uint8_t svc, uint16_t sp, uint16_t dp, uint32_t token, uint32_t op,
    const uint32_t *pl, unsigned nw)
{
	uint8_t p[128];
	unsigned i;
	size_t len = 20 + 4 * nw;

	le16enc_(p, 1 << 8 | 5 << 4);
	le16enc_(p + 2, (uint16_t)len);
	p[4] = svc;
	p[5] = 4;
	le16enc_(p + 6, sp);
	p[8] = svc;
	p[9] = 5;
	le16enc_(p + 10, dp);
	le32enc_(p + 12, token);
	le32enc_(p + 16, op);
	for (i = 0; i < nw; i++)
		le32enc_(p + 20 + 4 * i, pl[i]);
	sim_push(p, len);
}

static void
sim_basic(const uint8_t *cmd, uint32_t status, uint16_t sp)
{
	uint32_t pl[2] = { apr_opcode(cmd), status };

	sim_answer(cmd[8], sp, apr_src_port(cmd), apr_token(cmd),
	    APR_BASIC_RSP_RESULT, pl, 2);
}

static int
sim_send(void *ctx, const uint8_t *pkt, size_t len)
{
	uint32_t op = apr_opcode(pkt), pl[4];
	uint8_t svc = pkt[8];

	(void)ctx;
	CHECK(le16dec_(pkt + 2) == len);
	if (A.nops < 4096) {
		A.svcs[A.nops] = svc;
		A.ops[A.nops++] = op;
	}
	if (op == A.refuse_op) {
		sim_basic(pkt, 0x12, 0);
		return (0);
	}
	switch (op) {
	case AVCS_CMD_ADSP_EVENT_GET_STATE:
		pl[0] = 1;
		sim_answer(3, 0, 0, 0, AVCS_CMDRSP_ADSP_EVENT_GET_STATE, pl, 1);
		break;
	case ASM_CMD_SHARED_MEM_MAP_REGIONS:
		pl[0] = 0x55;
		sim_answer(7, 0, 0, apr_token(pkt),
		    ASM_CMDRSP_SHARED_MEM_MAP_REGIONS, pl, 1);
		break;
	case ADM_CMD_DEVICE_OPEN_V5:
		pl[0] = 0;
		pl[1] = 7;	/* copp id 7, reserved 0 */
		sim_answer(8, 0, apr_src_port(pkt), apr_token(pkt),
		    ADM_CMDRSP_DEVICE_OPEN_V5, pl, 2);
		break;
	case ASM_DATA_CMD_WRITE_V2:
	case ASM_DATA_CMD_READ_V2:
		A.pending[A.npending++] = apr_token(pkt) & 0xffff;
		if (A.npending > A.inflight_max)
			A.inflight_max = A.npending;
		A.last_addr = le32dec_(pkt + 20) |
		    (uint64_t)le32dec_(pkt + 24) << 32;
		A.last_handle = le32dec_(pkt + 28);
		A.last_len = le32dec_(pkt + 32);
		if (op == ASM_DATA_CMD_WRITE_V2)
			A.writes++;
		else
			A.reads++;
		break;
	case 0x110ff:	/* MVM create: the handle as source port */
		sim_basic(pkt, 0, 0x11);
		break;
	case 0x112bf:	/* CVP create */
		sim_basic(pkt, 0, 0x22);
		break;
	case AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST:
		pl[0] = 9;
		sim_answer(4, 0, 0, apr_token(pkt),
		    AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST, pl, 1);
		break;
	case ASM_STREAM_CMD_CLOSE:
		A.npending = 0;		/* the buffers come back */
		sim_basic(pkt, 0, apr_dest_port(pkt));
		break;
	default:
		sim_basic(pkt, 0, apr_dest_port(pkt));
	}
	return (0);
}

static int
sim_recv(void *ctx, uint8_t *buf, size_t size, int timeout_ms)
{
	size_t len;

	(void)ctx, (void)timeout_ms;
	if (A.n == 0)
		return (0);
	len = A.qlen[A.head];
	memcpy(buf, A.q[A.head], len < size ? len : size);
	A.head = (A.head + 1) % SIMQ;
	A.n--;
	return ((int)len);
}

static struct apr_io sim_io = { sim_send, sim_recv, NULL };

/* the ADSP finishes the oldest block of session s */
static void
sim_done(int session, bool write)
{
	uint32_t pl[4], idx;

	CHECK(A.npending > 0);
	if (A.npending == 0)
		return;
	idx = A.pending[0];
	memmove(A.pending, A.pending + 1, --A.npending * sizeof(A.pending[0]));
	if (write) {
		pl[0] = 0; pl[1] = 0; pl[2] = 0x55; pl[3] = 0;
		sim_answer(7, (uint16_t)(session << 8 | 1),
		    (uint16_t)(session << 8 | 1), idx,
		    ASM_DATA_EVENT_WRITE_DONE_V2, pl, 4);
	} else {
		pl[0] = 0; pl[1] = 0; pl[2] = 0; pl[3] = 0x55;
		sim_answer(7, (uint16_t)(session << 8 | 1),
		    (uint16_t)(session << 8 | 1), idx,
		    ASM_DATA_EVENT_READ_DONE_V2, pl, 4);
	}
}

static unsigned
count_op(uint32_t op, unsigned from)
{
	unsigned i, n = 0;

	for (i = from; i < A.nops; i++)
		n += A.ops[i] == op;
	return (n);
}

static int
index_op(uint32_t op, unsigned from)
{
	unsigned i;

	for (i = from; i < A.nops; i++)
		if (A.ops[i] == op)
			return ((int)i);
	return (-1);
}

/* --- a simulated snd_pro1x.ko -------------------------------------------- */

static struct {
	uint32_t hw[2], gen[2], ready_fix, blkcnt;
	bool running[2];
	unsigned advances, fails;
} K;

static int
k_advance(void *ctx, struct pro1x_pcm_advance *a)
{
	(void)ctx;
	K.advances++;
	a->running = K.running[a->chan] && K.gen[a->chan] == a->gen;
	if (a->running)
		K.hw[a->chan] += a->blocks;
	a->hw = K.hw[a->chan];
	/* sound(4) keeps ready_fix blocks filled from the hw position */
	a->ready = a->chan == PRO1X_PCM_PLAY ? K.ready_fix : K.blkcnt;
	return (0);
}

static int
k_fail(void *ctx, struct pro1x_pcm_advance *a)
{
	(void)ctx, (void)a;
	K.fails++;
	return (0);
}

static struct pcm_io k_io = { NULL, k_advance, k_fail, NULL };

/* fma_* for the test: quiet unless TEST_VERBOSE */
static void
pump(struct audiod *d)
{
	while (A.n > 0)
		apr_poll(&d->q, 0);
}

static void
on_async(struct q6 *q, const uint8_t *pkt, size_t len)
{
	struct audiod *d = q->priv;

	if (apr_src_svc(pkt) == APR_SVC_ASM)
		pcm_asm_event(d, pkt, len);
}

static void
setup(struct audiod *d)
{
	memset(d, 0, sizeof(*d));
	memset(&A, 0, sizeof(A));
	memset(&K, 0, sizeof(K));
	backend_init(d);
	d->q.io = &sim_io;
	d->q.timeout_ms = 50;
	d->q.async = on_async;
	d->q.priv = d;
	d->pcm = &k_io;
	d->idle_ms = 0;		/* down at the next tick */
}

static struct pro1x_pcm_event
ev_start(int chan, uint32_t gen, uint32_t blksz, uint32_t blkcnt,
    uint32_t ready, unsigned channels)
{
	struct pro1x_pcm_event e;

	memset(&e, 0, sizeof(e));
	e.type = PRO1X_PCM_EV_START;
	e.chan = (uint32_t)chan;
	e.gen = gen;
	e.rate = 48000;
	e.channels = channels;
	e.bits = 16;
	e.blksz = blksz;
	e.blkcnt = blkcnt;
	e.ready = ready;
	e.paddr = 0x9a3f0000;
	e.dspaddr = 0x19a3f0000ull;
	e.bufsize = 65536;
	return (e);
}

static void
test_pcm(void)
{
	static struct audiod d;
	struct pro1x_pcm_event e;
	int i_cfg, i_start, i_map, i_open, i_adm, i_mtx, i_fmt, i_run, i_w;
	unsigned i, mark;

	/* playback: 8 blocks of 7680 bytes, sound(4) keeps 3 filled */
	setup(&d);
	K.ready_fix = 3;
	K.blkcnt = 8;
	K.running[0] = true;
	K.gen[0] = 1;
	e = ev_start(PRO1X_PCM_PLAY, 1, 7680, 8, 3, 2);
	pcm_event(&d, &e);
	CHECK(d.st[0].state == ST_RUNNING);
	i_cfg = index_op(AFE_PORT_CMD_SET_PARAM_V2, 0);
	i_start = index_op(AFE_PORT_CMD_DEVICE_START, 0);
	i_map = index_op(ASM_CMD_SHARED_MEM_MAP_REGIONS, 0);
	i_open = index_op(ASM_STREAM_CMD_OPEN_WRITE_V3, 0);
	i_adm = index_op(ADM_CMD_DEVICE_OPEN_V5, 0);
	i_mtx = index_op(ADM_CMD_MATRIX_MAP_ROUTINGS_V5, 0);
	i_fmt = index_op(ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2, 0);
	i_w = index_op(ASM_DATA_CMD_WRITE_V2, 0);
	i_run = index_op(ASM_SESSION_CMD_RUN_V2, 0);
	/* core first; the back end's port before the session, Linux's order */
	CHECK(A.ops[0] == AVCS_CMD_ADSP_EVENT_GET_STATE);
	CHECK(i_cfg > 0 && i_cfg < i_start && i_start < i_map);
	CHECK(i_map < i_open && i_open < i_adm && i_adm < i_mtx &&
	    i_mtx < i_fmt && i_fmt < i_w && i_w < i_run);
	CHECK(A.writes == 3 && A.npending == 3);
	CHECK(d.st[0].copp == 7 && d.st[0].maphandle == 0x55);
	CHECK(A.last_addr == 0x19a3f0000ull + 2 * 7680 &&
	    A.last_handle == 0x55 && A.last_len == 7680);
	/* 40 blocks: never more in flight than filled, round robin */
	for (i = 0; i < 40; i++) {
		sim_done(1, true);
		pump(&d);
	}
	CHECK(K.hw[0] == 40);
	CHECK(A.inflight_max <= 3);
	CHECK(A.writes == 43);
	CHECK(A.last_addr == 0x19a3f0000ull + (42 % 8) * 7680);
	/* sound(4) fills more: up to 8 in flight */
	K.ready_fix = 8;
	sim_done(1, true);
	pump(&d);
	CHECK(A.npending == 8);
	/* stop: CLOSE, unmap, the COPP closed; the back end later */
	mark = A.nops;
	K.running[0] = false;
	memset(&e, 0, sizeof(e));
	e.type = PRO1X_PCM_EV_STOP;
	e.chan = 0;
	e.gen = 1;
	pcm_event(&d, &e);
	CHECK(d.st[0].state == ST_IDLE);
	CHECK(index_op(ASM_STREAM_CMD_CLOSE, mark) >= 0 &&
	    index_op(ASM_STREAM_CMD_CLOSE, mark) <
	    index_op(ASM_CMD_SHARED_MEM_UNMAP_REGIONS, mark) &&
	    index_op(ASM_CMD_SHARED_MEM_UNMAP_REGIONS, mark) <
	    index_op(ADM_CMD_DEVICE_CLOSE_V5, mark));
	CHECK(count_op(AFE_PORT_CMD_DEVICE_STOP, mark) == 0);
	CHECK(d.be[BE_RX].up && d.be[BE_RX].users == 0);
	backend_tick(&d);
	CHECK(!d.be[BE_RX].up && count_op(AFE_PORT_CMD_DEVICE_STOP, mark) == 1);
	/* a done event after the stop changes nothing */
	CHECK(K.fails == 0);

	/* again: a second start with a new generation */
	mark = A.nops;
	K.running[0] = true;
	K.gen[0] = 2;
	K.hw[0] = 0;
	K.ready_fix = 2;
	e = ev_start(PRO1X_PCM_PLAY, 2, 7680, 8, 2, 2);
	pcm_event(&d, &e);
	CHECK(d.st[0].state == ST_RUNNING && count_op(ASM_DATA_CMD_WRITE_V2,
	    mark) == 2 && count_op(AFE_PORT_CMD_DEVICE_START, mark) == 1);
	/* no block ready at all: one sent anyway, the stream goes on */
	K.ready_fix = 0;
	sim_done(1, true);
	sim_done(1, true);
	pump(&d);
	CHECK(A.npending >= 1);
	pcm_stop_all(&d);

	/* capture: mono, every block queued for reading, then RUN */
	setup(&d);
	K.blkcnt = 8;
	K.running[1] = true;
	K.gen[1] = 1;
	e = ev_start(PRO1X_PCM_REC, 1, 3840, 8, 8, 1);
	pcm_event(&d, &e);
	CHECK(d.st[1].state == ST_RUNNING);
	CHECK(index_op(ASM_STREAM_CMD_OPEN_READ_V3, 0) >= 0);
	CHECK(index_op(ASM_STREAM_CMD_SET_ENCDEC_PARAM, 0) <
	    index_op(ASM_DATA_CMD_READ_V2, 0));
	CHECK(A.reads == 8 && index_op(ASM_DATA_CMD_READ_V2, 0) <
	    index_op(ASM_SESSION_CMD_RUN_V2, 0));
	CHECK(d.be[BE_VA].up && !d.be[BE_RX].up);
	for (i = 0; i < 20; i++) {
		sim_done(2, false);
		pump(&d);
	}
	CHECK(K.hw[1] == 20 && A.reads == 28 && A.npending == 8);
	pcm_stop_all(&d);

	/* a refused COPP: undone in reverse, the kernel told */
	setup(&d);
	K.running[0] = true;
	K.gen[0] = 1;
	K.ready_fix = 4;
	K.blkcnt = 8;
	A.refuse_op = ADM_CMD_DEVICE_OPEN_V5;
	e = ev_start(PRO1X_PCM_PLAY, 1, 7680, 8, 4, 2);
	pcm_event(&d, &e);
	CHECK(d.st[0].state == ST_FAILED && K.fails == 1);
	CHECK(count_op(ASM_STREAM_CMD_CLOSE, 0) == 1 &&
	    count_op(ASM_CMD_SHARED_MEM_UNMAP_REGIONS, 0) == 1 &&
	    count_op(ADM_CMD_MATRIX_MAP_ROUTINGS_V5, 0) == 0 &&
	    count_op(ASM_DATA_CMD_WRITE_V2, 0) == 0);
	CHECK(d.be[BE_RX].users == 0);
	/* sequences without rx-up: no macro register is touched (volume) */
	setup(&d);
	{
		struct seq s;
		char e[128];
		static struct mmio trap;

		CHECK(seq_parse(&s, "[power]\nmark p\n[volume]\n"
		    "w rxmacro 0x414 $vol_db\n", "b.seq", e, sizeof(e)) == 0);
		d.seq = s;
		d.have_seq = true;
		trap.name = "trap";
		trap.rd = NULL;
		trap.wr = NULL;
		trap.base = NULL;	/* a write would crash the test */
		d.cio.win[TGT_RXMACRO] = &trap;
		CHECK(backend_get(&d, BE_RX) == 0 && d.be[BE_RX].up &&
		    !d.be[BE_RX].codec);
		backend_mixer(&d, 0, 50);
		backend_route(&d, "hph");
		backend_put(&d, BE_RX);
		backend_tick(&d);
		CHECK(!d.be[BE_RX].up);
		seq_free(&d.seq);
		d.have_seq = false;
	}
	printf("pcm: done\n");
}

/* --- the sequences ------------------------------------------------------- */

static struct {
	uint32_t regs[TGT_NMMIO][1024];
	char log[64][64];
	unsigned nlog;
	uint8_t sdw[0x4000], aw[2][256];
} C;

static uint32_t
c_rd(struct mmio *m, uint32_t off)
{
	return (C.regs[(intptr_t)m->priv][off / 4 % 1024]);
}

static void
c_wr(struct mmio *m, uint32_t off, uint32_t v)
{
	C.regs[(intptr_t)m->priv][off / 4 % 1024] = v;
}

static void
tlog(const char *fmt, ...)
{
	va_list ap;

	if (C.nlog == 64)
		return;
	va_start(ap, fmt);
	vsnprintf(C.log[C.nlog++], 64, fmt, ap);
	va_end(ap);
}

static int
c_sdw_w(void *ctx, int t, uint32_t reg, uint8_t v)
{
	(void)ctx;
	C.sdw[reg & 0x3fff] = v;
	tlog("sdw %s %#x %#x", tgt_name(t), reg, v);
	return (0);
}

static int
c_sdw_r(void *ctx, int t, uint32_t reg, uint8_t *v)
{
	(void)ctx, (void)t;
	*v = C.sdw[reg & 0x3fff];
	return (0);
}

static int
c_i2c_w(void *ctx, int t, uint8_t reg, uint8_t v)
{
	(void)ctx;
	C.aw[t == TGT_AW1][reg] = v;
	tlog("i2c %s %#x %#x", tgt_name(t), reg, v);
	return (0);
}

static int
c_i2c_r(void *ctx, int t, uint8_t reg, uint8_t *v)
{
	(void)ctx;
	*v = C.aw[t == TGT_AW1][reg];
	return (0);
}

static int c_gpio(void *ctx, unsigned pin, int v) { tlog("gpio %u %d", pin, v); return (0); }
static int c_rail(void *ctx, const char *n, bool on, uint32_t uv) { tlog("rail %s %d", n, on); return (0); }
static int c_clock(void *ctx, uint32_t id, uint32_t hz) { tlog("clock %#x %u", id, hz); return (0); }
static int c_vote(void *ctx, uint32_t b, bool on) { tlog("vote %u %d", b, on); return (0); }
static int c_afe(void *ctx, const char *w) { tlog("afe %s", w); return (0); }
static int c_swr(void *ctx, const char *w) { tlog("swr %s", w); return (0); }
static void c_delay(void *ctx, uint32_t us) { tlog("delay %u", us); }
static int
c_var(void *ctx, const char *n, uint32_t *v)
{
	(void)ctx;
	if (strcmp(n, "vol_db") != 0)
		return (-1);
	*v = 0xf4;	/* -12 dB */
	return (0);
}

static void
test_seq(void)
{
	static struct mmio win[TGT_NMMIO];
	struct codec_io io;
	struct seq s;
	char e[256];
	int t, r;
	const char *text =
	    "# a test\n"
	    "[power]\n"
	    "rail l9 on\n"
	    "rail l14 on 1800000\n"
	    "gpio 92 0\n"
	    "udelay 20\n"
	    "gpio 92 1\n"
	    "[rx-up]\n"
	    "vote LPASS_HW_DCODEC on\n"
	    "clock LPASS_CLK_ID_RX_CORE_MCLK 19200000\n"
	    "clock 0x30f 19200000   # NPL\n"
	    "u rxmacro 0x100 0x1 0x1\n"
	    "w rxmacro 0x108 0x1\n"
	    "w lpass_audiocc 0x98 0x2\n"
	    "delay 1\n"
	    "swr init swr1\n"
	    "swr enum swr1 4\n"
	    "w wcd 0x3005 0x40\n"
	    "r wcd 0x3005 0x40\n"
	    "poll rxmacro 0x108 0x1 0x1 10\n"
	    "afe start\n"
	    "[hph-up]\n"
	    "call rx-extra\n"
	    "w rxmacro 0x414 $vol_db\n"
	    "w aw0 0x01 0x0e\n"
	    "mark hph on\n"
	    "[rx-extra]\n"
	    "u wcd 0x3005 0xf0 0x80\n";

	r = seq_parse(&s, text, "t.seq", e, sizeof(e));
	CHECK(r == 0);
	if (r != 0) {
		printf("%s\n", e);
		return;
	}
	CHECK(s.nsect == 4 && seq_find(&s, "rx-up")->count == 13);
	memset(&C, 0, sizeof(C));
	memset(&io, 0, sizeof(io));
	for (t = 0; t < TGT_NMMIO; t++) {
		win[t].name = tgt_name(t);
		win[t].rd = c_rd;
		win[t].wr = c_wr;
		win[t].priv = (void *)(intptr_t)t;
		io.win[t] = &win[t];
	}
	io.sdw_write = c_sdw_w;
	io.sdw_read = c_sdw_r;
	io.i2c_write = c_i2c_w;
	io.i2c_read = c_i2c_r;
	io.gpio = c_gpio;
	io.rail = c_rail;
	io.clock = c_clock;
	io.vote = c_vote;
	io.afe = c_afe;
	io.swr = c_swr;
	io.delay_us = c_delay;
	io.var = c_var;
	CHECK(seq_run(&s, "power", &io) == 0);
	CHECK(C.nlog == 5 && strcmp(C.log[0], "rail l9 1") == 0 &&
	    strcmp(C.log[2], "gpio 92 0") == 0 &&
	    strcmp(C.log[3], "delay 20") == 0);
	C.nlog = 0;
	CHECK(seq_run(&s, "rx-up", &io) == 0);
	CHECK(strcmp(C.log[0], "vote 4 1") == 0 &&
	    strcmp(C.log[1], "clock 0x30e 19200000") == 0 &&
	    strcmp(C.log[2], "clock 0x30f 19200000") == 0);
	CHECK(C.regs[TGT_RXMACRO][0x100 / 4] == 1 &&
	    C.regs[TGT_RXMACRO][0x108 / 4] == 1 &&
	    C.regs[TGT_LPASS_AUDIOCC][0x98 / 4] == 2);
	CHECK(strcmp(C.log[3], "delay 1000") == 0 &&
	    strcmp(C.log[4], "swr init swr1") == 0 &&
	    strcmp(C.log[5], "swr enum swr1 4") == 0 &&
	    strcmp(C.log[6], "sdw wcd 0x3005 0x40") == 0 &&
	    strcmp(C.log[7], "afe start") == 0);
	C.nlog = 0;
	CHECK(seq_run(&s, "hph-up", &io) == 0);
	CHECK(C.sdw[0x3005] == 0x80);		/* u: 0x40 & ~0xf0 | 0x80 */
	CHECK(C.regs[TGT_RXMACRO][0x414 / 4] == 0xf4);
	CHECK(C.aw[0][1] == 0x0e);
	CHECK(seq_run(&s, "nothing", &io) == -ENOENT);
	/* a poll that never sees its value: one error, the rest goes on */
	seq_free(&s);
	CHECK(seq_parse(&s, "[x]\npoll rxmacro 0x0 0x1 0x1 5\nw rxmacro 0x4 7\n",
	    "p.seq", e, sizeof(e)) == 0);
	C.regs[TGT_RXMACRO][0] = 0;
	CHECK(seq_run(&s, "x", &io) == 1 && C.regs[TGT_RXMACRO][1] == 7);
	seq_free(&s);
	/* errors name their line */
	CHECK(seq_parse(&s, "[a]\nw rxmacro 0x10\n", "e.seq", e, sizeof(e)) != 0 &&
	    strstr(e, "e.seq:2:") != NULL);
	seq_free(&s);
	CHECK(seq_parse(&s, "w rxmacro 0 0\n", "e.seq", e, sizeof(e)) != 0);
	seq_free(&s);
	CHECK(seq_parse(&s, "[a]\nw nowhere 0 0\n", "e.seq", e, sizeof(e)) != 0);
	seq_free(&s);
	CHECK(seq_parse(&s, "[a]\n[a]\n", "e.seq", e, sizeof(e)) != 0);
	seq_free(&s);
	CHECK(seq_parse(&s, "[a]\nclock NOCLOCK 1\n", "e.seq", e, sizeof(e)) != 0);
	seq_free(&s);
	CHECK(seq_parse(&s, "[a]\nw aw0 0x1 0x100\n", "e.seq", e, sizeof(e)) == 0);
	CHECK(seq_run(&s, "a", &io) == 1);	/* an 8-bit register */
	seq_free(&s);
	printf("sequences: done\n");
}

/* --- a simulated SoundWire master with the WCD9370 behind it --------------- */

static struct {
	uint32_t r[0x2000 / 4 + 0x800];
	uint8_t slave[0x800];		/* the WCD's SCP/DPn registers */
	uint32_t wcmds[256];
	unsigned nwcmds;
	uint32_t rdfifo;
	bool rd_ready;
} S;

static uint32_t
s_rd(struct mmio *m, uint32_t off)
{
	(void)m;
	switch (off) {
	case 0x000: return (0x01060000);	/* v1.6.0 */
	case 0x014: return (1);			/* frame generator on */
	case 0x100: return (5 | 0 << 5 | 8 << 10 | 8 << 15);
	case 0x104: return (2);
	case 0x1090: return (1 << 2);		/* device 1 attached */
	case 0x530 + 8: return (0x01170223);	/* the WCD9370's TX id */
	case 0x534 + 8: return (0x0000000a);
	case 0x30c: return (S.rd_ready ? 1 << 16 : 0);
	case 0x318: S.rd_ready = false; return (S.rdfifo);
	case 0x200: return (1 << 10);		/* broadcasts finish at once */
	}
	return (S.r[off / 4]);
}

static void
s_wr(struct mmio *m, uint32_t off, uint32_t v)
{
	uint16_t reg = v & 0xffff;
	uint8_t id = (v >> 16) & 0xf;

	(void)m;
	if (off == 0x300) {
		if (S.nwcmds < 256)
			S.wcmds[S.nwcmds++] = v;
		if (reg < sizeof(S.slave))
			S.slave[reg] = v >> 24;
		return;
	}
	if (off == 0x304) {
		S.rdfifo = (uint32_t)id << 8 | (reg < sizeof(S.slave) ?
		    S.slave[reg] : 0);
		S.rd_ready = true;
		return;
	}
	S.r[off / 4] = v;
}

static void
test_swr(void)
{
	static struct audiod d;
	struct mmio m = { .name = "swr1", .rd = s_rd, .wr = s_wr };
	struct swr *w;
	uint8_t v = 0, ports[2] = { 1, 2 }, mask[2] = { 3, 1 };
	int r;

	memset(&S, 0, sizeof(S));
	backend_init(&d);
	w = &d.swr[1];
	w->m = &m;
	r = swr_init(w);
	CHECK(r == 0 && w->up && w->version == 0x01060000);
	CHECK(S.r[0x101c / 4] == (1 << 3 | 7));		/* 50 rows, 16 cols */
	CHECK(S.r[0x500 / 4] == 1);			/* auto enumeration */
	CHECK(S.r[0x314 / 4] == (0x7 | 1u << 31));	/* v1.5.1 and later */
	CHECK(S.r[0x1044 / 4] == 2);			/* bus clock start */
	CHECK((S.r[0x1048 / 4] >> 17 & 0x1f) == 0x1f);	/* no pings */
	CHECK(S.r[0x04 / 4] == 3 && S.r[0x210 / 4] == 0x1ffff);
	CHECK(w->wr_fifo_depth == 8 && w->dout == 5);
	/*
	 * the enumerator's bytes as qcom_swrm_enumerate reads them, for the
	 * id of the device tree's "sdw20217010a00" with unique id 3: version
	 * 2, manufacturer 0x217, part 0x10a, class 0
	 */
	r = swr_enumerate(w, 3, 10);
	CHECK(r == 0 && w->devnum == 1);
	CHECK(w->slave_id == 0x230217010a00ull);
	CHECK(swr_enumerate(w, 4, 10) == -ENODEV);
	w->devnum = 1;
	/* SWRM_REG_VAL_PACK(data, dev, id, reg), ids 1.. */
	S.nwcmds = 0;
	CHECK(swr_write(w, 1, 0x3005, 0x40) == 0);
	CHECK(S.nwcmds == 1 && S.wcmds[0] == (0x3005u | 1u << 16 | 1u << 20 |
	    0x40u << 24));
	S.slave[0x123] = 0x5a;
	CHECK(swr_read(w, 1, 0x123, &v) == 0 && v == 0x5a);
	/* the ids wrap after 14 to 0 */
	w->cmd_id = 14;
	swr_write(w, 1, 0x10, 1);
	CHECK(((S.wcmds[S.nwcmds - 1] >> 16) & 0xf) == 0);
	/* a stream on ports 1 (HPH L/R) and 2 (CLSH) */
	S.nwcmds = 0;
	r = swr_stream(w, ports, 2, mask, true);
	CHECK(r == 0);
	/* the slave: port 1 SampleCtrl1 = si 3, LaneCtrl 1, ChannelEn 3 */
	CHECK(S.slave[0x122] == 3 && S.slave[0x132] == 3);
	CHECK(S.slave[0x128] == 1 && S.slave[0x120] == 3 &&
	    S.slave[0x130] == 3);
	CHECK(S.slave[0x103] == 1);			/* word length 2 */
	CHECK(S.slave[0x222] == 0x1f && S.slave[0x203] == 7 &&
	    S.slave[0x220] == 1);
	/* the master: port 1 bank 0/1 si 3, channels in bits 24.. */
	CHECK(S.r[0x1124 / 4] == (3 | 3u << 24) &&
	    S.r[(0x1124 + 0x40) / 4] == (3 | 3u << 24));
	CHECK(S.r[0x1128 / 4] == 1);			/* lane control */
	CHECK(S.r[0x1134 / 4] == 0xf0);			/* no hstart/stop */
	CHECK(S.r[(0x1134 + 0x100) / 4] == (6 << 4 | 3));	/* port 2 */
	CHECK(S.r[(0x1138 + 0x100) / 4] == 0);		/* port 2 bp mode 0 */
	CHECK(S.r[0x112c / 4] == 1 && S.r[(0x112c + 0x100) / 4] == 7);
	/* the bank switch: broadcast (dev 15, id 15) to FrameCtrl_B1 */
	CHECK(S.wcmds[S.nwcmds - 1] == (0x70u | 15u << 16 | 15u << 20 |
	    0x0fu << 24));
	CHECK(S.r[(0x101c + 0x40) / 4] == (1 << 3 | 7));
	r = swr_stream(w, ports, 2, mask, false);
	CHECK(r == 0 && S.slave[0x120] == 0 && (S.r[0x1124 / 4] >> 24) == 0);
	printf("soundwire: done\n");
}

static void
test_voice(void)
{
	static struct audiod d;
	struct voice v;
	unsigned mark;

	setup(&d);
	memset(&v, 0, sizeof(v));
	CHECK(voice_start(&d.q, &v) == 0 && v.state == VP_UP);
	CHECK(v.mvm == 0x11 && v.cvp == 0x22);
	CHECK(A.nops == 6 && A.ops[0] == 0x110ff && A.ops[1] == 0x11327 &&
	    A.ops[2] == 0x112bf && A.ops[3] == 0x100c6 &&
	    A.ops[4] == 0x1123e && A.ops[5] == 0x11190);
	mark = A.nops;
	CHECK(voice_stop(&d.q, &v) == 0 && v.state == VP_IDLE);
	CHECK(A.nops - mark == 5 && A.ops[mark] == 0x11192 &&
	    A.ops[mark + 1] == 0x1123f && A.ops[mark + 2] == 0x110e1 &&
	    A.ops[mark + 3] == 0x1003c && A.svcs[mark + 3] == 0xb &&
	    A.ops[mark + 4] == 0x1003c && A.svcs[mark + 4] == 0x9);
	/* a refused CVP: MVM destroyed again, failed at step 2 */
	setup(&d);
	memset(&v, 0, sizeof(v));
	A.refuse_op = 0x112bf;
	CHECK(voice_start(&d.q, &v) != 0 && v.state == VP_FAILED &&
	    v.failed_step == 2 && v.failed_status == 0x12);
	CHECK(A.ops[A.nops - 1] == 0x1003c && A.svcs[A.nops - 1] == 9);
	printf("voice: done\n");
}

/* a file of tools/trace2seq.py (the Makefile's sample.seq), and base.seq */
static void
test_file(const char *path)
{
	struct seq s;
	char e[256];

	CHECK(seq_load(&s, path, e, sizeof(e)) == 0);
	if (s.nsect == 0)
		printf("%s\n", e);
	CHECK(seq_find(&s, "power") != NULL && seq_find(&s, "pins") != NULL &&
	    seq_find(&s, "volume") != NULL);
	seq_free(&s);
	printf("%s: done\n", path);
}

int
main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++)
		test_file(argv[i]);
	test_packets();
	test_pcm();
	test_seq();
	test_swr();
	test_voice();
	if (failures != 0) {
		printf("%d failure(s)\n", failures);
		return (1);
	}
	printf("all passed\n");
	return (0);
}
