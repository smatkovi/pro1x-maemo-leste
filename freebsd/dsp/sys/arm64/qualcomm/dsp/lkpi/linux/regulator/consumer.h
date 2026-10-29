/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's regulators for platform devices: a supply is the device tree's
 * <name>-supply; one the device tree lacks is a dummy, as in Linux. The
 * SoC's regulators are the RPM's: they go on and off by votes at the RPM
 * (qcom_dsp's qcom_rpm.c), as Linux's qcom_smd-regulator sends them
 * (freebsd/dsp: Linux platform drivers with LinuxKPI, lkpi_platform.c)
 */

#ifndef	_LKPI_PF_LINUX_REGULATOR_CONSUMER_H
#define	_LKPI_PF_LINUX_REGULATOR_CONSUMER_H

#include_next <linux/regulator/consumer.h>
#include <linux/device.h>

struct regulator;

struct regulator_bulk_data {
	const char		*supply;
	struct regulator	*consumer;
	int			ret;
};

int	lkpi_platform_regulators(struct device *, int, struct regulator_bulk_data *);
int	lkpi_platform_regulator_bulk(int, struct regulator_bulk_data *, bool);

static inline int
devm_regulator_bulk_get(struct device *dev, int n,
    struct regulator_bulk_data *r)
{
	return (lkpi_platform_regulators(dev, n, r));
}

static inline int
regulator_bulk_enable(int n, struct regulator_bulk_data *r)
{
	return (lkpi_platform_regulator_bulk(n, r, true));
}

static inline int
regulator_bulk_disable(int n, struct regulator_bulk_data *r)
{
	return (lkpi_platform_regulator_bulk(n, r, false));
}

#endif
