/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's regulators: the SoC's are the RPM's, which FreeBSD has no
 * driver for; they stay as the boot loader and Linux left them (logged once)
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

static inline int
devm_regulator_bulk_get(struct device *dev, int n,
    struct regulator_bulk_data *r)
{
	return (lkpi_platform_regulators(dev, n, r));
}

static inline int
regulator_bulk_enable(int n, struct regulator_bulk_data *r)
{
	return (0);
}

static inline int
regulator_bulk_disable(int n, struct regulator_bulk_data *r)
{
	return (0);
}

#endif
