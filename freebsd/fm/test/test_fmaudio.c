/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * pro1x-fmaudio on the PC (freebsd/fm/tools/pro1x-fmaudio):
 *
 * - its APR packets byte for byte against Linux's structures filled the
 *   way q6afe.c fills them (struct apr_hdr, afe_port_cmd_set_param_v2,
 *   afe_port_param_data_v2, union afe_port_config, afe_clk_set,
 *   afe_svc_cmd_set_param, the vote requests) and the downstream q6afe.c's
 *   loopback structures;
 * - its SLIMbus messages against qcom_slim_ngd_xfer_msg's assembly (the
 *   header word, Linux's txn->rl - 1, the payloads of get_laddr,
 *   enable_stream, slim_xfer_msg, slim_stream_prepare);
 * - the whole program (pro1x-fmaudio.c, its main renamed) against a
 *   simulated NGD with the ADSP's bus manager and a WCN3950 behind it,
 *   and a simulated ADSP behind APR: the order of everything, that no
 *   register is touched before the QMI power request, the WCN3950's port
 *   registers, a NACKed capability answer, a chip that is late or absent,
 *   a retained NGD, an AFE port the ADSP refuses, and the way back.
 *
 * The simulation is written from the protocol (what the manager answers),
 * not from ngd.c.
 */

#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define	FMAUDIO_TEST
#define	main fmaudio_main
#include "../tools/pro1x-fmaudio/pro1x-fmaudio.c"
#undef main

static int failures;
#define	CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); \
	failures++; } } while (0)

/* --- Linux's structures (include/linux/soc/qcom/apr.h, q6afe.c) -------- */

struct l_apr_hdr {
	uint16_t hdr_field, pkt_size;
	uint8_t src_svc, src_domain;
	uint16_t src_port;
	uint8_t dest_svc, dest_domain;
	uint16_t dest_port;
	uint32_t token, opcode;
} __attribute__((packed));

struct l_afe_port_cmd_device_start {
	uint16_t port_id, reserved;
} __attribute__((packed));

struct l_afe_port_param_data_v2 {
	uint32_t module_id, param_id;
	uint16_t param_size, reserved;
} __attribute__((packed));

struct l_afe_svc_cmd_set_param {
	uint32_t payload_size, payload_address_lsw, payload_address_msw,
	    mem_map_handle;
} __attribute__((packed));

struct l_afe_port_cmd_set_param_v2 {
	uint16_t port_id, payload_size;
	uint32_t payload_address_lsw, payload_address_msw, mem_map_handle;
} __attribute__((packed));

struct l_afe_param_id_hdmi_multi_chan_audio_cfg {
	uint32_t hdmi_cfg_minor_version;
	uint16_t datatype, channel_allocation;
	uint32_t sample_rate;
	uint16_t bit_width, reserved;
} __attribute__((packed));

struct l_afe_param_id_slimbus_cfg {
	uint32_t sb_cfg_minor_version;
	uint16_t slimbus_dev_id, bit_width, data_format, num_channels;
	uint8_t shared_ch_mapping[8];
	uint32_t sample_rate;
} __attribute__((packed));

struct l_afe_param_id_i2s_cfg {
	uint32_t i2s_cfg_minor_version;
	uint16_t bit_width, channel_mode, mono_stereo, ws_src;
	uint32_t sample_rate;
	uint16_t data_format, reserved;
} __attribute__((packed));

struct l_afe_param_id_tdm_cfg {
	uint32_t tdm_cfg_minor_version, num_channels, sample_rate, bit_width;
	uint16_t data_format, sync_mode, sync_src, nslots_per_frame,
	    ctrl_data_out_enable, ctrl_invert_sync_pulse, ctrl_sync_data_delay,
	    slot_width;
	uint32_t slot_mask;
} __attribute__((packed));

struct l_afe_param_id_cdc_dma_cfg {
	uint32_t cdc_dma_cfg_minor_version, sample_rate;
	uint16_t bit_width, data_format, num_channels, active_channels_mask;
} __attribute__((packed));

struct l_afe_param_id_usb_cfg {
	uint32_t cfg_minor_version, sample_rate;
	uint16_t bit_width, num_channels, data_format, reserved;
	uint32_t dev_token, endian, service_interval;
} __attribute__((packed));

union l_afe_port_config {
	struct l_afe_param_id_hdmi_multi_chan_audio_cfg hdmi_multi_ch;
	struct l_afe_param_id_slimbus_cfg slim_cfg;
	struct l_afe_param_id_i2s_cfg i2s_cfg;
	struct l_afe_param_id_tdm_cfg tdm_cfg;
	struct l_afe_param_id_cdc_dma_cfg dma_cfg;
	struct l_afe_param_id_usb_cfg usb_cfg;
} __attribute__((packed));

struct l_afe_clk_set {
	uint32_t clk_set_minor_version, clk_id, clk_freq_in_hz;
	uint16_t clk_attri, clk_root;
	uint32_t enable;
};

struct l_afe_cmd_remote_lpass_core_hw_vote_request {
	uint32_t hw_block_id;
	char client_name[8];
} __attribute__((packed));

struct l_afe_cmd_remote_lpass_core_hw_devote_request {
	uint32_t hw_block_id, client_handle;
} __attribute__((packed));

/* the downstream techpack/audio/include/dsp/apr_audio-v2.h */
struct l_afe_loopback_cfg_v1 {
	uint32_t loopback_cfg_minor_version;
	uint16_t dst_port_id, routing_mode, enable, reserved;
} __attribute__((packed));

struct l_afe_loopback_gain_per_path_param {
	uint16_t rx_port_id, gain;
} __attribute__((packed));

/* apr_send_pkt and q6afe.c's header: SEQ_CMD, 20 bytes, version 0 */
static void
l_hdr(void *p, uint8_t svc, uint32_t opcode, uint32_t token, size_t size)
{
	struct l_apr_hdr h = {
		.hdr_field = (2 << 8) | ((20 / 4) << 4) | 0,
		.pkt_size = (uint16_t)size, .src_svc = svc,
		.src_domain = 5, .dest_svc = svc, .dest_domain = 4,
		.token = token, .opcode = opcode,
	};

	memcpy(p, &h, sizeof(h));
}

/* q6afe_port_set_param_v2 into buf */
static size_t
l_port_set_param(uint8_t *buf, uint16_t port, uint32_t token, uint32_t module,
    uint32_t param, const void *data, int psize)
{
	struct l_afe_port_cmd_set_param_v2 sp;
	struct l_afe_port_param_data_v2 pd;
	size_t size = 20 + sizeof(sp) + sizeof(pd) + (size_t)psize;

	memset(buf, 0, size);
	l_hdr(buf, 4, AFE_PORT_CMD_SET_PARAM_V2, token, size);
	memset(&sp, 0, sizeof(sp));
	sp.port_id = port;
	sp.payload_size = (uint16_t)(sizeof(pd) + (size_t)psize);
	memset(&pd, 0, sizeof(pd));
	pd.module_id = module;
	pd.param_id = param;
	pd.param_size = (uint16_t)psize;
	memcpy(buf + 20, &sp, sizeof(sp));
	memcpy(buf + 20 + sizeof(sp), &pd, sizeof(pd));
	memcpy(buf + 20 + sizeof(sp) + sizeof(pd), data, (size_t)psize);
	return (size);
}

static void
test_packets(void)
{
	uint8_t a[512], b[512], cfg[AFE_PORT_CONFIG_SIZE], ch[2] = { 160, 161 };
	union l_afe_port_config pc;
	struct l_afe_clk_set cs;
	struct l_afe_svc_cmd_set_param ssp;
	struct l_afe_port_param_data_v2 pd;
	struct l_afe_port_cmd_device_start st;
	struct l_afe_cmd_remote_lpass_core_hw_vote_request vr;
	struct l_afe_cmd_remote_lpass_core_hw_devote_request dv;
	struct l_afe_loopback_cfg_v1 lb;
	struct l_afe_loopback_gain_per_path_param lg;
	size_t la, lb_;

	CHECK(sizeof(union l_afe_port_config) == AFE_PORT_CONFIG_SIZE);
	CHECK(sizeof(struct l_afe_clk_set) == 20);

	/* SLIMBUS_8_TX as q6afe_slim_port_prepare + q6afe_port_start */
	memset(&pc, 0, sizeof(pc));
	pc.slim_cfg.sb_cfg_minor_version = 1;
	pc.slim_cfg.sample_rate = 48000;
	pc.slim_cfg.bit_width = 16;
	pc.slim_cfg.num_channels = 2;
	pc.slim_cfg.data_format = 0;
	pc.slim_cfg.shared_ch_mapping[0] = 160;
	pc.slim_cfg.shared_ch_mapping[1] = 161;
	la = l_port_set_param(a, 0x4011, 152, AFE_MODULE_AUDIO_DEV_INTERFACE,
	    AFE_PARAM_ID_SLIMBUS_CONFIG, &pc, sizeof(pc));
	afe_cfg_slimbus(cfg, 48000, 16, 2, ch);
	lb_ = afe_pkt_port_set_param(b, 0x4011, 152,
	    AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_SLIMBUS_CONFIG, cfg,
	    sizeof(cfg));
	CHECK(la == lb_ && memcmp(a, b, la) == 0);

	/* RX_CODEC_DMA_RX_0 as q6dma_hw_params + q6afe_cdc_dma_port_prepare */
	memset(&pc, 0, sizeof(pc));
	pc.dma_cfg.cdc_dma_cfg_minor_version = 1;
	pc.dma_cfg.sample_rate = 48000;
	pc.dma_cfg.bit_width = 16;
	pc.dma_cfg.data_format = 0;
	pc.dma_cfg.num_channels = 2;
	pc.dma_cfg.active_channels_mask = 3;
	la = l_port_set_param(a, 0xb030, 113, AFE_MODULE_AUDIO_DEV_INTERFACE,
	    AFE_PARAM_ID_CODEC_DMA_CONFIG, &pc, sizeof(pc));
	afe_cfg_cdc_dma(cfg, 48000, 16, 2);
	lb_ = afe_pkt_port_set_param(b, 0xb030, 113,
	    AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_CODEC_DMA_CONFIG, cfg,
	    sizeof(cfg));
	CHECK(la == lb_ && memcmp(a, b, la) == 0);

	/* DEVICE_START / STOP */
	memset(a, 0, sizeof(a));
	l_hdr(a, 4, AFE_PORT_CMD_DEVICE_START, 152, 24);
	st.port_id = 0x4011;
	st.reserved = 0;
	memcpy(a + 20, &st, sizeof(st));
	lb_ = afe_pkt_port_cmd(b, AFE_PORT_CMD_DEVICE_START, 0x4011, 152);
	CHECK(lb_ == 24 && memcmp(a, b, 24) == 0);
	l_hdr(a, 4, AFE_PORT_CMD_DEVICE_STOP, 113, 24);
	st.port_id = 0xb030;
	memcpy(a + 20, &st, sizeof(st));
	lb_ = afe_pkt_port_cmd(b, AFE_PORT_CMD_DEVICE_STOP, 0xb030, 113);
	CHECK(lb_ == 24 && memcmp(a, b, 24) == 0);

	/* q6afe_set_lpass_clock(RX_CORE_MCLK, COUPLE_NO, root 0, 19.2 MHz) */
	memset(&cs, 0, sizeof(cs));
	cs.clk_set_minor_version = 1;
	cs.clk_id = 0x30e;
	cs.clk_freq_in_hz = 19200000;
	cs.clk_attri = 1;
	cs.clk_root = 0;
	cs.enable = 1;
	memset(a, 0, sizeof(a));
	la = 20 + sizeof(ssp) + sizeof(pd) + sizeof(cs);
	l_hdr(a, 4, AFE_SVC_CMD_SET_PARAM, 1024, la);
	memset(&ssp, 0, sizeof(ssp));
	ssp.payload_size = sizeof(pd) + sizeof(cs);
	memset(&pd, 0, sizeof(pd));
	pd.module_id = AFE_MODULE_CLOCK_SET;
	pd.param_id = AFE_PARAM_ID_CLOCK_SET;
	pd.param_size = sizeof(cs);
	memcpy(a + 20, &ssp, sizeof(ssp));
	memcpy(a + 20 + sizeof(ssp), &pd, sizeof(pd));
	memcpy(a + 20 + sizeof(ssp) + sizeof(pd), &cs, sizeof(cs));
	afe_cfg_clock(cfg, 0x30e, 19200000, 1, 0);
	lb_ = afe_pkt_svc_set_param(b, 1024, AFE_MODULE_CLOCK_SET,
	    AFE_PARAM_ID_CLOCK_SET, cfg, 20);
	CHECK(la == lb_ && memcmp(a, b, la) == 0);
	/* and off: freq 0, enable 0 */
	cs.clk_freq_in_hz = 0;
	cs.enable = 0;
	memcpy(a + 20 + sizeof(ssp) + sizeof(pd), &cs, sizeof(cs));
	afe_cfg_clock(cfg, 0x30e, 0, 1, 0);
	lb_ = afe_pkt_svc_set_param(b, 1024, AFE_MODULE_CLOCK_SET,
	    AFE_PARAM_ID_CLOCK_SET, cfg, 20);
	CHECK(la == lb_ && memcmp(a, b, la) == 0);

	/* q6afe_vote_lpass_core_hw: strscpy into 8 bytes */
	memset(a, 0, sizeof(a));
	l_hdr(a, 4, AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST, 4, 32);
	memset(&vr, 0, sizeof(vr));
	vr.hw_block_id = 4;
	memcpy(vr.client_name, "LPASS_H", 8);
	memcpy(a + 20, &vr, sizeof(vr));
	lb_ = afe_pkt_vote(b, 4, "LPASS_HW_DCODEC", true, 0);
	CHECK(lb_ == 32 && memcmp(a, b, 32) == 0);
	/* the downstream devote: token 0, block, handle */
	l_hdr(a, 4, AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST, 0, 28);
	dv.hw_block_id = 4;
	dv.client_handle = 7;
	memcpy(a + 20, &dv, sizeof(dv));
	lb_ = afe_pkt_vote(b, 4, NULL, false, 7);
	CHECK(lb_ == 28 && memcmp(a, b, 28) == 0);

	/* the downstream afe_loopback and afe_loopback_gain, in SET_PARAM_V2 */
	memset(&lb, 0, sizeof(lb));
	lb.loopback_cfg_minor_version = 1;
	lb.dst_port_id = 0xb030;
	lb.routing_mode = 1;
	lb.enable = 1;
	la = l_port_set_param(a, 0x4011, 152, AFE_MODULE_LOOPBACK,
	    AFE_PARAM_ID_LOOPBACK_CONFIG, &lb, sizeof(lb));
	afe_cfg_loopback(cfg, 0xb030, true);
	lb_ = afe_pkt_port_set_param(b, 0x4011, 152, AFE_MODULE_LOOPBACK,
	    AFE_PARAM_ID_LOOPBACK_CONFIG, cfg, 12);
	CHECK(la == lb_ && memcmp(a, b, la) == 0);
	lg.rx_port_id = 0x4011;
	lg.gain = 0x2000;
	la = l_port_set_param(a, 0x4011, 152, AFE_MODULE_LOOPBACK,
	    AFE_PARAM_ID_LOOPBACK_GAIN_PER_PATH, &lg, sizeof(lg));
	afe_cfg_loopback_gain(cfg, 0x4011, 0x2000);
	lb_ = afe_pkt_port_set_param(b, 0x4011, 152, AFE_MODULE_LOOPBACK,
	    AFE_PARAM_ID_LOOPBACK_GAIN_PER_PATH, cfg, 4);
	CHECK(la == lb_ && memcmp(a, b, la) == 0);
}

/* --- SLIMbus messages ----------------------------------------------------- */

/* qcom_slim_ngd_xfer_msg for a logical address: rl - 1, header, payload */
static size_t
l_ngd_msg(uint8_t *buf, uint8_t rl, uint8_t mt, uint8_t mc, uint8_t la,
    const uint8_t *wbuf, size_t n, int tid, int ec)
{
	uint32_t w;
	uint8_t *puc = buf + 3;

	rl--;
	w = SLIM_MSG_ASM_FIRST_WORD(rl, mt, mc, 0, la);
	memcpy(buf, &w, 4);	/* little endian, as the kernel's *pbuf */
	if (tid >= 0)
		*puc++ = (uint8_t)tid;
	if (ec >= 0) {
		*puc++ = ec & 0xff;
		*puc++ = (uint8_t)(ec >> 8);
	}
	memcpy(puc, wbuf, n);
	return (rl);
}

static void
test_messages(void)
{
	static const uint8_t ea[6] = { 0x00, 0x01, 0x20, 0x02, 0x17, 0x02 };
	uint8_t a[64], b[64], w[16];
	size_t la, lb_;
	int exp;

	/* the device tree's codec@1,0 "slim217,220" is the downstream
	 * elemental-addr [00 01 20 02 17 02] */
	{
		struct slim_ea e = { 0, 1, 0x220, 0x217 };
		uint8_t p[7];

		p[0] = 9;
		p[1] = e.instance;
		p[2] = e.dev_index;
		le16enc_(p + 3, e.prod_code);
		le16enc_(p + 5, e.manf_id);
		CHECK(memcmp(p + 1, ea, 6) == 0);
		/* get_laddr: rl 11, 7 bytes (tid, EA), no TID/EC of the core */
		la = l_ngd_msg(a, 11, 2, 0x0d, 0xff, p, 7, -1, -1);
		lb_ = ngd_build(b, 2, 0x0d, 0xff, p, 7);
		CHECK(la == lb_ && la == 10 && memcmp(a, b, la) == 0);
	}
	/* REPORT_SATELLITE: rl 8 */
	{
		static const uint8_t sat[] = { 0xd9, 0xc5, 1, 1 };

		la = l_ngd_msg(a, 8, 6, 0x01, 0xff, sat, 4, -1, -1);
		lb_ = ngd_build(b, 6, 0x01, 0xff, sat, 4);
		CHECK(la == lb_ && memcmp(a, b, la) == 0);
	}
	/* slim_write(1 byte at 0x101): rl 6 + 1, EC sl 0 | 8 | addr << 4 */
	w[0] = 0x06;
	la = l_ngd_msg(a, 7, 0, 0x68, 0xc3, w, 1, -1, 0x8 | 0x101 << 4);
	{
		uint8_t p[3] = { (0x8 | 0x101 << 4) & 0xff,
		    (0x8 | 0x101 << 4) >> 8, 0x06 };

		lb_ = ngd_build(b, 0, 0x68, 0xc3, p, 3);
	}
	CHECK(la == lb_ && la == 6 && memcmp(a, b, la) == 0);
	/* the stream's DEF_ACT_CHAN at 48 kHz/16 bit: rl num_bytes + 4 */
	CHECK(slim_coef(12, &exp) == 1 && exp == 2);
	CHECK(slim_coef(1, &exp) == 0 && exp == 0);
	CHECK(slim_coef(4, &exp) == 0 && exp == 2);
	CHECK(slim_coef(6, &exp) == 1 && exp == 1);
	CHECK(slim_coef(24, &exp) == 1 && exp == 3);
	{
		uint8_t p[7] = { 0xc2, 16 >> 2 | 1 << 5, 2 << 4 | 0, 3 | 0x80,
		    5, 160, 161 };

		la = l_ngd_msg(a, 7 + 4, 2, 0x21, 0xff, p, 7, -1, -1);
		lb_ = ngd_build(b, 2, 0x21, 0xff, p, 7);
		CHECK(la == lb_ && la == 10 && memcmp(a, b, la) == 0);
	}
}

/* --- the simulation: NGD, manager, WCN3950; ADSP ------------------------- */

#define	SIM_NGD_LA	0x40
#define	SIM_PGD_LA	0xc2
#define	SIM_IFD_LA	0xc3

static struct sim {
	/* the scenario */
	bool		nack_first_sat, retained, no_capability, wcn_never;
	unsigned	wcn_late;	/* ADDR_QUERYs answered "unknown" */
	uint32_t	fail_opcode;	/* the ADSP refuses this command ... */
	uint16_t	fail_port;	/* ... for this port */
	/* the NGD */
	bool		qmi_powered;
	unsigned	mmio_before_qmi;
	uint32_t	cfg, status, rxmsgq, inten, intstat;
	uint32_t	txw[16];
	uint8_t		rxq[16][40];
	unsigned	rxlen[16], rxn;
	bool		sat_ok;
	uint8_t		ifd[0xc00];
	bool		bad_msg;
	/* the ADSP */
	uint8_t		aq[16][256];
	size_t		aqlen[16];
	unsigned	aqn;
	bool		voted, bad_pkt;
	/* what happened */
	char		ev[128][80];
	unsigned	nev;
} sim;

static void
ev(const char *fmt, ...)
{
	va_list ap;

	if (sim.nev == 128)
		return;
	va_start(ap, fmt);
	vsnprintf(sim.ev[sim.nev++], sizeof(sim.ev[0]), fmt, ap);
	va_end(ap);
}

static void
sim_reset(void)
{
	memset(&sim, 0, sizeof(sim));
	sim.ifd[WCN399X_HW_REV_MSB] = 0x12;
	sim.ifd[WCN399X_HW_REV_LSB] = 0x01;
	sim.status = 0;
}

static void
sim_rx(const uint8_t *b, unsigned len)
{
	assert(sim.rxn < 16 && len <= 40);
	memcpy(sim.rxq[sim.rxn], b, len);
	sim.rxlen[sim.rxn++] = len;
}

static void
sim_ack(uint8_t tid)
{
	uint8_t m[5] = { 5 | 6 << 5, 0x25, SIM_NGD_LA, tid, 0x20 };

	sim_rx(m, 5);
}

/* one whole message from the NGD's TX registers */
static void
sim_msg(const uint8_t *m, unsigned len)
{
	uint8_t mt = m[0] >> 5, mc = m[1] & 0x7f, la = m[2];
	const uint8_t *p = m + 3;
	unsigned n = len - 3, addr, ec;

	if ((m[1] & 0x80) != 0) {
		ev("message not to a logical address");
		sim.bad_msg = true;
	}
	sim.intstat |= NGD_INT_TX_MSG_SENT;
	if (mt == 6 && mc == 0x01 && la == 0xff) {
		if (n != 4 || p[0] != 0xd9 || p[1] != 0xc5 || p[2] != 1 ||
		    p[3] != 1)
			sim.bad_msg = true;
		if (sim.nack_first_sat) {
			sim.nack_first_sat = false;
			sim.intstat = (sim.intstat & ~NGD_INT_TX_MSG_SENT) |
			    NGD_INT_TX_NACKED_2;
			ev("satellite NACKed");
			return;
		}
		sim.sat_ok = true;
		ev("satellite");
	} else if (mt == 2 && mc == 0x0d && la == 0xff && n == 7) {
		static const uint8_t pgd[6] = { 0, 1, 0x20, 0x02, 0x17, 0x02 };
		static const uint8_t ifd[6] = { 0, 0, 0x20, 0x02, 0x17, 0x02 };
		uint8_t r[11] = { 11 | 6 << 5, 0x0e, SIM_NGD_LA, p[0] };

		if (!sim.sat_ok)
			sim.bad_msg = true;
		if (sim.wcn_late > 0 || sim.wcn_never) {
			if (sim.wcn_late > 0)
				sim.wcn_late--;
		} else if (memcmp(p + 1, pgd, 6) == 0) {
			memcpy(r + 4, pgd, 6);
			r[10] = SIM_PGD_LA;
		} else if (memcmp(p + 1, ifd, 6) == 0) {
			memcpy(r + 4, ifd, 6);
			r[10] = SIM_IFD_LA;
		}
		ev("address of %02x:%02x", p[1], p[2]);
		sim_rx(r, 11);
	} else if (mt == 0 && mc == 0x68 && n >= 3) {
		ec = (unsigned)(p[0] | p[1] << 8);
		addr = ec >> 4;
		if (la != SIM_IFD_LA || (ec & 0xf) != 0x8 || n != 3 ||
		    addr >= sizeof(sim.ifd))
			sim.bad_msg = true;
		else
			sim.ifd[addr] = p[2];
		ev("write %#x = %#x", addr, p[2]);
	} else if (mt == 0 && mc == 0x60 && n == 3) {
		uint8_t r[5] = { 5, 0x64, SIM_NGD_LA, p[0] };

		ec = (unsigned)(p[1] | p[2] << 8);
		addr = ec >> 4;
		if (la != SIM_IFD_LA || (ec & 0xf) != 0x8)
			sim.bad_msg = true;
		r[4] = sim.ifd[addr & 0xfff];
		sim_rx(r, 5);
		ev("read %#x", addr);
	} else if (mt == 2 && mc == 0x2c && la == 0xff && n == 4) {
		if (p[0] != SIM_PGD_LA)
			sim.bad_msg = true;
		ev("connect port %u channel %u", p[1], p[2]);
		sim_ack(p[3]);
	} else if (mt == 2 && mc == 0x21 && la == 0xff && n == 7) {
		/* {device, bits/4 | C=3, E << 4 | ISO, 48 kHz | FL, tid, chs} */
		if (p[0] != SIM_PGD_LA || p[1] != 0x24 || p[2] != 0x20 ||
		    p[3] != 0x83)
			sim.bad_msg = true;
		ev("define and activate %u %u", p[5], p[6]);
		sim_ack(p[4]);
	} else if (mt == 2 && mc == 0x24 && la == 0xff && n == 2) {
		if (p[1] != SIM_PGD_LA)
			sim.bad_msg = true;
		ev("reconfigure");
		sim_ack(p[0]);
	} else if (mt == 2 && mc == 0x23 && la == 0xff && n == 4) {
		if (p[0] != (2 << 6 | (SIM_PGD_LA & 0x1f)))
			sim.bad_msg = true;
		ev("remove %u %u", p[2], p[3]);
		sim_ack(p[1]);
	} else if (mt == 2 && mc == 0x2e && la == 0xff && n == 3) {
		if (p[0] != SIM_PGD_LA)
			sim.bad_msg = true;
		ev("disconnect port %u", p[1]);
		sim_ack(p[2]);
	} else {
		ev("invalid mt %u mc %#x la %#x len %u", mt, mc, la, len);
		sim.bad_msg = true;
		sim.intstat = (sim.intstat & ~NGD_INT_TX_MSG_SENT) |
		    NGD_INT_MSG_TX_INVAL;
	}
}

static uint32_t
sim_rd(struct mmio *m, uint32_t off)
{
	uint32_t reg, v = 0;
	unsigned i;

	if (!sim.qmi_powered)
		sim.mmio_before_qmi++;
	if (off == 0)
		return (0x10050000);
	if (off < NGD_BASE || off >= NGD_BASE + 0x1000)
		return (0);
	reg = off - NGD_BASE;
	switch (reg) {
	case NGD_CFG:
		return (sim.cfg);
	case NGD_STATUS:
		return (sim.status);
	case NGD_RX_MSGQ_CFG:
		return (sim.rxmsgq);
	case NGD_INT_EN:
		return (sim.inten);
	case NGD_INT_STAT:
		return (sim.intstat | (sim.rxn ? NGD_INT_RX_MSG_RCVD : 0));
	}
	if (reg >= NGD_RX_MSG && reg < NGD_RX_MSG + 40 && sim.rxn > 0) {
		i = reg - NGD_RX_MSG;
		v = (uint32_t)sim.rxq[0][i] | (uint32_t)sim.rxq[0][i + 1] << 8 |
		    (uint32_t)sim.rxq[0][i + 2] << 16 |
		    (uint32_t)sim.rxq[0][i + 3] << 24;
	}
	return (v);
}

static void
sim_wr(struct mmio *m, uint32_t off, uint32_t v)
{
	uint32_t reg = off - NGD_BASE;
	uint8_t msg[64];
	unsigned i, len;

	if (!sim.qmi_powered)
		sim.mmio_before_qmi++;
	switch (reg) {
	case NGD_CFG:
		if (v != sim.cfg)
			ev("cfg %#x", v);
		sim.cfg = v;
		if ((v & NGD_CFG_ENABLE) && !(sim.status & NGD_LADDR) &&
		    !sim.no_capability) {
			/* enumerated; the manager announces itself */
			uint8_t c[6] = { 6 | 6 << 5, 0x00, SIM_NGD_LA, 1, 2, 3 };

			sim.status |= NGD_LADDR;
			sim_rx(c, 6);
		}
		return;
	case NGD_INT_EN:
		sim.inten = v;
		return;
	case NGD_RX_MSGQ_CFG:
		sim.rxmsgq = v;
		return;
	case NGD_INT_CLR:
		if ((v & NGD_INT_RX_MSG_RCVD) && sim.rxn > 0) {
			memmove(sim.rxq[0], sim.rxq[1], sizeof(sim.rxq[0]) *
			    (sim.rxn - 1));
			memmove(sim.rxlen, sim.rxlen + 1, sizeof(sim.rxlen[0]) *
			    (sim.rxn - 1));
			sim.rxn--;
		}
		sim.intstat &= ~v;
		return;
	}
	if (reg >= NGD_TX_MSG && reg < NGD_TX_MSG + 64) {
		i = (reg - NGD_TX_MSG) / 4;
		sim.txw[i] = v;
		len = sim.txw[0] & 0x1f;
		if (i + 1 == (len + 3) / 4) {
			for (i = 0; i < len; i++)
				msg[i] = (uint8_t)(sim.txw[i / 4] >> (8 * (i % 4)));
			sim_msg(msg, len);
		}
		return;
	}
	ev("write to %#x", off);
}

/* the ADSP: an answer for every command */
static void
sim_apr_reply(const uint8_t *cmd, uint32_t opcode, const uint8_t *pl,
    size_t plen)
{
	uint8_t *r;

	assert(sim.aqn < 16);
	r = sim.aq[sim.aqn];
	sim.aqlen[sim.aqn++] = APR_HDR_SIZE + plen;
	le16enc_(r, 0x0250);
	le16enc_(r + 2, (uint16_t)(APR_HDR_SIZE + plen));
	r[4] = cmd[8];
	r[5] = APR_DOMAIN_ADSP;
	le16enc_(r + 6, 0);
	r[8] = cmd[4];
	r[9] = APR_DOMAIN_APPS;
	le16enc_(r + 10, 0);
	memcpy(r + 12, cmd + 12, 4);	/* the token */
	le32enc_(r + 16, opcode);
	memcpy(r + APR_HDR_SIZE, pl, plen);
}

static int
sim_apr_send(void *ctx, const uint8_t *pkt, size_t len)
{
	uint32_t opcode = le32dec_(pkt + 16), token = le32dec_(pkt + 12);
	const uint8_t *p = pkt + APR_HDR_SIZE;
	uint8_t res[8];
	uint16_t port = 0;
	uint32_t status = 0;

	if (le16dec_(pkt + 2) != len || le16dec_(pkt) != 0x0250 ||
	    pkt[5] != APR_DOMAIN_APPS || pkt[9] != APR_DOMAIN_ADSP)
		sim.bad_pkt = true;
	if (pkt[8] == APR_SVC_ADSP_CORE && opcode == AVCS_CMD_ADSP_EVENT_GET_STATE) {
		uint8_t one[4] = { 1, 0, 0, 0 };

		sim_apr_reply(pkt, AVCS_CMDRSP_ADSP_EVENT_GET_STATE, one, 4);
		ev("adsp state");
		return (0);
	}
	if (pkt[8] != APR_SVC_AFE)
		sim.bad_pkt = true;
	switch (opcode) {
	case AFE_PORT_CMD_SET_PARAM_V2:
		port = le16dec_(p);
		if (le16dec_(p + 2) != len - APR_HDR_SIZE - 16 ||
		    le16dec_(p + 24) != len - APR_HDR_SIZE - 28)
			sim.bad_pkt = true;
		if (le32dec_(p + 20) == AFE_PARAM_ID_SLIMBUS_CONFIG) {
			const uint8_t *c = p + 28;

			if (le32dec_(c) != 1 || le16dec_(c + 6) != 16 ||
			    le16dec_(c + 10) != 2 || c[12] != 160 || c[13] != 161 ||
			    le32dec_(c + 20) != 48000 || token != 152)
				sim.bad_pkt = true;
		}
		ev("set %#x param %#x", port, le32dec_(p + 20));
		break;
	case AFE_PORT_CMD_DEVICE_START:
	case AFE_PORT_CMD_DEVICE_STOP:
		port = le16dec_(p);
		ev("%s %#x", opcode == AFE_PORT_CMD_DEVICE_START ? "start" :
		    "stop", port);
		break;
	case AFE_SVC_CMD_SET_PARAM:
		ev("clock %#x %u", le32dec_(p + 28 + 4), le32dec_(p + 28 + 8));
		break;
	case AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST: {
		uint8_t h[4] = { 7, 0, 0, 0 };

		ev("vote %u %.8s", le32dec_(p), (const char *)p + 4);
		sim.voted = true;
		sim_apr_reply(pkt, AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST,
		    h, 4);
		return (0);
	}
	case AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST:
		if (le32dec_(p + 4) != 7)
			sim.bad_pkt = true;
		ev("devote %u", le32dec_(p));
		sim.voted = false;
		break;
	default:
		ev("opcode %#x", opcode);
		sim.bad_pkt = true;
	}
	if (opcode == sim.fail_opcode && port == sim.fail_port)
		status = 0x2;	/* ADSP_EFAILED */
	le32enc_(res, opcode);
	le32enc_(res + 4, status);
	sim_apr_reply(pkt, APR_BASIC_RSP_RESULT, res, 8);
	return (0);
}

static int
sim_apr_recv(void *ctx, uint8_t *buf, size_t size, int timeout_ms)
{
	size_t n;

	if (sim.aqn == 0)
		return (0);
	n = sim.aqlen[0];
	memcpy(buf, sim.aq[0], n);
	memmove(sim.aq[0], sim.aq[1], sizeof(sim.aq[0]) * (sim.aqn - 1));
	memmove(sim.aqlen, sim.aqlen + 1, sizeof(sim.aqlen[0]) * (sim.aqn - 1));
	sim.aqn--;
	return ((int)n);
}

/* the hooks pro1x-fmaudio.c leaves to the test */
static int
qmi_up(struct slimqmi *q)
{
	sim.qmi_powered = true;
	q->powered = true;
	q->s = -1;
	ev("qmi power active");
	if (sim.retained)
		sim.status |= NGD_LADDR;
	return (0);
}

static void
qmi_down(struct slimqmi *q)
{
	if (q->powered)
		ev("qmi power inactive");
	q->powered = false;
	sim.qmi_powered = false;
}

static int
map_window(struct mmio *m, const char *name, uint64_t pa, size_t len)
{
	int trace = m->trace;

	memset(m, 0, sizeof(*m));
	m->trace = trace;
	m->name = name;
	m->pa = pa;
	m->len = len;
	m->rd = sim_rd;
	m->wr = sim_wr;
	return (0);
}

static int
apr_attach(struct path *p)
{
	p->aprfd = open("/dev/null", O_RDONLY);
	p->io.send = sim_apr_send;
	p->io.recv = sim_apr_recv;
	p->io.ctx = NULL;
	return (0);
}

/* --- the runs ------------------------------------------------------------- */

static int
run(const char *const *args)
{
	char *argv[16];
	int argc = 0;

	while (args[argc] != NULL) {
		argv[argc] = (char *)args[argc];
		argc++;
	}
	argv[argc] = NULL;
	optind = 0;		/* glibc: start getopt afresh */
	opt.hold_s = 5;
	opt.wait_s = 5;
	opt.gain = AFE_LOOPBACK_GAIN_UNITY;
	opt.no_output = false;
	stop = 0;
	return (fmaudio_main(argc, argv));
}

/* the events, in order, are exactly want[] */
static void
expect(const char *name, const char *const *want)
{
	unsigned i;

	for (i = 0; want[i] != NULL && i < sim.nev; i++)
		if (strcmp(want[i], sim.ev[i]) != 0)
			break;
	if (want[i] != NULL || i != sim.nev) {
		printf("FAIL %s: event %u: got \"%s\", want \"%s\"\n", name, i,
		    i < sim.nev ? sim.ev[i] : "(end)",
		    want[i] != NULL ? want[i] : "(end)");
		for (i = 0; i < sim.nev; i++)
			printf("    %s\n", sim.ev[i]);
		failures++;
	}
}

#define	BUS_UP \
	"qmi power active", "cfg 0x1", "satellite", \
	"address of 00:01", "address of 00:00", "read 0x1", "read 0x2"
#define	PORTS_ON \
	"write 0x104 = 0x6", "write 0x1f1 = 0x3", "write 0x51 = 0x11", \
	"write 0x108 = 0x6", "write 0x1f2 = 0x3", "write 0x52 = 0x11"
#define	STREAM_ON \
	"connect port 1 channel 160", "connect port 2 channel 161", \
	"define and activate 160 161", "reconfigure"
#define	STREAM_OFF \
	"remove 160 161", "reconfigure", "disconnect port 1", \
	"disconnect port 2", "write 0x51 = 0x10", "write 0x52 = 0x10"
#define	OUTPUT_ON \
	"vote 4 LPASS_H", "clock 0x30c 19200000", "clock 0x30d 19200000", \
	"clock 0x30e 19200000", "clock 0x30f 19200000", \
	"set 0x4011 param 0x10206", "set 0x4011 param 0x1020b", \
	"set 0xb030 param 0x102b8", "start 0xb030"
#define	TX_ON	"set 0x4011 param 0x10212", "start 0x4011"

static void
test_runs(void)
{
	/* probe: the bus up to the chip's addresses and revision, and down */
	{
		static const char *const args[] = { "x", "probe", NULL };
		static const char *const want[] = { BUS_UP,
		    "qmi power inactive", NULL };

		sim_reset();
		CHECK(run(args) == 0);
		expect("probe", want);
		CHECK(sim.mmio_before_qmi == 0 && !sim.bad_msg);
		CHECK(sim.inten == DEF_NGD_INT_MASK && sim.cfg == NGD_CFG_ENABLE);
		CHECK((sim.rxmsgq & SLIM_RX_MSGQ_TIMEOUT_VAL) != 0);
	}
	/* afe: everything of play-fm, then back in reverse */
	{
		static const char *const args[] = { "x", "-t", "0", "afe", NULL };
		static const char *const want[] = { BUS_UP, PORTS_ON, STREAM_ON,
		    "adsp state", OUTPUT_ON, TX_ON,
		    "stop 0x4011", "set 0x4011 param 0x1020b", "stop 0xb030",
		    "clock 0x30f 0", "clock 0x30e 0", "clock 0x30d 0",
		    "clock 0x30c 0", "devote 4",
		    STREAM_OFF, "qmi power inactive", NULL };

		sim_reset();
		CHECK(run(args) == 0);
		expect("afe", want);
		CHECK(sim.mmio_before_qmi == 0 && !sim.bad_msg && !sim.bad_pkt);
		CHECK(!sim.voted && sim.ifd[0x104] == 6 && sim.ifd[0x108] == 6);
	}
	/* afe -N: SLIMBUS_8_TX alone */
	{
		static const char *const args[] = { "x", "-N", "-t", "0", "afe",
		    NULL };
		static const char *const want[] = { BUS_UP, PORTS_ON, STREAM_ON,
		    "adsp state", TX_ON, "stop 0x4011", STREAM_OFF,
		    "qmi power inactive", NULL };

		sim_reset();
		CHECK(run(args) == 0);
		expect("afe -N", want);
		CHECK(!sim.bad_msg && !sim.bad_pkt);
	}
	/* a NACKed capability answer, a chip that answers on the 3rd query */
	{
		static const char *const want[] = { "qmi power active", "cfg 0x1",
		    "satellite NACKed", "satellite",
		    "address of 00:01", "address of 00:01", "address of 00:01",
		    "address of 00:00", "read 0x1", "read 0x2",
		    PORTS_ON, STREAM_ON, STREAM_OFF, "qmi power inactive", NULL };
		static const char *const args[] = { "x", "-t", "0", "slim", NULL };

		sim_reset();
		sim.nack_first_sat = true;
		sim.wcn_late = 2;
		CHECK(run(args) == 0);
		expect("NACK and late chip", want);
		CHECK(!sim.bad_msg);
	}
	/* no WCN3950 on the bus: the error, and everything off again */
	{
		static const char *const args[] = { "x", "-w", "0", "start", NULL };
		static const char *const want[] = { "qmi power active", "cfg 0x1",
		    "satellite", "address of 00:01", "qmi power inactive", NULL };

		sim_reset();
		sim.wcn_never = true;
		CHECK(run(args) == 1);
		expect("no chip", want);
	}
	/* a retained NGD: no capability exchange */
	{
		static const char *const args[] = { "x", "probe", NULL };
		static const char *const want[] = { "qmi power active", "cfg 0x1",
		    "address of 00:01", "address of 00:00", "read 0x1",
		    "read 0x2", "qmi power inactive", NULL };

		sim_reset();
		sim.retained = true;
		sim.sat_ok = true;
		CHECK(run(args) == 0);
		expect("retained", want);
	}
	/* the ADSP refuses SLIMBUS_8_TX's start: all of it back */
	{
		static const char *const args[] = { "x", "-t", "0", "afe", NULL };
		static const char *const want[] = { BUS_UP, PORTS_ON, STREAM_ON,
		    "adsp state", OUTPUT_ON, TX_ON,
		    "set 0x4011 param 0x1020b", "stop 0xb030",
		    "clock 0x30f 0", "clock 0x30e 0", "clock 0x30d 0",
		    "clock 0x30c 0", "devote 4",
		    STREAM_OFF, "qmi power inactive", NULL };

		sim_reset();
		sim.fail_opcode = AFE_PORT_CMD_DEVICE_START;
		sim.fail_port = 0x4011;
		CHECK(run(args) == 1);
		expect("refused start", want);
	}
	/* no manager: the capability never comes (10 tries) */
	{
		static const char *const args[] = { "x", "-M", "20", "probe",
		    NULL };

		sim_reset();
		sim.no_capability = true;
		CHECK(run(args) == 1);
		CHECK(sim.nev >= 2 && strcmp(sim.ev[sim.nev - 1],
		    "qmi power inactive") == 0);
	}
}

int
main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	test_packets();
	test_messages();
	test_runs();
	printf("test_fmaudio: %s\n", failures ? "FAILED" : "ok");
	return (failures != 0);
}
