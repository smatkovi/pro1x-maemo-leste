/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's power sequencers: none here (drivers fall back to their
 * regulators, as for device trees without one)
 * (freebsd/dsp: Linux platform drivers with LinuxKPI, lkpi_platform.c)
 */

#ifndef	_LKPI_PF_LINUX_PWRSEQ_CONSUMER_H
#define	_LKPI_PF_LINUX_PWRSEQ_CONSUMER_H

#include <linux/device.h>
#include <linux/err.h>

struct pwrseq_desc;

static inline struct pwrseq_desc *
devm_pwrseq_get(struct device *dev, const char *target)
{
	return (ERR_PTR(-EPROBE_DEFER));
}

static inline int
pwrseq_power_on(struct pwrseq_desc *d)
{
	return (0);
}

static inline int
pwrseq_power_off(struct pwrseq_desc *d)
{
	return (0);
}

#endif
