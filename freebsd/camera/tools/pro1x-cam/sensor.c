/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The two sensors and the rear lens over the CCI (/dev/iicN), as Linux's
 * imx582.c, s5k4h7.c and dw9800.c drive them: the register sequences and
 * modes are theirs (sensor_tables.h, generated from them), so is the
 * order at stream start: the global ("init") sequence, the mode's, the
 * controls (frame length = the mode's VTS, exposure, analogue gain), then
 * MODE_SELECT (0x0100) = 1. 16-bit registers are written big-endian, one
 * register a transfer (cci_multi_reg_write).
 */

#include <err.h>
#include <string.h>

#include "cam.h"
#include "sensor_tables.h"

static int
wr8(struct cam *c, int s, uint16_t reg, uint8_t v)
{
	const struct camss_sensor *cs = &c->info.sensor[s];

	return (iic_wr16(cs->iic_unit, cs->addr, reg, &v, 1));
}

static int
wr16(struct cam *c, int s, uint16_t reg, uint16_t v)
{
	const struct camss_sensor *cs = &c->info.sensor[s];
	uint8_t b[2] = { v >> 8, v & 0xff };

	return (iic_wr16(cs->iic_unit, cs->addr, reg, b, 2));
}

int
sensor_write_regs(struct cam *c, int s, const void *regs, u_int n)
{
	const struct reg8 *r = regs;
	u_int i;

	for (i = 0; i < n; i++)
		if (wr8(c, s, r[i].reg, r[i].val) < 0) {
			warn("%s: register %#06x", c->info.sensor[s].node,
			    r[i].reg);
			return (-1);
		}
	return (0);
}

/* a register of width 8 or 16 bits, read or written */
int
sensor_reg(struct cam *c, int s, uint16_t reg, int width, int write,
    uint32_t *val)
{
	const struct camss_sensor *cs = &c->info.sensor[s];
	uint8_t b[2];

	if (write)
		return (width == 16 ? wr16(c, s, reg, *val) :
		    wr8(c, s, reg, *val));
	if (iic_rd16(cs->iic_unit, cs->addr, reg, b, width / 8) < 0)
		return (-1);
	*val = width == 16 ? (uint32_t)(b[0] << 8 | b[1]) : b[0];
	return (0);
}

int
sensor_read_id(struct cam *c, int s, uint16_t *id)
{
	const struct sensor_ops *o = sensor_ops(c, s);
	uint32_t v;

	if (o == NULL || sensor_reg(c, s, o->id_reg, 16, 0, &v) < 0)
		return (-1);
	*id = v;
	return (0);
}

/* --- IMX582 ------------------------------------------------------------------ */

static int
imx582_exposure(struct cam *c, int s, const struct sensor_mode *m, u_int lines,
    u_int gain)
{
	if (wr16(c, s, IMX582_FRAME_LENGTH_REG, m->vts_def) < 0 ||
	    wr16(c, s, IMX582_EXP_REG, lines) < 0 ||
	    wr16(c, s, IMX582_AGAIN_REG, gain) < 0)
		return (-1);
	return (0);
}

static int
imx582_start(struct cam *c, int s, const struct sensor_mode *m)
{
	if (sensor_write_regs(c, s, imx582_global_regs,
	    nitems(imx582_global_regs)) < 0 ||
	    sensor_write_regs(c, s, m->regs, m->nregs) < 0)
		return (-1);
	/* the controls' defaults: VBLANK = vts_def - height, gain minimum,
	 * exposure maximum (vts_def - IMX582_EXP_MARGIN) */
	if (imx582_exposure(c, s, m, m->vts_def - IMX582_EXP_MARGIN,
	    IMX582_AGAIN_MIN) < 0)
		return (-1);
	return (wr8(c, s, IMX582_STREAMING_REG, 1));
}

static int
imx582_stop(struct cam *c, int s)
{
	return (wr8(c, s, IMX582_STREAMING_REG, 0));
}

/* --- S5K4H7 ------------------------------------------------------------------ */

static int
s5k4h7_exposure(struct cam *c, int s, const struct sensor_mode *m, u_int lines,
    u_int gain)
{
	if (wr16(c, s, S5K4H7_FRAME_LENGTH_REG, m->vts_def) < 0 ||
	    wr16(c, s, S5K4H7_EXP_REG, lines) < 0 ||
	    wr16(c, s, S5K4H7_AGAIN_REG, gain) < 0)
		return (-1);
	return (0);
}

static int
s5k4h7_start(struct cam *c, int s, const struct sensor_mode *m)
{
	if (sensor_write_regs(c, s, s5k4h7_init_regs,
	    nitems(s5k4h7_init_regs)) < 0 ||
	    sensor_write_regs(c, s, m->regs, m->nregs) < 0)
		return (-1);
	/* the controls' defaults: VBLANK, gain minimum, exposure maximum */
	if (s5k4h7_exposure(c, s, m, m->vts_def - S5K4H7_EXP_MARGIN,
	    S5K4H7_AGAIN_MIN) < 0)
		return (-1);
	return (wr8(c, s, S5K4H7_STREAMING_REG, 1));
}

static int
s5k4h7_stop(struct cam *c, int s)
{
	return (wr8(c, s, S5K4H7_STREAMING_REG, 0));
}

static const struct sensor_ops ops[] = {
	{ "sony,imx582", "IMX582", imx582_supplies, IMX582_REG_CHIP_ID,
	  IMX582_CHIP_ID, imx582_modes, nitems(imx582_modes),
	  imx582_link_freqs, IMX582_BAYER, imx582_start, imx582_stop,
	  imx582_exposure, IMX582_EXP_MIN, IMX582_EXP_MARGIN,
	  IMX582_AGAIN_MIN, IMX582_AGAIN_MAX },
	{ "samsung,s5k4h7", "S5K4H7", s5k4h7_supplies, S5K4H7_REG_CHIP_ID,
	  S5K4H7_CHIP_ID, s5k4h7_modes, nitems(s5k4h7_modes),
	  s5k4h7_link_freqs, S5K4H7_BAYER, s5k4h7_start, s5k4h7_stop,
	  s5k4h7_exposure, S5K4H7_EXP_MIN, S5K4H7_EXP_MARGIN,
	  S5K4H7_AGAIN_MIN, S5K4H7_AGAIN_MAX },
};

const struct sensor_ops *
sensor_ops(struct cam *c, int s)
{
	u_int i;

	for (i = 0; i < nitems(ops); i++)
		if (strcmp(c->info.sensor[s].compatible, ops[i].compatible) == 0)
			return (&ops[i]);
	return (NULL);
}

/* a mode by name ("4000x3000"), or the first */
const struct sensor_mode *
sensor_mode(const struct sensor_ops *o, const char *name)
{
	u_int i;

	if (name == NULL)
		return (&o->modes[0]);
	for (i = 0; i < o->nmodes; i++)
		if (strcmp(o->modes[i].name, name) == 0)
			return (&o->modes[i]);
	return (NULL);
}

/* --- the lens: DW9800 --------------------------------------------------------- */

int
lens_status(struct cam *c, int s, uint8_t *st)
{
	const struct camss_sensor *cs = &c->info.sensor[s];

	return (iic_rd8(cs->iic_unit, cs->lens_addr, DW9800_STATUS_ADDR, st, 1));
}

/* dw9800_set_dac(): wait until not busy, then DAC[9:8], DAC[7:0] */
static int
lens_dac(struct cam *c, int s, u_int pos)
{
	const struct camss_sensor *cs = &c->info.sensor[s];
	uint8_t st, b[2] = { (pos >> 8) & 0x03, pos & 0xff };
	int i;

	for (i = 0; i < DW9800_MAX_RETRY; i++) {
		if (lens_status(c, s, &st) < 0)
			return (-1);
		if ((st & 1) == 0)
			break;
		msleep(1);
	}
	if (i == DW9800_MAX_RETRY)
		warnx("%s: VCM busy", cs->lens_node);
	return (iic_wr8(cs->iic_unit, cs->lens_addr, DW9800_MSB_ADDR, b, 2));
}

/* in DW9800_CTRL_STEPS steps of 1 ms, as the driver's resume and suspend */
int
lens_move(struct cam *c, int s, int from, int to)
{
	int p;

	if (to < 0)
		to = 0;
	if (to > DW9800_MAX_FOCUS_POS)
		to = DW9800_MAX_FOCUS_POS;
	if (from < 0 || from == to)
		return (lens_dac(c, s, to));
	for (p = from; p != to; ) {
		if (p < to)
			p = p + DW9800_CTRL_STEPS > to ? to : p + DW9800_CTRL_STEPS;
		else
			p = p - DW9800_CTRL_STEPS < to ? to : p - DW9800_CTRL_STEPS;
		if (lens_dac(c, s, p) < 0)
			return (-1);
		msleep(DW9800_CTRL_DELAY_US / 1000);
	}
	return (0);
}
