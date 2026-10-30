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

static int
cb_clock(void *ctx, uint32_t id, uint32_t hz)
{
	struct audiod *d = ctx;
	uint8_t pkt[128], cfg[20];
	char what[48];
	size_t len;

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
	if (!on) {
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

static const char *
be_route(struct audiod *d, int be)
{
	return (be == BE_RX ? d->route : be == BE_VA ? "mic" : "amic");
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
	section(d, name);
	snprintf(name, sizeof(name), "%s-up", route);
	afe_in_seq |= has_afe(d, name);
	if (!afe_in_seq && (r = be_port(d, b, true)) != 0) {
		snprintf(name, sizeof(name), "%s-down", b->name);
		section(d, name);
		return (r);
	}
	snprintf(name, sizeof(name), "%s-up", route);
	if (section(d, name) == -ENOENT && d->have_seq)
		fma_log("codec: no path [%s]: the ADSP's side alone", name);
	if (be == BE_RX)
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
	snprintf(name, sizeof(name), "%s-down", b->route);
	section(d, name);
	snprintf(name, sizeof(name), "%s-down", b->name);
	section(d, name);
	b->up = false;
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

int
backend_route(struct audiod *d, const char *route)
{
	struct backend *b = &d->be[BE_RX];
	char name[32];

	if (strcmp(route, "spk") && strcmp(route, "hph") && strcmp(route, "ear"))
		return (-EINVAL);
	if (b->up && strcmp(b->route, route) != 0) {
		d->cur = b;
		snprintf(name, sizeof(name), "%s-down", b->route);
		section(d, name);
		snprintf(name, sizeof(name), "%s-up", route);
		section(d, name);
		section(d, "volume");
		snprintf(b->route, sizeof(b->route), "%s", route);
	}
	snprintf(d->route, sizeof(d->route), "%s", route);
	fma_log("route %s", route);
	return (0);
}

void
backend_mixer(struct audiod *d, unsigned dev, unsigned left)
{
	if (dev >= nitems(d->mix))
		return;
	d->mix[dev] = left;
	fma_dbg("mixer %u: %u", dev, left);
	if (dev == 0 && d->be[BE_RX].up) {	/* SOUND_MIXER_VOLUME */
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
