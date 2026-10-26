/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's clocks: none here, the SoC's clocks stay as the boot loader
 * and Linux left them (optional clocks are absent)
 * (freebsd/dsp: Linux platform drivers with LinuxKPI, lkpi_platform.c)
 */

#ifndef	_LKPI_PF_LINUX_CLK_H
#define	_LKPI_PF_LINUX_CLK_H

#include <linux/device.h>

struct clk;

struct clk_bulk_data {
	const char	*id;
	struct clk	*clk;
};

static inline int
devm_clk_bulk_get_optional(struct device *dev, int n, struct clk_bulk_data *c)
{
	int i;

	for (i = 0; i < n; i++)
		c[i].clk = NULL;
	return (0);
}

static inline int
clk_bulk_prepare_enable(int n, const struct clk_bulk_data *c)
{
	return (0);
}

static inline void
clk_bulk_disable_unprepare(int n, const struct clk_bulk_data *c)
{
}

#endif
