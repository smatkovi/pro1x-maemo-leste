/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The back ends: what Linux's DPCM brings up behind a front end before
 * its ASM session runs (q6afe-dai.c's prepare, DAPM's power sequence):
 *
 *   rx  RX_CODEC_DMA_RX_0 (0xb030): RX macro, SoundWire swr1, the WCD9370's
 *       HPH/EAR stages, the AW87339s (route spk, hph or ear)
 *   va  VA_CODEC_DMA_TX_0 (0xb021): VA macro, DMIC3 (route mic)
 *   tx  TX_CODEC_DMA_TX_3 (0xb037): TX macro, SoundWire swr0, the WCD9370's
 *       ADCs (route amic: the headset's microphone)
 *
 * Bringing one up runs the replay sequences (codec.c) by name: "power"
 * once (the codec's rails and reset), "<backend>-up" (its clocks at the
 * AFE, the macro, the SoundWire masters, the WCD9370's init), then
 * "<route>-up" (the path: Linux's DAPM sequence, the amplifiers last as
 * patch 0026 orders them), "volume"; the AFE port is configured and
 * started where a sequence says "afe start", else after "<backend>-up"
 * (Linux: the back end's prepare comes before DAPM's stream event). Down
 * goes the other way, the AFE port first, `idle_ms` after the last user
 * (Linux's PulseAudio suspends after 3 s; the macros' runtime PM 3 s).
 * A missing section is left out: without any, the ADSP plays into the
 * codec DMA with no codec behind it (the data path alone, for a test).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audiod.h"

#define	AFE_PARAM_ID_LOOPBACK_CONFIG_	AFE_PARAM_ID_LOOPBACK_CONFIG

/* the device tree's port tables (sm6115.dtsi: qcom,ports-*) */
static const struct swr_port swr1_ports[] = {	/* soundwire@a610000, RX */
	{ 0x03, 0x00, 0x00, 0xff, 0xff, 0xff, 0x01, 0xff, 0x01 },
	{ 0x1f, 0x00, 0x00, 0x00, 0x03, 0x06, 0x07, 0xff, 0x00 },
	{ 0x1f, 0x0b, 0x0b, 0x01, 0xff, 0xff, 0x04, 0xff, 0x00 },
	{ 0x07, 0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 },
	{ 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00 },
};
static const struct swr_port swr0_ports[] = {	/* soundwire@a740000, VA_TX */
	{ 0x03, 0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 },
	{ 0x03, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 },
	{ 0x03, 0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 },
};
/* the WCD9370's devices: reg = <0 3> on swr0 (TX), <0 4> on swr1 (RX) */
#define	WCD_TX_UNIQUE	3
#define	WCD_RX_UNIQUE	4

static uint32_t vote_handle[8];
static bool cb_skip(void *ctx, const struct seq_line *l);

int
afe_cmd(struct q6 *q, const uint8_t *pkt, size_t len, const char *what)
{
	int r = apr_txn(q, pkt, len, 0, NULL, NULL);

	if (r != 0)
		fma_log("afe: %s: %s", what, q6_err(r));
	else
		fma_dbg("afe: %s: done", what);
	return (r);
}

static int
port_cmd(struct audiod *d, uint16_t port, unsigned token, uint32_t param,
    const uint8_t *cfg, bool start)
{
	uint8_t pkt[256];
	char what[48];
	size_t len;
	int r;

	if (start) {
		len = afe_pkt_port_set_param(pkt, port, token,
		    AFE_MODULE_AUDIO_DEV_INTERFACE, param, cfg,
		    AFE_PORT_CONFIG_SIZE);
		snprintf(what, sizeof(what), "port %#x configuration", port);
		if ((r = afe_cmd(&d->q, pkt, len, what)) != 0)
			return (r);
	}
	len = afe_pkt_port_cmd(pkt, start ? AFE_PORT_CMD_DEVICE_START :
	    AFE_PORT_CMD_DEVICE_STOP, port, token);
	snprintf(what, sizeof(what), "port %#x %s", port, start ? "start" :
	    "stop");
	r = afe_cmd(&d->q, pkt, len, what);
	if (r == 0)
		fma_log("afe: port %#x %s", port, start ? "started" : "stopped");
	return (r);
}

static int
be_port(struct audiod *d, struct backend *b, bool start)
{
	uint8_t cfg[AFE_PORT_CONFIG_SIZE];
	int r;

	if (b->port_started == start)
		return (0);
	afe_cfg_cdc_dma(cfg, 48000, 16, b->channels);
	r = port_cmd(d, b->afe_port, b->afe_idx, AFE_PARAM_ID_CODEC_DMA_CONFIG,
	    cfg, start);
	if (r == 0 || !start)
		b->port_started = start;
	return (r);
}

/* --- what the sequences act on: the ADSP and SoundWire parts --------------- */

static int
cb_afe(void *ctx, const char *what)
{
	struct audiod *d = ctx;

	if (d->cur == NULL)
		return (-ENXIO);
	return (be_port(d, d->cur, strcmp(what, "start") == 0) < 0 ? -EIO : 0);
}

/*
 * Clocks and votes are counted, as Linux's clock framework counts its
 * prepares: the jack's [jack-up] and a route's sequences turn on the same
 * TX macro clocks, and only the last off reaches the ADSP.
 */
static int
cb_clock(void *ctx, uint32_t id, uint32_t hz)
{
	struct audiod *d = ctx;
	uint8_t pkt[128], cfg[20];
	char what[48];
	size_t len;
	unsigned i = id & 0x1f;

	if (hz != 0 && d->clk_ref[i]++ > 0)
		return (0);
	if (hz == 0) {
		if (d->clk_ref[i] == 0)
			return (0);
		if (--d->clk_ref[i] > 0)
			return (0);
	}

	afe_cfg_clock(cfg, id, hz, LPASS_CLK_ATTRIBUTE_COUPLE_NO,
	    Q6AFE_LPASS_CLK_ROOT_DEFAULT);
	len = afe_pkt_svc_set_param(pkt, AFE_CLK_TOKEN, AFE_MODULE_CLOCK_SET,
	    AFE_PARAM_ID_CLOCK_SET, cfg, sizeof(cfg));
	snprintf(what, sizeof(what), "clock %#x at %u Hz", id, hz);
	return (afe_cmd(&d->q, pkt, len, what) != 0 ? -EIO : 0);
}

static int
cb_vote(void *ctx, uint32_t block, bool on)
{
	struct audiod *d = ctx;
	uint8_t pkt[64], rsp[APR_PKT_MAX];
	size_t len, rlen = 0;
	int r;

	if (block >= nitems(vote_handle))
		return (-EINVAL);
	if (on && d->vote_ref[block]++ > 0)
		return (0);
	if (!on) {
		if (d->vote_ref[block] == 0 || --d->vote_ref[block] > 0)
			return (0);
		if (vote_handle[block] == 0)
			return (0);
		len = afe_pkt_vote(pkt, block, NULL, false, vote_handle[block]);
		vote_handle[block] = 0;
		return (afe_cmd(&d->q, pkt, len, "devote") != 0 ? -EIO : 0);
	}
	/* the client's name as Linux's vote clocks have it (7 characters) */
	len = afe_pkt_vote(pkt, block, block == Q6AFE_LPASS_CORE_HW_MACRO_BLOCK ?
	    "LPASS_HW_MACRO" : "LPASS_HW_DCODEC", true, 0);
	r = apr_txn(&d->q, pkt, len,
	    AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST, rsp, &rlen);
	if (r == 0 && rlen >= APR_HDR_SIZE + 4)
		vote_handle[block] = le32dec_(rsp + APR_HDR_SIZE);
	if (r == -ENOMSG)
		r = 0;		/* acknowledged without a handle */
	if (r != 0)
		fma_log("afe: vote for block %u: %s", block, q6_err(r));
	else
		fma_log("afe: block %u voted on, handle %u", block,
		    vote_handle[block]);
	return (r != 0 ? -EIO : 0);
}

static struct swr *
swr_by_name(struct audiod *d, const char *name)
{
	if (strcmp(name, "swr0") == 0)
		return (&d->swr[0]);
	if (strcmp(name, "swr1") == 0)
		return (&d->swr[1]);
	return (NULL);
}

/*
 * The WCD9370's probe-time setup, which no run-time trace shows (Linux's
 * wcd937x_soc_codec_probe: wcd937x_io_init with its efuse-dependent
 * branches, then the interrupt levels), over its TX device (swr0). The
 * registers of wcd937x.h.
 */
#define	WCD937X_ANA_BIAS			0x3001
#define	WCD937X_BIAS_VBG_FINE_ADJ		0x3029
#define	WCD937X_MICB1_TEST_CTL_1		0x306b
#define	WCD937X_MICB1_TEST_CTL_2		0x306c
#define	WCD937X_MICB2_TEST_CTL_1		0x306e
#define	WCD937X_MICB2_TEST_CTL_2		0x306f
#define	WCD937X_MICB3_TEST_CTL_1		0x3071
#define	WCD937X_MICB3_TEST_CTL_2		0x3072
#define	WCD937X_HPH_SURGE_HPHLR_SURGE_EN	0x30e2
#define	WCD937X_SLEEP_CTL			0x3103
#define	WCD937X_LDORXTX_CONFIG			0x312a
#define	WCD937X_HPH_NEW_INT_RDAC_GAIN_CTL	0x3132
#define	WCD937X_HPH_NEW_INT_RDAC_HD2_CTL_L	0x3133
#define	WCD937X_HPH_NEW_INT_RDAC_HD2_CTL_R	0x3136
#define	WCD937X_DIGITAL_INTR_LEVEL_0		0x3474
#define	WCD937X_DIGITAL_EFUSE_REG_16		0x34c0
#define	WCD937X_DIGITAL_EFUSE_REG_17		0x34c1
#define	WCD937X_DIGITAL_EFUSE_REG_29		0x34cd

static int
wcd_upd(struct swr *s, uint16_t reg, uint8_t mask, uint8_t val)
{
	uint8_t old = 0;
	int r;

	if ((r = swr_read(s, s->devnum, reg, &old)) != 0)
		return (r);
	return (swr_write(s, s->devnum, reg, (uint8_t)((old & ~mask) |
	    (val & mask))));
}

static int
wcd_init(struct audiod *d)
{
	struct swr *s = &d->swr[0];
	uint8_t v29 = 0, v16 = 0, v17 = 0;
	int r = 0, i;

	if (s->m == NULL || s->devnum == 0)
		return (-ENXIO);
	r |= swr_read(s, s->devnum, WCD937X_DIGITAL_EFUSE_REG_29, &v29);
	r |= swr_read(s, s->devnum, WCD937X_DIGITAL_EFUSE_REG_16, &v16);
	r |= swr_read(s, s->devnum, WCD937X_DIGITAL_EFUSE_REG_17, &v17);
	if (v16 == 0x02 || v17 > 0x09)
		r |= wcd_upd(s, WCD937X_SLEEP_CTL, 0x0e, v29 & 0x0f);
	else
		r |= wcd_upd(s, WCD937X_SLEEP_CTL, 0x0e, 0x0e);
	r |= wcd_upd(s, WCD937X_SLEEP_CTL, 0x80, 0x80);
	fma_sleep_ms(1);
	r |= wcd_upd(s, WCD937X_SLEEP_CTL, 0x40, 0x40);
	fma_sleep_ms(1);
	r |= wcd_upd(s, WCD937X_LDORXTX_CONFIG, 0x10, 0x00);
	r |= wcd_upd(s, WCD937X_BIAS_VBG_FINE_ADJ, 0xf0, 0x80);
	r |= wcd_upd(s, WCD937X_ANA_BIAS, 0x80, 0x80);
	r |= wcd_upd(s, WCD937X_ANA_BIAS, 0x40, 0x40);
	fma_sleep_ms(10);
	r |= wcd_upd(s, WCD937X_ANA_BIAS, 0x40, 0x00);
	r |= wcd_upd(s, WCD937X_HPH_SURGE_HPHLR_SURGE_EN, 0xff, 0xd9);
	r |= wcd_upd(s, WCD937X_MICB1_TEST_CTL_1, 0xff, 0xfa);
	r |= wcd_upd(s, WCD937X_MICB2_TEST_CTL_1, 0xff, 0xfa);
	r |= wcd_upd(s, WCD937X_MICB3_TEST_CTL_1, 0xff, 0xfa);
	r |= wcd_upd(s, WCD937X_MICB1_TEST_CTL_2, 0x38, 0x00);
	r |= wcd_upd(s, WCD937X_MICB2_TEST_CTL_2, 0x38, 0x00);
	r |= wcd_upd(s, WCD937X_MICB3_TEST_CTL_2, 0x38, 0x00);
	/* the bandgap +5 mV for the Tanggu SMIC part */
	if (v16 == 0x01)
		r |= wcd_upd(s, WCD937X_BIAS_VBG_FINE_ADJ, 0xf0, 0xb0);
	else if (v16 == 0x02) {
		r |= wcd_upd(s, WCD937X_HPH_NEW_INT_RDAC_HD2_CTL_L, 0x1f, 0x04);
		r |= wcd_upd(s, WCD937X_HPH_NEW_INT_RDAC_HD2_CTL_R, 0x1f, 0x04);
		r |= wcd_upd(s, WCD937X_BIAS_VBG_FINE_ADJ, 0xf0, 0xb0);
		r |= wcd_upd(s, WCD937X_HPH_NEW_INT_RDAC_GAIN_CTL, 0xf0, 0x50);
	}
	for (i = 0; i < 3; i++)		/* wcd937x_regmap_irq_chip.num_regs */
		r |= swr_write(s, s->devnum, WCD937X_DIGITAL_INTR_LEVEL_0 + i, 0);
	fma_log("wcd: init (efuse 16 %#x, 17 %#x, 29 %#x)%s", v16, v17, v29,
	    r ? ": with errors" : "");
	return (r ? -EIO : 0);
}

/* "init swr1", "enum swr0 3", "stream swr1 on 1:3 2:1", "dump swr0" */
static int
cb_swr(void *ctx, const char *what)
{
	struct audiod *d = ctx;
	char buf[256], *w[12], *p;
	uint8_t ports[SWR_NPORTS], mask[SWR_NPORTS];
	struct swr *s;
	int n = 0, i;

	snprintf(buf, sizeof(buf), "%s", what);
	for (p = strtok(buf, " "); p != NULL && n < 12; p = strtok(NULL, " "))
		w[n++] = p;
	if (n == 1 && strcmp(w[0], "wcd-init") == 0)
		return (wcd_init(d));
	if (n < 2 || (s = swr_by_name(d, w[1])) == NULL)
		return (-EINVAL);
	if (s->m == NULL)
		return (-ENXIO);
	if (strcmp(w[0], "init") == 0)
		return (swr_init(s));
	if (strcmp(w[0], "enum") == 0)
		return (swr_enumerate(s, n > 2 ? (uint8_t)strtoul(w[2], NULL, 0) :
		    s == &d->swr[0] ? WCD_TX_UNIQUE : WCD_RX_UNIQUE, 1000));
	if (strcmp(w[0], "dump") == 0) {
		swr_dump(s, "sequence");
		return (0);
	}
	if (strcmp(w[0], "stream") == 0 && n >= 3) {
		for (i = 3; i < n && i - 3 < SWR_NPORTS; i++) {
			ports[i - 3] = (uint8_t)strtoul(w[i], &p, 0);
			mask[i - 3] = *p == ':' ? (uint8_t)strtoul(p + 1, NULL, 0) :
			    1;
		}
		return (swr_stream(s, ports, (unsigned)(i - 3), mask,
		    strcmp(w[2], "on") == 0));
	}
	return (-EINVAL);
}

static int
cb_sdw_write(void *ctx, int tgt, uint32_t reg, uint8_t val)
{
	struct audiod *d = ctx;
	struct swr *s = &d->swr[tgt == TGT_WCD ? 0 : 1];

	if (s->m == NULL || s->devnum == 0)
		return (-ENXIO);
	return (swr_write(s, s->devnum, (uint16_t)reg, val));
}

static int
cb_sdw_read(void *ctx, int tgt, uint32_t reg, uint8_t *val)
{
	struct audiod *d = ctx;
	struct swr *s = &d->swr[tgt == TGT_WCD ? 0 : 1];

	if (s->m == NULL || s->devnum == 0)
		return (-ENXIO);
	return (swr_read(s, s->devnum, (uint16_t)reg, val));
}

/*
 * $vol_db: the master volume as the RX macro's digital gain
 * (CDC_RX_RXn_RX_VOL_CTL, signed dB, "RX_RXn Digital Volume" -84..40 in
 * lpass-rx-macro.c): 0 mutes (-84 dB), 75 and above are 0 dB, below that
 * half a dB per step.
 */
static int
cb_var(void *ctx, const char *name, uint32_t *val)
{
	struct audiod *d = ctx;
	int level = (int)d->mix[0], db;

	if (strcmp(name, "vol_db") != 0)
		return (-ENOENT);
	db = level <= 0 ? -84 : level >= 75 ? 0 : (level - 75) / 2;
	*val = (uint8_t)(int8_t)db;
	return (0);
}

void
backend_init(struct audiod *d)
{
	struct codec_io *io = &d->cio;

	d->be[BE_RX] = (struct backend){ .name = "rx", .afe_port =
	    AFE_PORT_ID_RX_CODEC_DMA_RX_0, .afe_idx =
	    AFE_TOKEN_RX_CODEC_DMA_RX_0, .channels = 2 };
	d->be[BE_VA] = (struct backend){ .name = "va", .afe_port =
	    AFE_PORT_ID_VA_CODEC_DMA_TX_0, .afe_idx =
	    AFE_TOKEN_VA_CODEC_DMA_TX_0, .channels = 2 };
	d->be[BE_TX] = (struct backend){ .name = "tx", .afe_port =
	    AFE_PORT_ID_TX_CODEC_DMA_TX_3, .afe_idx =
	    AFE_TOKEN_TX_CODEC_DMA_TX_3, .channels = 2 };
	d->swr[0].name = "swr0";
	memcpy(d->swr[0].port, swr0_ports, sizeof(swr0_ports));
	d->swr[0].nports = nitems(swr0_ports);
	d->swr[1].name = "swr1";
	memcpy(d->swr[1].port, swr1_ports, sizeof(swr1_ports));
	d->swr[1].nports = nitems(swr1_ports);
	if (d->route[0] == '\0')
		snprintf(d->route, sizeof(d->route), "spk");
	if (d->recroute[0] == '\0')
		snprintf(d->recroute, sizeof(d->recroute), "mic");
	if (d->idle_ms == 0)
		d->idle_ms = 3000;
	if (d->fm_gain == 0)
		d->fm_gain = AFE_LOOPBACK_GAIN_UNITY;
	d->mix[0] = 75;
	io->ctx = d;
	io->afe = cb_afe;
	io->clock = cb_clock;
	io->vote = cb_vote;
	io->swr = cb_swr;
	io->sdw_write = cb_sdw_write;
	io->sdw_read = cb_sdw_read;
	io->var = cb_var;
	io->skip = cb_skip;
	d->jack_enabled = true;
}

/* --- up and down ---------------------------------------------------------- */

static int
section(struct audiod *d, const char *name)
{
	int r;

	if (!d->have_seq)
		return (-ENOENT);
	r = seq_run(&d->seq, name, &d->cio);
	if (r == -ENOENT)
		fma_dbg("codec: no sequence [%s]", name);
	else if (r > 0)
		fma_log("codec: [%s]: %d error(s)", name, r);
	else if (r == 0)
		fma_log("codec: [%s] done", name);
	return (r);
}

static bool
has_afe(struct audiod *d, const char *name)
{
	const struct seq_sect *s;
	unsigned i;

	if (!d->have_seq || (s = seq_find(&d->seq, name)) == NULL)
		return (false);
	for (i = 0; i < s->count; i++)
		if (d->seq.lines[s->first + i].op == SEQ_AFE)
			return (true);
	return (false);
}

/*
 * The routes as the jack decides them: a plug in the jack takes the
 * playback (hph), a headset's microphone the recording (amic); without
 * one, what was asked for (route, rec; default spk and mic)
 */
const char *
eff_route(struct audiod *d)
{
	switch (d->mbhc.plug) {
	case MBHC_PLUG_HEADPHONE:
	case MBHC_PLUG_HEADSET:
	case MBHC_PLUG_HIGH_HPH:
		if (d->jack_on)
			return ("hph");
	}
	return (d->route);
}

const char *
eff_recroute(struct audiod *d)
{
	if (d->jack_on && d->mbhc.plug == MBHC_PLUG_HEADSET)
		return ("amic");
	return (d->recroute);
}

static const char *
be_route(struct audiod *d, int be)
{
	return (be == BE_RX ? eff_route(d) : be == BE_VA ? "mic" : "amic");
}

static int
backend_up(struct audiod *d, int be)
{
	struct backend *b = &d->be[be];
	char name[32];
	const char *route = be_route(d, be);
	bool afe_in_seq;
	int r;

	d->cur = b;
	if (!d->q.core_ready && (r = q6_core_ready(&d->q, 5000)) != 0)
		return (r);
	if (!d->powered) {
		section(d, "power");
		d->powered = true;
	}
	snprintf(name, sizeof(name), "%s-up", b->name);
	afe_in_seq = has_afe(d, name);
	/*
	 * Only after it are the macro's registers clocked: without it no
	 * sequence touches them (a read of a gated block hangs the bus)
	 */
	b->codec = section(d, name) >= 0;
	snprintf(name, sizeof(name), "%s-up", route);
	afe_in_seq |= has_afe(d, name);
	if (!afe_in_seq && (r = be_port(d, b, true)) != 0) {
		snprintf(name, sizeof(name), "%s-down", b->name);
		section(d, name);
		return (r);
	}
	snprintf(name, sizeof(name), "%s-up", route);
	if (!b->codec)
		fma_log("codec: no [%s-up]: the ADSP's side alone, no codec "
		    "path", b->name);
	else if (section(d, name) == -ENOENT)
		fma_log("codec: no path [%s]", name);
	if (be == BE_RX && b->codec)
		section(d, "volume");
	if (!b->port_started) {
		fma_log("codec: [%s] said nothing of the AFE port: started "
		    "now", name);
		if ((r = be_port(d, b, true)) != 0)
			return (r);
	}
	snprintf(b->route, sizeof(b->route), "%s", route);
	b->up = true;
	fma_log("backend %s up (%s)", b->name, route);
	return (0);
}

static void
backend_down(struct audiod *d, int be)
{
	struct backend *b = &d->be[be];
	char name[32];

	d->cur = b;
	/* Linux: the back end's shutdown stops the port, DAPM follows */
	be_port(d, b, false);
	if (b->codec) {
		snprintf(name, sizeof(name), "%s-down", b->route);
		section(d, name);
		snprintf(name, sizeof(name), "%s-down", b->name);
		section(d, name);
	}
	b->up = false;
	b->codec = false;
	b->idle_at = 0;
	fma_log("backend %s down", b->name);
}

int
backend_get(struct audiod *d, int be)
{
	struct backend *b = &d->be[be];
	int r;

	b->users++;
	b->idle_at = 0;
	if (b->up)
		return (0);
	if ((r = backend_up(d, be)) != 0) {
		b->users--;
		fma_log("backend %s: %s", b->name, q6_err(r));
	}
	return (r);
}

void
backend_put(struct audiod *d, int be)
{
	struct backend *b = &d->be[be];

	if (b->users > 0 && --b->users == 0 && b->up)
		b->idle_at = fma_now_ms() + d->idle_ms;
}

void
backend_tick(struct audiod *d)
{
	uint64_t now = fma_now_ms();
	int i;

	for (i = 0; i < BE_N; i++)
		if (d->be[i].up && d->be[i].users == 0 &&
		    d->be[i].idle_at != 0 && now >= d->be[i].idle_at)
			backend_down(d, i);
}

/* the rx back end on the effective route, if it is up on another */
static void
apply_route(struct audiod *d)
{
	struct backend *b = &d->be[BE_RX];
	const char *route = eff_route(d);
	char name[32];

	if (!b->up || !b->codec || strcmp(b->route, route) == 0)
		return;
	d->cur = b;
	snprintf(name, sizeof(name), "%s-down", b->route);
	section(d, name);
	snprintf(name, sizeof(name), "%s-up", route);
	section(d, name);
	section(d, "volume");
	fma_log("route %s -> %s", b->route, route);
	snprintf(b->route, sizeof(b->route), "%s", route);
}

int
backend_route(struct audiod *d, const char *route)
{
	if (strcmp(route, "spk") && strcmp(route, "hph") && strcmp(route, "ear"))
		return (-EINVAL);
	snprintf(d->route, sizeof(d->route), "%s", route);
	fma_log("route %s asked for, %s in use", route, eff_route(d));
	apply_route(d);
	return (0);
}

/* --- the jack ------------------------------------------------------------- */

/*
 * While the jack is watched its [jack-up] holds what it brought up (swr0's
 * bus: the TX macro's clock registers, the CGCR reset, the master): the
 * other sections' lines on those very registers (a route's back end
 * bringing the TX macro up again, or down) and on that master are left
 * out, as Linux's clocks and runtime PM count their users; their other
 * registers of the same blocks still go. The WCD9370 and the amplifiers
 * stay every route's.
 */
static bool
jack_holds(struct audiod *d, int target, uint32_t reg)
{
	uint32_t key = (uint32_t)target << 24 | (reg & 0xffffff);
	unsigned i;

	for (i = 0; i < d->njack_regs; i++)
		if (d->jack_regs[i] == key)
			return (true);
	return (false);
}

static bool
cb_skip(void *ctx, const struct seq_line *l)
{
	struct audiod *d = ctx;
	int m;

	if (!d->jack_on || d->in_jack_seq)
		return (false);
	switch (l->op) {
	case SEQ_W:
	case SEQ_U:
	case SEQ_R:
	case SEQ_POLL:
		return (l->target >= 0 && l->target < TGT_NMMIO &&
		    jack_holds(d, l->target, l->reg));
	case SEQ_SWR:
		if (strncmp(l->text, "init swr", 8) == 0 ||
		    strncmp(l->text, "enum swr", 8) == 0) {
			m = l->text[8] - '0';
			return (m >= 0 && m < 2 && (d->jack_swr & (1u << m)));
		}
		return (false);
	default:
		return (false);
	}
}

static int
mb_read(void *ctx, uint16_t reg, uint8_t *val)
{
	struct audiod *d = ctx;

	return (swr_read(&d->swr[0], d->swr[0].devnum, reg, val));
}

static int
mb_write(void *ctx, uint16_t reg, uint8_t val)
{
	struct audiod *d = ctx;

	return (swr_write(&d->swr[0], d->swr[0].devnum, reg, val));
}

static void
mb_sleep(void *ctx, unsigned ms)
{
	(void)ctx;
	fma_sleep_ms(ms);
}

static uint64_t
mb_now(void *ctx)
{
	(void)ctx;
	return (fma_now_ms());
}

int
jack_start(struct audiod *d)
{
	const struct seq_sect *sec;
	const struct seq_line *l;
	unsigned i;
	int r;

	if (!d->jack_enabled || d->jack_on)
		return (0);
	if (!d->have_seq || (sec = seq_find(&d->seq, "jack-up")) == NULL) {
		fma_log("jack: no [jack-up] in the sequences: not watched");
		return (-ENOENT);
	}
	if (!d->q.core_ready && (r = q6_core_ready(&d->q, 5000)) != 0)
		return (r);
	if (!d->powered) {
		section(d, "power");
		d->powered = true;
	}
	d->njack_regs = 0;
	d->jack_swr = 0;
	for (i = 0; i < sec->count; i++) {
		l = &d->seq.lines[sec->first + i];
		if ((l->op == SEQ_W || l->op == SEQ_U) && l->target >= 0 &&
		    l->target < TGT_NMMIO && !jack_holds(d, l->target, l->reg) &&
		    d->njack_regs < nitems(d->jack_regs))
			d->jack_regs[d->njack_regs++] =
			    (uint32_t)l->target << 24 | (l->reg & 0xffffff);
		if (l->op == SEQ_SWR && strncmp(l->text, "init swr", 8) == 0)
			d->jack_swr |= 1u << ((l->text[8] - '0') & 1);
	}
	d->cur = NULL;
	d->in_jack_seq = true;
	r = section(d, "jack-up");
	d->in_jack_seq = false;
	if (r < 0 || d->swr[0].devnum == 0) {
		fma_log("jack: [jack-up] did not bring the WCD9370 up (swr0 "
		    "device %u): not watched", d->swr[0].devnum);
		return (-EIO);
	}
	d->mbio = (struct mbhc_io){ mb_read, mb_write, mb_sleep, mb_now,
	    jack_report, jack_button, d };
	d->mbhc.io = &d->mbio;
	if ((r = mbhc_init(&d->mbhc)) != 0)
		return (r);
	d->jack_on = true;
	fma_log("jack: watched (%u registers held, masters %#x)",
	    d->njack_regs, d->jack_swr);
	return (0);
}

void
jack_stop(struct audiod *d)
{
	if (!d->jack_on)
		return;
	d->jack_on = false;
	d->in_jack_seq = true;
	section(d, "jack-down");
	d->in_jack_seq = false;
	d->mbhc.up = false;
}

void
jack_report(void *ctx, int plug)
{
	struct audiod *d = ctx;
	struct voice *v = d->voice;
	char ev[64];
	int be;

	snprintf(ev, sizeof(ev), "jack %s", mbhc_plug_name(plug));
	if (d->notify != NULL)
		d->notify(d, ev);
	apply_route(d);
	/* a call's microphone: the session's tx port is fixed, so anew */
	be = strcmp(eff_recroute(d), "amic") == 0 ? BE_TX : BE_VA;
	if (v != NULL && v->state == VP_UP && be != d->voice_be) {
		fma_log("voice: the microphone changes: the session anew");
		voice_down(d, v);
		voice_up(d, v);
	}
}

void
jack_button(void *ctx, unsigned mask, bool pressed)
{
	struct audiod *d = ctx;
	char ev[64];

	snprintf(ev, sizeof(ev), "button %#x %s", mask, pressed ? "press" :
	    "release");
	if (d->notify != NULL)
		d->notify(d, ev);
}

/*
 * A call's sound: the voice session with the microphone's port (the
 * headset's, TX_CODEC_DMA_TX_3, or the built-in DMIC3, VA_CODEC_DMA_TX_0),
 * then the back ends behind it (Linux's order: the front end's startup,
 * then the back ends' prepare); the playback route is what the caller
 * asked for (ear) unless the jack has a plug.
 */
int
voice_up(struct audiod *d, struct voice *v)
{
	int r;

	d->voice_be = strcmp(eff_recroute(d), "amic") == 0 ? BE_TX : BE_VA;
	v->tx_port = d->be[d->voice_be].afe_port;
	v->rx_port = AFE_PORT_ID_RX_CODEC_DMA_RX_0;
	if (v->state == VP_UP)
		return (0);
	if ((r = voice_start(&d->q, v)) != 0)
		return (r);
	if ((r = backend_get(d, BE_RX)) != 0) {
		voice_stop(&d->q, v);
		return (r);
	}
	if ((r = backend_get(d, d->voice_be)) != 0) {
		backend_put(d, BE_RX);
		voice_stop(&d->q, v);
		return (r);
	}
	fma_log("voice: %s, microphone %s (port %#x)", eff_route(d),
	    d->be[d->voice_be].name, v->tx_port);
	return (0);
}

int
voice_down(struct audiod *d, struct voice *v)
{
	int r;

	if (v->state != VP_UP)
		return (0);
	r = voice_stop(&d->q, v);
	backend_put(d, BE_RX);
	backend_put(d, d->voice_be);
	return (r);
}

void
backend_mixer(struct audiod *d, unsigned dev, unsigned left)
{
	if (dev >= nitems(d->mix))
		return;
	d->mix[dev] = left;
	fma_dbg("mixer %u: %u", dev, left);
	if (dev == 0 && d->be[BE_RX].up && d->be[BE_RX].codec) {
		/* SOUND_MIXER_VOLUME: only while the RX macro is clocked */
		d->cur = &d->be[BE_RX];
		section(d, "volume");
	}
}

/*
 * The FM radio's sound (freebsd/fm, stage 3b): SLIMBUS_8_TX looped back
 * inside the ADSP into RX_CODEC_DMA_RX_0, the rx back end behind it.
 * pro1x-fmaudio keeps the SLIMbus side (the NGD, the WCN3950's stream).
 */
int
backend_fm(struct audiod *d, bool on)
{
	uint8_t cfg[AFE_PORT_CONFIG_SIZE], lb[12], gain[4], pkt[256];
	const uint8_t ch[2] = { 160, 161 };
	size_t len;
	int r;

	if (on == d->fm_on)
		return (0);
	if (on) {
		if ((r = backend_get(d, BE_RX)) != 0)
			return (r);
		afe_cfg_loopback_gain(gain, AFE_PORT_ID_SLIMBUS_MULTI_CHAN_8_TX,
		    d->fm_gain);
		len = afe_pkt_port_set_param(pkt,
		    AFE_PORT_ID_SLIMBUS_MULTI_CHAN_8_TX, AFE_TOKEN_SLIMBUS_8_TX,
		    AFE_MODULE_LOOPBACK, AFE_PARAM_ID_LOOPBACK_GAIN_PER_PATH,
		    gain, sizeof(gain));
		afe_cmd(&d->q, pkt, len, "FM loopback gain");
		afe_cfg_loopback(lb, AFE_PORT_ID_RX_CODEC_DMA_RX_0, true);
		len = afe_pkt_port_set_param(pkt,
		    AFE_PORT_ID_SLIMBUS_MULTI_CHAN_8_TX, AFE_TOKEN_SLIMBUS_8_TX,
		    AFE_MODULE_LOOPBACK, AFE_PARAM_ID_LOOPBACK_CONFIG_, lb,
		    sizeof(lb));
		if ((r = afe_cmd(&d->q, pkt, len, "FM loopback")) == 0) {
			afe_cfg_slimbus(cfg, 48000, 16, 2, ch);
			r = port_cmd(d, AFE_PORT_ID_SLIMBUS_MULTI_CHAN_8_TX,
			    AFE_TOKEN_SLIMBUS_8_TX, AFE_PARAM_ID_SLIMBUS_CONFIG,
			    cfg, true);
		}
		if (r != 0) {
			backend_put(d, BE_RX);
			return (r);
		}
		d->fm_on = true;
		return (0);
	}
	port_cmd(d, AFE_PORT_ID_SLIMBUS_MULTI_CHAN_8_TX, AFE_TOKEN_SLIMBUS_8_TX,
	    0, NULL, false);
	afe_cfg_loopback(lb, AFE_PORT_ID_RX_CODEC_DMA_RX_0, false);
	len = afe_pkt_port_set_param(pkt, AFE_PORT_ID_SLIMBUS_MULTI_CHAN_8_TX,
	    AFE_TOKEN_SLIMBUS_8_TX, AFE_MODULE_LOOPBACK,
	    AFE_PARAM_ID_LOOPBACK_CONFIG_, lb, sizeof(lb));
	afe_cmd(&d->q, pkt, len, "FM loopback off");
	d->fm_on = false;
	backend_put(d, BE_RX);
	return (0);
}
