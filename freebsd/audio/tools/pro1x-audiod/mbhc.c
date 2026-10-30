/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The headset jack: the WCD9370's MBHC (multi-button headset control) as
 * Linux's sound/soc/codecs/wcd-mbhc-v2.c (its ADC detection) drives it
 * with wcd937x.c's callbacks and register fields:
 *
 *   init     wcd_mbhc_initialise: the L-detect pull-up (2 uA), the switch
 *            types (normally open, the device tree's default and the
 *            vendor's qcom,msm-mbhc-hphl-swh/gnd-swh 1), the mechanical
 *            detection on, debounces 96 ms and 16 ms, the MBHC bias and RCO
 *            clock, HS_VREF for 1.6 V, the button thresholds; the micbias
 *            voltages of the device tree (1.8 V); the interrupts unmasked
 *            as Linux requests them (insertion/removal of the plug, buttons,
 *            over-current; the electrical ones masked until needed)
 *   plug     mbhc_plug_detect_fn on MBHC_SW_DET: the detection type
 *            flipped, then on insertion micbias 2 and the plug type found
 *            by the ADC on the mic line (wcd_correct_swch_plug: cross
 *            connection checks, below 50 mV a headphone, up to 1700 mV a
 *            headset, above a high impedance plug; then 3 s of corrections
 *            every 180 ms, here one step per tick); on removal the report
 *   buttons  wcd_mbhc_btn_press/release_handler: BTN_RESULT of a headset,
 *            a press 400 ms long reported as held
 *   the electrical interrupts (wcd_mbhc_adc_hs_ins_irq, _rem_irq): a
 *            headphone that is a headset after all, an extension cable
 *
 * Linux gets the codec's interrupts in-band: the WCD9370's TX device
 * raises a SoundWire alert (its SCP_INT1 implementation-defined bit), the
 * master's interrupt (swr0, SPI 296; its wake interrupt SPI 79 while the
 * bus' clock is stopped) runs the slave's interrupt callback, which reads
 * DIGITAL_INTR_STATUS_0-2 (regmap-irq). The WCD node has no interrupt line
 * of its own. FreeBSD has no driver for those SPIs: the daemon reads
 * DIGITAL_INTR_STATUS_0 over SoundWire every tick (mbhc_tick) while the
 * jack is watched, which needs swr0's bus running (the [jack-up]
 * sequence). Not ported: impedance detection (a line-out is taken as a
 * headphone), moisture detection (off in Linux's default too), the US/EU
 * switch (the Pro1X has no us-euro GPIO).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "audiod.h"

/* wcd937x.h */
#define	R_ANA_HPH			0x3009
#define	R_ANA_MBHC_MECH			0x3014
#define	R_ANA_MBHC_ELECT		0x3015
#define	R_ANA_MBHC_ZDET			0x3016
#define	R_ANA_MBHC_RESULT_3		0x3019
#define	R_ANA_MBHC_BTN0			0x301a
#define	R_ANA_MICB1			0x3022
#define	R_ANA_MICB2			0x3023
#define	R_ANA_MICB2_RAMP		0x3024
#define	R_ANA_MICB3			0x3025
#define	R_MICB1_TEST_CTL_2		0x306c
#define	R_MICB2_TEST_CTL_2		0x306f
#define	R_MICB3_TEST_CTL_2		0x3072
#define	R_HPH_OCP_CTL			0x30ce
#define	R_HPH_PA_CTL2			0x30d2
#define	R_MBHC_NEW_CTL_1		0x3120
#define	R_MBHC_NEW_CTL_2		0x3121
#define	R_MBHC_NEW_PLUG_DETECT_CTL	0x3122
#define	R_MBHC_NEW_FSM_STATUS		0x3125
#define	R_MBHC_NEW_ADC_RESULT		0x3126
#define	R_MBHC_NEW_INT_MECH_DET_CURRENT	0x31b1
#define	R_DIGITAL_CDC_ANA_CLK_CTL	0x3408
#define	R_DIGITAL_CDC_DIG_CLK_CTL	0x3409
#define	R_DIGITAL_INTR_MASK_0		0x346b
#define	R_DIGITAL_INTR_MASK_1		0x346c
#define	R_DIGITAL_INTR_MASK_2		0x346d
#define	R_DIGITAL_INTR_STATUS_0		0x346e
#define	R_DIGITAL_INTR_CLEAR_0		0x3471

/* wcd937x.c's wcd_mbhc_fields: register, mask */
#define	F_L_DET_EN		R_ANA_MBHC_MECH, 0x80
#define	F_MECH_DETECTION_TYPE	R_ANA_MBHC_MECH, 0x20
#define	F_ELECT_DETECTION_TYPE	R_ANA_MBHC_ELECT, 0x08
#define	F_HS_L_DET_PULL_UP_COMP	R_ANA_MBHC_MECH, 0x04
#define	F_HPHL_PLUG_TYPE	R_ANA_MBHC_MECH, 0x10
#define	F_GND_PLUG_TYPE		R_ANA_MBHC_MECH, 0x08
#define	F_SW_HPH_LP_100K_TO_GND	R_ANA_MBHC_MECH, 0x01
#define	F_ELECT_SCHMT_ISRC	R_ANA_MBHC_ELECT, 0x06
#define	F_FSM_EN		R_ANA_MBHC_ELECT, 0x80
#define	F_INSREM_DBNC		R_MBHC_NEW_PLUG_DETECT_CTL, 0x0f
#define	F_BTN_DBNC		R_MBHC_NEW_CTL_1, 0x03
#define	F_HS_VREF		R_MBHC_NEW_CTL_2, 0x03
#define	F_IN2P_CLAMP_STATE	R_ANA_MBHC_RESULT_3, 0x10
#define	F_OCP_FSM_EN		R_HPH_OCP_CTL, 0x10
#define	F_BTN_RESULT		R_ANA_MBHC_RESULT_3, 0x07
#define	F_BTN_ISRC_CTL		R_ANA_MBHC_ELECT, 0x70
#define	F_HPH_PA_EN		R_ANA_HPH, 0xc0
#define	F_MUX_CTL		R_MBHC_NEW_CTL_2, 0x70
#define	F_ADC_EN		R_MBHC_NEW_CTL_1, 0x08
#define	F_ADC_COMPLETE		R_MBHC_NEW_FSM_STATUS, 0x40
#define	F_ADC_TIMEOUT		R_MBHC_NEW_FSM_STATUS, 0x80
#define	F_ADC_RESULT		R_MBHC_NEW_ADC_RESULT, 0xff
#define	F_MICB2_VOUT		R_ANA_MICB2, 0x3f
#define	F_ADC_MODE		R_MBHC_NEW_CTL_1, 0x10
#define	F_DETECTION_DONE	R_MBHC_NEW_CTL_1, 0x04
#define	F_ELECT_ISRC_EN		R_ANA_MBHC_ZDET, 0x02
/* the callbacks' own */
#define	F_RCO_EN		R_MBHC_NEW_CTL_1, 0x80
#define	F_MBHC_BIAS_EN		R_ANA_MBHC_ELECT, 0x01
#define	F_HSDET_PULLUP_C	R_MBHC_NEW_INT_MECH_DET_CURRENT, 0x1f
#define	F_MICB2_EN		R_ANA_MICB2, 0xc0
#define	F_RAMP_EN		R_ANA_MICB2_RAMP, 0x80
#define	F_RAMP_SHIFT_CTRL	R_ANA_MICB2_RAMP, 0x1c
#define	F_HPHPA_GND_R		R_HPH_PA_CTL2, 0x40
#define	F_HPHPA_GND_L		R_HPH_PA_CTL2, 0x10

/* DIGITAL_INTR_STATUS_0's bits (wcd937x_irqs) */
#define	I_BTN_PRESS		(1u << 0)
#define	I_BTN_RELEASE		(1u << 1)
#define	I_ELECT_INS_REM		(1u << 2)	/* hs_rem */
#define	I_ELECT_INS_REM_LEG	(1u << 3)	/* hs_ins */
#define	I_SW_DET		(1u << 4)
#define	I_HPHR_OCP		(1u << 5)
#define	I_HPHL_OCP		(1u << 7)

/* wcd-mbhc-v2.c, wcd937x.c */
#define	WCD_MBHC_HS_V_MAX		1600
#define	HS_VREF_MIN_VAL			1400
#define	HS_PULLUP_I_2P0_UA		0x08
#define	WCD_MBHC_ADC_MICBIAS_MV		1800
#define	WCD_MBHC_ADC_HS_THRESHOLD_MV_	1700
#define	WCD_MBHC_THR_HS_MICB_MV		2700
#define	HPHL_CROSS_CONN_THRESHOLD	100
#define	GND_MIC_SWAP_THRESHOLD		4
#define	HS_DETECT_PLUG_TIME_MS		3000
#define	MBHC_BUTTON_PRESS_THRESHOLD_MIN	250
#define	WCD_FAKE_REMOVAL_MIN_PERIOD_MS	100
#define	FAKE_REM_RETRY_ATTEMPTS		3
#define	WCD_MBHC_FAKE_INS_RETRY		4
#define	LONG_PRESS_MS			400
#define	MICB_VOUT_1800			16	/* (1800 - 1000) / 50 */
enum { MUX_CTL_AUTO, MUX_CTL_IN2P, MUX_CTL_IN3P, MUX_CTL_IN4P,
	MUX_CTL_HPH_L, MUX_CTL_HPH_R };

/*
 * The button thresholds: the Pro1X's vendor kernel (bengal.c's
 * def_wcd_mbhc_cal: 75, 150, 237 mV, then 500); Leste's device tree has
 * no qcom,mbhc-buttons-vthreshold-microvolt, so its Linux uses 500 mV for
 * all eight.
 */
static const unsigned btn_high_mv[8] = { 75, 150, 237, 500, 500, 500, 500,
    500 };

const char *
mbhc_plug_name(int plug)
{
	switch (plug) {
	case MBHC_PLUG_NONE: return ("none");
	case MBHC_PLUG_HEADPHONE: return ("headphone");
	case MBHC_PLUG_HEADSET: return ("headset");
	case MBHC_PLUG_HIGH_HPH: return ("lineout");
	case MBHC_PLUG_GND_MIC_SWAP: return ("swapped");
	}
	return ("?");
}

static unsigned
shift(uint8_t mask)
{
	unsigned s = 0;

	while (mask != 0 && (mask & 1) == 0) {
		mask >>= 1;
		s++;
	}
	return (s);
}

/* snd_soc_component_read_field */
static int
rf(struct mbhc *m, uint16_t reg, uint8_t mask)
{
	uint8_t v = 0;

	if (m->io->read(m->io->ctx, reg, &v) != 0) {
		m->errors++;
		return (0);
	}
	return ((v & mask) >> shift(mask));
}

/* snd_soc_component_write_field: update_bits(mask, val << ffs(mask)) */
static void
wf(struct mbhc *m, uint16_t reg, uint8_t mask, unsigned val)
{
	uint8_t v = 0;

	if (m->io->read(m->io->ctx, reg, &v) != 0) {
		m->errors++;
		return;
	}
	v = (uint8_t)((v & ~mask) | ((val << shift(mask)) & mask));
	if (m->io->write(m->io->ctx, reg, v) != 0)
		m->errors++;
}

static void
wr(struct mbhc *m, uint16_t reg, uint8_t val)
{
	if (m->io->write(m->io->ctx, reg, val) != 0)
		m->errors++;
}

static void
msleep_(struct mbhc *m, unsigned ms)
{
	m->io->sleep_ms(m->io->ctx, ms);
}

/* regmap-irq's enable_irq/disable_irq of the two electrical interrupts */
static void
irq_en(struct mbhc *m, uint8_t bit, bool on)
{
	if (on)
		m->irq_enabled |= bit;
	else
		m->irq_enabled &= (uint8_t)~bit;
	wr(m, R_DIGITAL_INTR_MASK_0, (uint8_t)~m->irq_enabled);
}

static void
report(struct mbhc *m, int plug)
{
	if (plug == m->plug)
		return;
	m->plug = plug;
	if (plug == MBHC_PLUG_HEADSET)
		m->t_report = m->io->now_ms(m->io->ctx);
	m->reports++;
	fma_log("jack: %s", mbhc_plug_name(plug));
	if (m->io->report != NULL)
		m->io->report(m->io->ctx, plug);
}

static void
button(struct mbhc *m, unsigned mask, bool pressed)
{
	if (m->io->button != NULL)
		m->io->button(m->io->ctx, mask, pressed);
	fma_log("jack: button %#x %s", mask, pressed ? "pressed" : "released");
}

/* --- micbias 2 (wcd937x_micbias_control, _micb_adjust_voltage) ---------- */

static void
micb2(struct mbhc *m, bool enable)
{
	if (enable) {
		if (m->micb_ref++ == 0) {
			wf(m, R_DIGITAL_CDC_DIG_CLK_CTL, 0xf0, 0x0f);
			wf(m, R_DIGITAL_CDC_ANA_CLK_CTL, 0x10, 1);
			wf(m, R_MICB1_TEST_CTL_2, 0x01, 1);
			wf(m, R_MICB2_TEST_CTL_2, 0x01, 1);
			wf(m, R_MICB3_TEST_CTL_2, 0x01, 1);
			wf(m, F_MICB2_EN, 1);		/* BIT(6) of 0xc0 */
		}
		return;
	}
	if (m->micb_ref == 0)
		return;
	if (--m->micb_ref == 0) {
		wf(m, F_MICB2_EN, 0);
		wf(m, R_DIGITAL_CDC_ANA_CLK_CTL, 0x10, 0);
	}
}

static void
micb2_voltage(struct mbhc *m, unsigned mv)
{
	unsigned en = (unsigned)rf(m, F_MICB2_EN), cur = (unsigned)rf(m,
	    F_MICB2_VOUT), req = (mv - 1000) / 50;

	if (cur == req)
		return;
	if (en == 1)
		wf(m, F_MICB2_EN, 2);			/* pull up meanwhile */
	wf(m, F_MICB2_VOUT, req);
	if (en == 1) {
		wf(m, F_MICB2_EN, 1);
		msleep_(m, 2);
	}
}

/* wcd937x_mbhc_micb_ctrl_threshold_mic: 2.7 V for a threshold mic */
static void
micb2_thr_mic(struct mbhc *m, bool on)
{
	micb2_voltage(m, on ? WCD_MBHC_THR_HS_MICB_MV : 1800);
}

static void
micbias_disable(struct mbhc *m)
{
	micb2(m, false);
	micb2_thr_mic(m, false);
}

static void
pull_down(struct mbhc *m, bool on)
{
	wf(m, F_HPHPA_GND_R, on);
	wf(m, F_HPHPA_GND_L, on);
}

static void
ramp(struct mbhc *m, bool on)
{
	if (on) {
		wf(m, F_RAMP_SHIFT_CTRL, 0x0c);
		wf(m, F_RAMP_EN, 1);
	} else {
		wf(m, F_RAMP_EN, 0);
		wf(m, F_RAMP_SHIFT_CTRL, 0);
	}
}

/* --- the ADC (wcd_measure_adc_*, the thresholds) --------------------------- */

static int
micbias_mv(struct mbhc *m)
{
	return (1000 + rf(m, F_MICB2_VOUT) * 50);
}

int
mbhc_adc_mv(unsigned val, int micbias)
{
	return ((int)val * 125 * micbias / (WCD_MBHC_ADC_MICBIAS_MV * 10));
}

static int
adc_once(struct mbhc *m, int mux)
{
	int retry = 6, out = -EINVAL, en;

	wf(m, F_ADC_MODE, 0);
	en = rf(m, F_ADC_EN);
	wf(m, F_ADC_EN, 0);
	wf(m, F_FSM_EN, 0);
	wf(m, F_MUX_CTL, (unsigned)mux);
	wf(m, F_FSM_EN, 1);
	wf(m, F_ADC_EN, 1);
	while (retry--) {
		msleep_(m, 1);		/* Linux: 600 us */
		if (rf(m, F_ADC_TIMEOUT))
			continue;
		if (!rf(m, F_ADC_COMPLETE))
			continue;
		out = mbhc_adc_mv((unsigned)rf(m, F_ADC_RESULT), micbias_mv(m));
		break;
	}
	wf(m, F_ADC_EN, (unsigned)en);
	if (out < 0)
		m->adc_failures++;
	return (out);
}

static int
adc_continuous(struct mbhc *m)
{
	int retry = 3, en, v = 0;

	wf(m, F_ELECT_SCHMT_ISRC, 0);
	wf(m, F_ADC_MODE, 1);
	en = rf(m, F_ADC_EN);
	wf(m, F_ADC_EN, 0);
	wf(m, F_FSM_EN, 0);
	wf(m, F_MUX_CTL, MUX_CTL_IN2P);
	wf(m, F_FSM_EN, 1);
	wf(m, F_ADC_EN, 1);
	while (retry--) {
		msleep_(m, 3);
		v = rf(m, F_ADC_RESULT);
	}
	wf(m, F_ADC_EN, (unsigned)en);
	return (mbhc_adc_mv((unsigned)v, micbias_mv(m)));
}

/* > 0 cross connected, 0 not, < 0 unknown (PA on, ADC failed) */
static int
cross_conn(struct mbhc *m)
{
	int elect, mode, en, fsm, l, r;

	if (rf(m, F_HPH_PA_EN))
		return (-EINVAL);
	elect = rf(m, F_ELECT_SCHMT_ISRC);
	wf(m, F_ELECT_SCHMT_ISRC, 0);
	mode = rf(m, F_ADC_MODE);
	en = rf(m, F_ADC_EN);
	fsm = rf(m, F_FSM_EN);
	if ((l = adc_once(m, MUX_CTL_HPH_L)) < 0)
		return (l);
	if ((r = adc_once(m, MUX_CTL_HPH_R)) < 0)
		return (r);
	wf(m, F_FSM_EN, 0);
	wf(m, F_MUX_CTL, MUX_CTL_AUTO);
	wf(m, F_FSM_EN, 1);
	wf(m, F_ADC_EN, (unsigned)en);
	wf(m, F_ADC_MODE, (unsigned)mode);
	wf(m, F_FSM_EN, (unsigned)fsm);
	wf(m, F_ELECT_SCHMT_ISRC, (unsigned)elect);
	return (l > HPHL_CROSS_CONN_THRESHOLD || r > HPHL_CROSS_CONN_THRESHOLD);
}

/* cfg hs_thr 1700 / hph_thr 50 at the configured 1.8 V, scaled otherwise */
static int
hs_thres(struct mbhc *m)
{
	int mb = micbias_mv(m);

	return (mb == 1800 ? 1700 : 1700 * mb / 1800);
}

static int
hph_thres(struct mbhc *m)
{
	int mb = micbias_mv(m);

	return (mb == 1800 ? 50 : 50 * mb / 1800);
}

int
mbhc_plug_from_mv(int mv, int hph_thr, int hs_thr)
{
	if (mv < hph_thr)
		return (MBHC_PLUG_HEADPHONE);
	if (mv > hs_thr)
		return (MBHC_PLUG_HIGH_HPH);
	return (MBHC_PLUG_HEADSET);
}

static bool
spl_headset(struct mbhc *m)
{
	int out, hs, hph;

	micb2_thr_mic(m, true);
	msleep_(m, 10);
	out = adc_once(m, MUX_CTL_IN2P);
	/* wcd_mbhc_get_spl_hs_thres: cfg micb_mv is 1800 -> 1700 scaled */
	hs = WCD_MBHC_ADC_HS_THRESHOLD_MV_ * micbias_mv(m) / WCD_MBHC_ADC_MICBIAS_MV;
	hph = hph_thres(m);
	if (!(out > hs || out < hph))
		return (true);
	micb2_thr_mic(m, false);
	msleep_(m, 10);
	return (false);
}

/* --- plug detection --------------------------------------------------------- */

static void
find_plug_and_report(struct mbhc *m, int plug)
{
	if (plug == MBHC_PLUG_GND_MIC_SWAP) {
		if (m->plug == MBHC_PLUG_HEADPHONE || m->plug == MBHC_PLUG_HEADSET)
			report(m, MBHC_PLUG_NONE);
		return;
	}
	report(m, plug);
}

static void
update_fsm_source(struct mbhc *m, int plug)
{
	if (plug == MBHC_PLUG_HEADPHONE)
		wf(m, F_BTN_ISRC_CTL, 3);
	else if (plug == MBHC_PLUG_HEADSET) {
		if (rf(m, F_MICB2_EN) != 1)
			wf(m, F_BTN_ISRC_CTL, 3);
	} else
		wf(m, F_BTN_ISRC_CTL, 0);
}

/* wcd_correct_swch_plug, up to its 3 s loop */
static void
correct_start(struct mbhc *m)
{
	int try = 0, cc = 0, plug, mv;

	m->c_micbias_mv = micbias_mv(m);
	m->c_hs_thr = hs_thres(m);
	m->c_spl_hs = false;
	m->c_swap_cnt = 0;
	irq_en(m, I_ELECT_INS_REM_LEG, false);
	do {
		cc = cross_conn(m);
	} while (++try < GND_MIC_SWAP_THRESHOLD);
	if (cc > 0) {
		plug = MBHC_PLUG_GND_MIC_SWAP;
		fma_log("jack: ground and mic cross connected");
	} else {
		mv = adc_continuous(m);
		plug = mbhc_plug_from_mv(mv, hph_thres(m), m->c_hs_thr);
		m->last_mv = mv;
		if (plug == MBHC_PLUG_HEADPHONE)
			find_plug_and_report(m, plug);
		else if (plug == MBHC_PLUG_HEADSET) {
			find_plug_and_report(m, plug);
			wf(m, F_ADC_MODE, 0);
			wf(m, F_ADC_EN, 0);
			wf(m, F_DETECTION_DONE, 1);
		}
	}
	m->c_plug = plug;
	m->correcting = true;
	m->c_until = m->io->now_ms(m->io->ctx) + HS_DETECT_PLUG_TIME_MS;
	m->c_next = m->io->now_ms(m->io->ctx) + 180;
}

static void
correct_end(struct mbhc *m)
{
	int plug = m->c_plug;

	m->correcting = false;
	if (plug == MBHC_PLUG_HIGH_HPH) {
		if (m->c_spl_hs)
			plug = MBHC_PLUG_HEADSET;
		else
			wf(m, F_ELECT_ISRC_EN, 1);
	}
	wf(m, F_ADC_MODE, 0);
	wf(m, F_ADC_EN, 0);
	/* a line-out (no impedance detection here) counts as a headphone */
	find_plug_and_report(m, plug == MBHC_PLUG_HIGH_HPH ?
	    MBHC_PLUG_HEADPHONE : plug);
	wf(m, F_DETECTION_DONE, plug == MBHC_PLUG_HEADSET);
	update_fsm_source(m, plug);
	micb2(m, false);
	if (plug == MBHC_PLUG_HEADPHONE) {
		micbias_disable(m);
		irq_en(m, I_ELECT_INS_REM_LEG, true);
	}
	pull_down(m, true);
	fma_log("jack: detection done: %s (%d mV)", mbhc_plug_name(m->plug),
	    m->last_mv);
}

/* one turn of the 3 s loop, every 180 ms */
static void
correct_step(struct mbhc *m)
{
	int mv, cc;
	bool pa_on;

	mv = adc_once(m, MUX_CTL_IN2P);
	m->c_plug = mbhc_plug_from_mv(mv, hph_thres(m), m->c_hs_thr);
	pa_on = rf(m, F_HPH_PA_EN) != 0;
	if (mv > m->c_hs_thr && !m->c_spl_hs) {
		m->c_spl_hs = spl_headset(m);
		mv = adc_once(m, MUX_CTL_IN2P);
		if (m->c_spl_hs)
			m->c_hs_thr = m->c_hs_thr * micbias_mv(m) /
			    m->c_micbias_mv;
	}
	m->last_mv = mv;
	if (mv <= m->c_hs_thr && !pa_on) {
		cc = cross_conn(m);
		if (cc > 0) {
			if (++m->c_swap_cnt < GND_MIC_SWAP_THRESHOLD)
				return;
			m->c_plug = MBHC_PLUG_GND_MIC_SWAP;
		} else {
			if (cc == 0) {
				m->c_swap_cnt = 0;
				m->c_plug = mbhc_plug_from_mv(mv, hph_thres(m),
				    m->c_hs_thr);
			}
			return;
		}
	}
	if (mv > m->c_hs_thr)
		m->c_plug = MBHC_PLUG_HIGH_HPH;
}

/* mbhc_plug_detect_fn on MBHC_SW_DET */
static void
plug_detect(struct mbhc *m)
{
	int det;

	m->in_swch = true;
	m->correcting = false;
	m->long_at = 0;
	det = rf(m, F_MECH_DETECTION_TYPE);
	wf(m, F_MECH_DETECTION_TYPE, !det);
	ramp(m, true);
	if (det) {
		fma_log("jack: something inserted");
		if (m->plug != MBHC_PLUG_NONE)
			goto out;
		wf(m, F_MBHC_BIAS_EN, 1);
		m->is_btn_press = false;
		/* wcd_mbhc_adc_detect_plug_type */
		pull_down(m, false);
		wf(m, F_DETECTION_DONE, 0);
		micb2(m, true);
		correct_start(m);
	} else {
		fma_log("jack: removed");
		wf(m, F_FSM_EN, 0);
		wf(m, F_BTN_ISRC_CTL, 0);
		m->extn_cable_hph_rem = false;
		if (m->plug == MBHC_PLUG_NONE)
			goto out;
		m->is_btn_press = false;
		if (m->plug == MBHC_PLUG_HIGH_HPH)
			wf(m, F_ELECT_ISRC_EN, 0);
		irq_en(m, I_ELECT_INS_REM | I_ELECT_INS_REM_LEG, false);
		wf(m, F_ELECT_DETECTION_TYPE, 1);
		wf(m, F_ELECT_SCHMT_ISRC, 0);
		if (m->buttons != 0) {
			button(m, m->buttons, false);
			m->buttons = 0;
		}
		micbias_disable(m);
		report(m, MBHC_PLUG_NONE);
	}
out:
	m->in_swch = false;
}

/* wcd_mbhc_adc_hs_rem_irq: an extension cable, or a fake removal */
static void
hs_rem(struct mbhc *m)
{
	uint64_t until = m->io->now_ms(m->io->ctx) + WCD_FAKE_REMOVAL_MIN_PERIOD_MS;
	int retry = 0, thr = hs_thres(m), mv;

	do {
		retry++;
		msleep_(m, 10);
		mv = adc_once(m, MUX_CTL_IN2P);
		if (mv <= thr && retry > FAKE_REM_RETRY_ATTEMPTS)
			return;
	} while (m->io->now_ms(m->io->ctx) < until);
	if (m->plug == MBHC_PLUG_HEADPHONE)
		m->extn_cable_hph_rem = true;
	wf(m, F_DETECTION_DONE, 0);
	wf(m, F_ADC_MODE, 0);
	wf(m, F_ADC_EN, 0);
	/* wcd_mbhc_elec_hs_report_unplug */
	m->correcting = false;
	report(m, MBHC_PLUG_HEADPHONE);		/* Linux: LINEOUT */
	irq_en(m, I_ELECT_INS_REM, false);
	wf(m, F_FSM_EN, 0);
	wf(m, F_ELECT_SCHMT_ISRC, 3);
	wf(m, F_ELECT_DETECTION_TYPE, 1);
	irq_en(m, I_ELECT_INS_REM_LEG, true);
	wf(m, F_BTN_ISRC_CTL, 0);
}

/* wcd_mbhc_adc_hs_ins_irq: a "headphone" with a mic after all */
static void
hs_ins(struct mbhc *m)
{
	int retry = WCD_MBHC_FAKE_INS_RETRY;

	if (m->extn_cable_hph_rem) {
		m->extn_cable_hph_rem = false;
		return;
	}
	do {
		if (rf(m, F_IN2P_CLAMP_STATE))
			return;
		msleep_(m, 30);
	} while (--retry);
	if (m->plug == MBHC_PLUG_HEADPHONE) {
		irq_en(m, I_ELECT_INS_REM_LEG, false);
		wf(m, F_DETECTION_DONE, 1);
		report(m, MBHC_PLUG_HEADSET);
	}
}

static void
btn_press(struct mbhc *m)
{
	uint64_t now = m->io->now_ms(m->io->ctx);
	unsigned btn;

	m->long_at = 0;
	m->is_btn_press = true;
	if (now - m->t_report < MBHC_BUTTON_PRESS_THRESHOLD_MIN || m->in_swch ||
	    m->plug != MBHC_PLUG_HEADSET)
		return;
	btn = (unsigned)rf(m, F_BTN_RESULT);
	if (btn > 5)
		return;
	m->buttons |= 1u << btn;
	m->long_at = now + LONG_PRESS_MS;
	m->long_reported = false;
}

static void
btn_release(struct mbhc *m)
{
	if (!m->is_btn_press)
		return;		/* a fake press */
	m->is_btn_press = false;
	if (m->buttons == 0)
		return;
	if (m->long_reported)
		button(m, m->buttons, false);
	else if (!m->in_swch) {
		button(m, m->buttons, true);
		button(m, m->buttons, false);
	}
	m->long_at = 0;
	m->buttons = 0;
}

int
mbhc_init(struct mbhc *m)
{
	unsigned i;

	m->plug = MBHC_PLUG_NONE;
	m->micb_ref = 0;
	m->correcting = false;
	m->buttons = 0;
	m->errors = 0;
	/* wcd937x_set_micbias_data: the device tree's 1.8 V for all */
	wf(m, R_ANA_MICB1, 0x3f, MICB_VOUT_1800);
	wf(m, R_ANA_MICB2, 0x3f, MICB_VOUT_1800);
	wf(m, R_ANA_MICB3, 0x3f, MICB_VOUT_1800);
	/* wcd_mbhc_initialise */
	wf(m, F_HSDET_PULLUP_C, HS_PULLUP_I_2P0_UA);
	wf(m, F_HPHL_PLUG_TYPE, 1);		/* hphl_swh: normally open */
	wf(m, F_GND_PLUG_TYPE, 1);		/* gnd_swh */
	wf(m, F_SW_HPH_LP_100K_TO_GND, 1);
	wf(m, F_HS_L_DET_PULL_UP_COMP, 1);
	wf(m, F_L_DET_EN, 1);
	wf(m, F_INSREM_DBNC, 6);		/* 96 ms */
	wf(m, F_BTN_DBNC, 2);			/* 16 ms */
	wf(m, F_MBHC_BIAS_EN, 1);
	wf(m, F_RCO_EN, 1);
	wf(m, F_HS_VREF, (WCD_MBHC_HS_V_MAX - HS_VREF_MIN_VAL) / 100);
	for (i = 0; i < 8; i++)
		wf(m, R_ANA_MBHC_BTN0 + i, 0xfc, (btn_high_mv[i] * 2 / 25) & 0x3f);
	/*
	 * The interrupts as wcd_mbhc_init and the codec's probe request them:
	 * plug switch, buttons, over-current (and the PDM watchdogs of
	 * register 1) unmasked, the electrical detection masked; whatever
	 * is latched cleared
	 */
	m->irq_enabled = I_SW_DET | I_BTN_PRESS | I_BTN_RELEASE | I_HPHL_OCP |
	    I_HPHR_OCP;
	wr(m, R_DIGITAL_INTR_MASK_0, (uint8_t)~m->irq_enabled);
	wr(m, R_DIGITAL_INTR_MASK_1, 0x1f);
	wr(m, R_DIGITAL_INTR_MASK_2, 0xff);
	wr(m, R_DIGITAL_INTR_CLEAR_0, 0xff);
	wr(m, R_DIGITAL_INTR_CLEAR_0, 0x00);
	m->up = m->errors == 0;
	fma_log("jack: MBHC set up%s", m->up ? "" : " with errors");
	/*
	 * A plug already in at the start: the mechanical detection type
	 * (insertion) gives a switch interrupt once the comparator settles;
	 * the first tick looks.
	 */
	return (m->up ? 0 : -EIO);
}

/* one poll: the latched interrupts, the corrections' and long press' time */
int
mbhc_tick(struct mbhc *m)
{
	uint8_t st = 0;
	uint64_t now;

	if (!m->up)
		return (0);
	if (m->io->read(m->io->ctx, R_DIGITAL_INTR_STATUS_0, &st) != 0) {
		m->errors++;
		return (-EIO);
	}
	m->polls++;
	st &= m->irq_enabled;
	if (st != 0) {
		/* regmap-irq: ack (use_ack, clear_ack), then handle_post_irq */
		wr(m, R_DIGITAL_INTR_CLEAR_0, st);
		wr(m, R_DIGITAL_INTR_CLEAR_0, 0);
		m->irqs++;
		if (st & I_SW_DET)
			plug_detect(m);
		if (st & I_BTN_PRESS)
			btn_press(m);
		if (st & I_BTN_RELEASE)
			btn_release(m);
		if (st & I_ELECT_INS_REM)
			hs_rem(m);
		if (st & I_ELECT_INS_REM_LEG)
			hs_ins(m);
		if (st & (I_HPHL_OCP | I_HPHR_OCP)) {
			fma_log("jack: over-current on %s", st & I_HPHL_OCP ?
			    "HPHL" : "HPHR");
			wf(m, F_OCP_FSM_EN, 0);
			wf(m, F_OCP_FSM_EN, 1);
		}
	}
	now = m->io->now_ms(m->io->ctx);
	if (m->correcting) {
		if (now > m->c_until)
			correct_end(m);
		else if (now >= m->c_next) {
			correct_step(m);
			m->c_next = m->io->now_ms(m->io->ctx) + 180;
		}
	}
	if (m->long_at != 0 && now >= m->long_at && m->plug ==
	    MBHC_PLUG_HEADSET && !m->long_reported) {
		button(m, m->buttons, true);
		m->long_reported = true;
		m->long_at = 0;
	}
	return (st != 0);
}
