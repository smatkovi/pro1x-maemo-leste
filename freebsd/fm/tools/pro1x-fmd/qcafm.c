/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The WCN3950's FM receiver (qcafm.h), radio-qca-fm.c without V4L2: the
 * same commands in the same order, the same parsing of the events.
 */

#include <errno.h>
#include <string.h>

#include "qcafm.h"

/* event mask: signal level, RDS sync, audio (stereo) changes */
#define	FM_EVENTS		0x07
/* the chip's RDS processing: radio text, station names, simple name */
#define	FM_RDS_RT		(1u << 0)
#define	FM_RDS_PS		(1u << 1)
#define	FM_RDS_PS_SIMPLE	(1u << 2)
#define	FM_EMPHASIS_75		0
#define	FM_EMPHASIS_50		1
#define	FM_SPACING_100		1
#define	FM_RDS_STD_RDS		1
#define	FM_SEEK			0
#define	FM_HARD_MUTE_BOTH	3

static uint32_t
le32(const uint8_t *p)
{
	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24);
}

static void
put_le32(uint8_t *p, uint32_t v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = v >> 24;
}

void
qfm_init(struct qfm *fm, struct qbt *bt)
{
	memset(fm, 0, sizeof(*fm));
	fm->bt = bt;
	fm->freq = FM_BAND_LOW;
	bt->fm_cb = qfm_event;
	bt->fm_arg = fm;
}

static void
changed(struct qfm *fm, const char *what)
{
	if (fm->changed != NULL)
		fm->changed(fm->changed_arg, what);
}

void
qfm_parse_station(const uint8_t *p, struct fm_station *st)
{
	st->freq = le32(p);
	st->service = p[4];
	st->rssi = (int8_t)p[5];
	st->stereo = p[6];
	st->rds_sync = p[7];
	st->mute = p[8];
	st->sinr = (int8_t)p[9];
	st->intf_det_th = p[10];
}

/* RDS text as radio-qca-fm hands it to the controls: printable, padded */
static void
rds_text(char *dst, const uint8_t *src, int len, int size)
{
	int i;
	uint8_t c;

	for (i = 0; i < size; i++) {
		c = i < len ? src[i] : ' ';
		dst[i] = (c < 0x20 || c > 0x7e) ? ' ' : (char)c;
	}
	dst[size] = '\0';
}

static void
strip(char *s)
{
	size_t n = strlen(s);

	while (n > 0 && s[n - 1] == ' ')
		s[--n] = '\0';
	n = strspn(s, " ");
	if (n > 0)
		memmove(s, s + n, strlen(s + n) + 1);
}

/* radio-qca-fm.c: qca_fm_recv and qca_fm_rds_work */
void
qfm_event(void *arg, uint8_t evt, const uint8_t *p, uint8_t plen)
{
	struct qfm *fm = arg;
	int len;

	switch (evt) {
	case FM_EV_TUNE_STATUS:
		/* a sub-event byte, then the station */
		if (plen >= 1 + 11) {
			qfm_parse_station(p + 1, &fm->st);
			if (fm->st.freq != 0)
				fm->freq = fm->st.freq;
		}
		fm->tunes++;
		/* a new station: its RDS starts again */
		fm->pty = 0;
		fm->ps[0] = fm->rt[0] = '\0';
		changed(fm, "tune");
		break;
	case FM_EV_SEARCH_COMPLETE:
	case FM_EV_SEARCH_RDS_COMPLETE:
		fm->seek_done = true;
		break;
	case FM_EV_PS:
		/* PI (2), PTY, TA/TP flags (4), names, then the names */
		if (plen < 8 + 8 || p[7] == 0)
			break;
		fm->pty = p[2] & 0x1f;
		rds_text(fm->ps, p + 8, 8, 8);
		strip(fm->ps);
		fm->ps_count++;
		changed(fm, "ps");
		break;
	case FM_EV_RT:
		/* PI (2), PTY, TA/TP, A/B flag, then up to 64 characters */
		if (plen < 5)
			break;
		for (len = 0; len < 64 && 5 + len < plen; len++)
			if (p[5 + len] == 0x0d)
				break;
		fm->pty = p[2] & 0x1f;
		rds_text(fm->rt, p + 5, len, len);
		strip(fm->rt);
		fm->rt_count++;
		changed(fm, "rt");
		break;
	default:
		if (fm->bt->debug >= 1)
			qbt_logf(fm->bt, "FM event %#04x, %u bytes", evt, plen);
		break;
	}
}

static int
cmd(struct qfm *fm, uint16_t op, const void *param, uint8_t plen, void *rsp,
    size_t rsp_len)
{
	return (qbt_fm_cmd(fm->bt, op, param, plen, rsp, rsp_len,
	    FM_CMD_TIMEOUT_MS));
}

static int
cmd_u8(struct qfm *fm, uint16_t op, uint8_t v)
{
	return (cmd(fm, op, &v, 1, NULL, 0));
}

static int
set_conf(struct qfm *fm)
{
	uint8_t conf[12];

	/* emphasis, spacing, RDS standard, hlsi, the band in kHz */
	conf[0] = fm->emphasis75 ? FM_EMPHASIS_75 : FM_EMPHASIS_50;
	conf[1] = FM_SPACING_100;
	conf[2] = FM_RDS_STD_RDS;
	conf[3] = 0;
	put_le32(conf + 4, FM_BAND_LOW);
	put_le32(conf + 8, FM_BAND_HIGH);
	return (cmd(fm, FM_RX_SET_CONF, conf, sizeof(conf), NULL, 0));
}

int
qfm_set_mute(struct qfm *fm, bool mute)
{
	/* hard (both channels), soft */
	uint8_t mode[2] = { mute ? FM_HARD_MUTE_BOTH : 0, 1 };
	int error;

	fm->mute = mute;
	if (!fm->on)
		return (0);
	error = cmd(fm, FM_RX_SET_MUTE, mode, sizeof(mode), NULL, 0);
	return (error);
}

int
qfm_set_mono(struct qfm *fm, bool mono)
{
	/* mono or stereo, signal blending, interference blending, switch */
	uint8_t mode[4] = { mono, 1, 0, 0 };

	fm->mono = mono;
	if (!fm->on)
		return (0);
	return (cmd(fm, FM_RX_SET_STEREO, mode, sizeof(mode), NULL, 0));
}

uint32_t
qfm_round(uint32_t khz)
{
	if (khz < FM_BAND_LOW)
		khz = FM_BAND_LOW;
	if (khz > FM_BAND_HIGH)
		khz = FM_BAND_HIGH;
	return (FM_BAND_LOW + (khz - FM_BAND_LOW + 50) / 100 * 100);
}

/* until a counter moves or the time is up */
static bool
wait_count(struct qfm *fm, unsigned int *count, unsigned int from,
    int timeout_ms)
{
	struct qbt *bt = fm->bt;
	uint64_t until = bt->ops->now_ms(bt->arg) + (uint64_t)timeout_ms;

	while (*count == from) {
		uint64_t t = bt->ops->now_ms(bt->arg);

		if (t >= until || qbt_pump(bt, (int)(until - t)) != 0)
			return (false);
	}
	return (true);
}

int
qfm_tune(struct qfm *fm, uint32_t khz)
{
	uint8_t f[4];
	unsigned int from = fm->tunes;
	int error;

	if (!fm->on)
		return (-ENETDOWN);
	khz = qfm_round(khz);
	put_le32(f, khz);
	if ((error = cmd(fm, FM_TUNE, f, sizeof(f), NULL, 0)) != 0)
		return (error);
	if (!wait_count(fm, &fm->tunes, from, FM_TUNE_TIMEOUT_MS))
		qbt_logf(fm->bt, "no tune status for %u kHz", khz);
	fm->freq = khz;
	return (0);
}

int
qfm_station(struct qfm *fm, struct fm_station *st)
{
	uint8_t rsp[11];
	int error;

	if (!fm->on)
		return (-ENETDOWN);
	if ((error = cmd(fm, FM_RX_GET_STATION, NULL, 0, rsp,
	    sizeof(rsp))) != 0)
		return (error);
	qfm_parse_station(rsp, st);
	fm->st = *st;
	return (0);
}

/* radio-qca-fm.c: qca_fm_s_hw_freq_seek with patch 0017 */
int
qfm_seek(struct qfm *fm, bool up)
{
	uint8_t req[3] = { FM_SEEK, 0, !up };	/* mode, dwell, direction */
	struct qbt *bt = fm->bt;
	struct fm_station st;
	uint32_t from = fm->freq;
	uint64_t until;
	int error;

	if (!fm->on)
		return (-ENETDOWN);
	fm->seek_done = false;
	if ((error = cmd(fm, FM_RX_SEARCH, req, sizeof(req), NULL, 0)) != 0)
		return (error);
	until = bt->ops->now_ms(bt->arg) + FM_SEEK_TIMEOUT_MS;
	while (!fm->seek_done) {
		uint64_t t = bt->ops->now_ms(bt->arg);

		if (t >= until || qbt_pump(bt, (int)(until - t)) != 0)
			break;
	}
	if (!fm->seek_done) {
		cmd(fm, FM_RX_CANCEL_SEARCH, NULL, 0, NULL, 0);
		error = -QFM_ENOSTATION;
	}
	/*
	 * Where the search stopped. Finding nothing, the chip goes round the
	 * whole band (some 7 s) and back to where it started.
	 */
	if (qfm_station(fm, &st) == 0 && st.freq != 0)
		fm->freq = st.freq;
	if (error == 0 && fm->freq == from)
		error = -QFM_ENOSTATION;
	return (error);
}

int
qfm_on(struct qfm *fm)
{
	uint8_t rds[4] = { FM_RDS_RT | FM_RDS_PS | FM_RDS_PS_SIMPLE, 0, 0, 0 };
	uint32_t freq = fm->freq;
	int error;

	if (fm->on)
		return (0);
	if ((error = cmd(fm, FM_RX_ENABLE, NULL, 0, NULL, 0)) != 0)
		return (error);
	fm->on = true;
	if ((error = set_conf(fm)) == 0 &&
	    (error = qfm_set_mono(fm, fm->mono)) == 0 &&
	    (error = cmd_u8(fm, FM_RX_EVENT_MASK, FM_EVENTS)) == 0 &&
	    (error = cmd(fm, FM_RX_RDS_PROCESS, rds, sizeof(rds), NULL,
	    0)) == 0 &&
	    (error = qfm_set_mute(fm, fm->mute)) == 0)
		error = qfm_tune(fm, freq);
	if (error != 0) {
		cmd(fm, FM_RX_DISABLE, NULL, 0, NULL, 0);
		fm->on = false;
		return (error);
	}
	/* the audio to the DSP; nothing stops the radio without it */
	fm->slimbus = cmd_u8(fm, FM_ENABLE_SLIMBUS, 1) == 0;
	if (!fm->slimbus)
		qbt_logf(fm->bt, "no SLIMbus audio port");
	changed(fm, "on");
	return (0);
}

int
qfm_off(struct qfm *fm)
{
	int error;

	if (!fm->on)
		return (0);
	cmd_u8(fm, FM_ENABLE_SLIMBUS, 0);
	error = cmd(fm, FM_RX_DISABLE, NULL, 0, NULL, 0);
	fm->on = false;
	fm->slimbus = false;
	changed(fm, "off");
	return (error);
}

int
qfm_signal(int rssi)
{
	int s = rssi + 120;

	if (s < 0)
		s = 0;
	if (s > 40)
		s = 40;
	return (s * 100 / 40);
}
