/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The cameras' power, sequenced as Linux's drivers do it, from what
 * /dev/camss reads out of the device tree (CAMSS_IOC_INFO):
 *
 * - the PM8008 (keyboard I2C bus, 0x08; its regulators at 0x09 =
 *   client->addr + 1, qcom-pm8008.c): out of reset first (reset-gpios,
 *   tlmm 26, active low; Linux's probe drives it deasserted), then 1 ms
 * - a PM8008 LDO n (qcom-pm8008-regulator.c): base 0x4000 + 0x100 * (n - 1),
 *   VSET_LB/UB at +0x40 (mV, little endian), ENABLE at +0x46 bit 7. The
 *   voltage is set only when it is outside the device tree's range, to the
 *   lowest point of the LDO's grid at or above the minimum (NLDO 528 mV +
 *   8 mV steps for LDO 1-2, PLDO 1504 mV + 8 mV for LDO 3-7), as the
 *   regulator core's constraints do it
 * - a regulator-fixed on a PCA9534 pin (gpio-pca953x): output register 1,
 *   configuration register 3 (0 = output); only that pin is changed, the
 *   others (touchscreen rails, caps lock LED) are kept
 * - a sensor (imx582_power_on, s5k4h7_power_on): each rail in the
 *   driver's order with 1-2 ms between, the MCLK, 2-3 ms, the reset
 *   deasserted, 20 ms; off in reverse (reset, MCLK, rails)
 * - the lens (dw9800_power_up): its rails, 5 ms, CTL (0x02) = 0 leaves
 *   power-down, 1 ms; off: CTL = 1, its rails
 */

#include <sys/ioctl.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cam.h"
#include "sensor_tables.h"

#define	PM8008_LDO_BASE(n)	(0x4000 + 0x100 * ((n) - 1))
#define	PM8008_VSET_LB		0x40
#define	PM8008_ENABLE		0x46
#define	 PM8008_ENABLE_BIT	0x80
#define	PCA9534_OUTPUT		1
#define	PCA9534_CONFIG		3

int
cam_open(struct cam *c, const char *path)
{
	memset(c, 0, sizeof(*c));
	if ((c->fd = open(path, O_RDWR)) < 0)
		return (-1);
	if (ioctl(c->fd, CAMSS_IOC_INFO, &c->info) < 0)
		return (-1);
	if (c->info.version != CAMSS_ABI_VERSION) {
		errno = EPROTO;
		return (-1);
	}
	return (0);
}

/* "rear" (orientation 1, back), "front" (0) or an index */
int
cam_find(struct cam *c, const char *name)
{
	char *end;
	long i;
	u_int s;

	i = strtol(name, &end, 10);
	if (*end == '\0' && i >= 0 && i < (long)c->info.nsensors)
		return ((int)i);
	for (s = 0; s < c->info.nsensors; s++) {
		if (strcmp(name, "rear") == 0 &&
		    c->info.sensor[s].orientation == 1)
			return (s);
		if (strcmp(name, "front") == 0 &&
		    c->info.sensor[s].orientation == 0)
			return (s);
		if (strstr(c->info.sensor[s].compatible, name) != NULL)
			return (s);
	}
	return (-1);
}

const char *
cam_name(struct cam *c, int s)
{
	switch (c->info.sensor[s].orientation) {
	case 0:
		return ("front");
	case 1:
		return ("rear");
	default:
		return (c->info.sensor[s].node);
	}
}

static int
gpio(struct cam *c, int pin, int asserted)
{
	struct camss_gpio g = { .gpio = pin, .value = asserted };

	if (pin < 0)
		return (0);
	if (ioctl(c->fd, CAMSS_IOC_GPIO, &g) < 0) {
		warn("gpio%d %s", pin, asserted ? "assert" : "deassert");
		return (-1);
	}
	return (0);
}

static int
pmic_up(struct cam *c)
{
	if (c->pmic_up)
		return (0);
	if (c->info.pmic_reset_gpio >= 0) {
		if (gpio(c, c->info.pmic_reset_gpio, 0) < 0)
			return (-1);
		vmsg("PM8008: reset gpio%d deasserted", c->info.pmic_reset_gpio);
		msleep(1);
	}
	c->pmic_up = 1;
	return (0);
}

static int
pm8008_ldo(struct cam *c, const struct camss_supply *s, int on)
{
	uint16_t base = PM8008_LDO_BASE(s->index);
	uint8_t v[2], en;
	uint8_t a = s->addr + 1;	/* the regulators' "secondary" regmap */
	uint32_t uv, mv;

	if (pmic_up(c) < 0)
		return (-1);
	if (on) {
		if (iic_rd16(s->iic_unit, a, base + PM8008_VSET_LB, v, 2) < 0) {
			warn("PM8008 ldo%u: VSET", s->index);
			return (-1);
		}
		uv = (uint32_t)(v[0] | v[1] << 8) * 1000;
		if (s->min_uv != 0 && (uv < s->min_uv || uv > s->max_uv)) {
			mv = (pm8008_grid_uv(s->index, s->min_uv) + 999) / 1000;
			v[0] = mv & 0xff;
			v[1] = mv >> 8;
			if (iic_wr16(s->iic_unit, a, base + PM8008_VSET_LB, v,
			    2) < 0) {
				warn("PM8008 ldo%u: set %u mV", s->index, mv);
				return (-1);
			}
			msg("PM8008 ldo%u: %u mV -> %u mV (%u-%u uV)", s->index,
			    uv / 1000, mv, s->min_uv, s->max_uv);
		} else
			vmsg("PM8008 ldo%u: %u mV", s->index, uv / 1000);
	}
	if (iic_rd16(s->iic_unit, a, base + PM8008_ENABLE, &en, 1) < 0) {
		warn("PM8008 ldo%u: ENABLE", s->index);
		return (-1);
	}
	en = on ? (en | PM8008_ENABLE_BIT) : (en & ~PM8008_ENABLE_BIT);
	if (iic_wr16(s->iic_unit, a, base + PM8008_ENABLE, &en, 1) < 0) {
		warn("PM8008 ldo%u: %s", s->index, on ? "on" : "off");
		return (-1);
	}
	return (0);
}

static int
pca9534_pin(struct cam *c, const struct camss_supply *s, int on)
{
	uint8_t out, cfg, bit = 1u << s->index;

	if (iic_rd8(s->iic_unit, s->addr, PCA9534_OUTPUT, &out, 1) < 0 ||
	    iic_rd8(s->iic_unit, s->addr, PCA9534_CONFIG, &cfg, 1) < 0) {
		warn("PCA9534 0x%02x", s->addr);
		return (-1);
	}
	vmsg("PCA9534: output %#04x config %#04x", out, cfg);
	if (on == s->active_high)
		out |= bit;
	else
		out &= ~bit;
	if (iic_wr8(s->iic_unit, s->addr, PCA9534_OUTPUT, &out, 1) < 0)
		return (-1);
	if (cfg & bit) {
		cfg &= ~bit;
		if (iic_wr8(s->iic_unit, s->addr, PCA9534_CONFIG, &cfg, 1) < 0)
			return (-1);
	}
	return (0);
}

static int *
ref(struct cam *c, const struct camss_supply *s)
{
	return (&c->rail_ref[s->type < 3 ? s->type : 0][s->index & 15]);
}

int
rail_on(struct cam *c, const struct camss_supply *s)
{
	int error = 0;

	if (s->type == CAMSS_SUPPLY_OTHER) {
		vmsg("rail %s (%s): not switchable here", s->name, s->node);
		return (0);
	}
	if (s->iic_unit < 0) {
		warnx("rail %s (%s): its I2C bus has no /dev/iic", s->name,
		    s->node);
		return (-1);
	}
	if ((*ref(c, s))++ > 0)
		return (0);
	if (s->type == CAMSS_SUPPLY_PM8008)
		error = pm8008_ldo(c, s, 1);
	else
		error = pca9534_pin(c, s, 1);
	if (error)
		(*ref(c, s))--;
	else
		vmsg("rail %s (%s) on", s->name, s->node);
	return (error);
}

int
rail_off(struct cam *c, const struct camss_supply *s)
{
	if (s->type == CAMSS_SUPPLY_OTHER || s->iic_unit < 0 ||
	    *ref(c, s) == 0 || --(*ref(c, s)) > 0)
		return (0);
	vmsg("rail %s (%s) off", s->name, s->node);
	if (s->type == CAMSS_SUPPLY_PM8008)
		return (pm8008_ldo(c, s, 0));
	return (pca9534_pin(c, s, 0));
}

static const struct camss_supply *
supply(const struct camss_supply *list, u_int n, const char *name)
{
	u_int i;

	for (i = 0; i < n; i++)
		if (strcmp(list[i].name, name) == 0)
			return (&list[i]);
	return (NULL);
}

int
sensor_power(struct cam *c, int s, int on)
{
	const struct camss_sensor *cs = &c->info.sensor[s];
	const struct sensor_ops *o = sensor_ops(c, s);
	const struct camss_supply *sp;
	struct camss_mclk m = { .sensor = s };
	int i, n;

	if (o == NULL) {
		warnx("%s: no driver for %s", cs->node, cs->compatible);
		return (-1);
	}
	for (n = 0; o->supplies[n] != NULL; n++)
		;
	if (!on) {
		gpio(c, cs->reset_gpio, 1);
		m.on = 0;
		ioctl(c->fd, CAMSS_IOC_MCLK, &m);
		for (i = n - 1; i >= 0; i--)
			if ((sp = supply(cs->supply, cs->nsupplies,
			    o->supplies[i])) != NULL)
				rail_off(c, sp);
		return (0);
	}
	/* held in reset while the rails come up */
	if (gpio(c, cs->reset_gpio, 1) < 0)
		return (-1);
	for (i = 0; i < n; i++) {
		if ((sp = supply(cs->supply, cs->nsupplies,
		    o->supplies[i])) == NULL) {
			warnx("%s: no %s-supply in the device tree", cs->node,
			    o->supplies[i]);
			goto fail;
		}
		if (rail_on(c, sp) < 0)
			goto fail;
		msleep(2);
	}
	m.on = 1;
	if (ioctl(c->fd, CAMSS_IOC_MCLK, &m) < 0) {
		warn("%s: MCLK", cs->node);
		goto fail;
	}
	vmsg("%s: MCLK%d at %u Hz", cs->node, cs->mclk, m.hz);
	msleep(3);
	if (gpio(c, cs->reset_gpio, 0) < 0)
		goto fail;
	msleep(20);
	return (0);
fail:
	sensor_power(c, s, 0);
	return (-1);
}

int
lens_power(struct cam *c, int s, int on)
{
	const struct camss_sensor *cs = &c->info.sensor[s];
	const struct camss_supply *sp;
	uint8_t v;
	int i, n;

	if (!cs->has_lens)
		return (-1);
	for (n = 0; dw9800_supplies[n] != NULL; n++)
		;
	if (!on) {
		v = 0x01;		/* power down */
		iic_wr8(cs->iic_unit, cs->lens_addr, DW9800_CTL_ADDR, &v, 1);
		for (i = n - 1; i >= 0; i--)
			if ((sp = supply(cs->lens_supply, cs->nlens_supplies,
			    dw9800_supplies[i])) != NULL)
				rail_off(c, sp);
		return (0);
	}
	for (i = 0; i < n; i++) {
		if ((sp = supply(cs->lens_supply, cs->nlens_supplies,
		    dw9800_supplies[i])) == NULL || rail_on(c, sp) < 0) {
			warnx("%s: lens rail %s", cs->lens_node,
			    dw9800_supplies[i]);
			goto fail;
		}
	}
	msleep(6);
	v = 0x00;			/* leave power-down */
	if (iic_wr8(cs->iic_unit, cs->lens_addr, DW9800_CTL_ADDR, &v, 1) < 0) {
		warn("%s: no answer at 0x%02x", cs->lens_node, cs->lens_addr);
		goto fail;
	}
	msleep(2);
	return (0);
fail:
	lens_power(c, s, 0);
	return (-1);
}
