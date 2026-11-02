/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The DSP glue's regulators (freebsd/dsp lkpi/linux/regulator/consumer.h)
 * and msm's single ones: the SM6115's GPU node names no "vdd" nor "vddcx"
 * supply (its CX is a power domain, voted with the OPPs); Linux gives a
 * dummy then, which msm treats as none. (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_REGULATOR_CONSUMER_H
#define	_MSM_LKPI_LINUX_REGULATOR_CONSUMER_H

#include_next <linux/regulator/consumer.h>
#include <linux/err.h>

static inline struct regulator *
devm_regulator_get(struct device *dev, const char *id)
{
	return (ERR_PTR(-ENODEV));
}

static inline int
regulator_enable(struct regulator *r)
{
	return (0);
}

static inline int
regulator_disable(struct regulator *r)
{
	return (0);
}

#endif
