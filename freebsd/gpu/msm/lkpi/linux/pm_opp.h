/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's operating performance points for msm.ko: the GPU node's
 * "operating-points-v2" table (opp-hz, opp-supported-hw against the speed
 * bin, required-opps: the RPM power domain's level for VDDCX), and setting
 * one: the "core" clock's rate and the CX vote (msm_freebsd.c,
 * msm_freebsd_bsd.c), in Linux's order (the level up before the clock goes
 * up, down after it went down). hw.msm.max_freq caps the table.
 * (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_PM_OPP_H
#define	_MSM_LKPI_LINUX_PM_OPP_H

#include <linux/device.h>
#include <linux/err.h>

struct dev_pm_opp;
struct icc_path;

struct dev_pm_opp_config {
	const char * const	*clk_names;
	void			*config_clks;
	const char		*prop_name;
	void			*config_regulators;
	const unsigned int	*supported_hw;
	unsigned int		supported_hw_count;
	const char * const	*regulator_names;
	struct device		*required_dev;
	unsigned int		required_dev_index;
};

int	devm_pm_opp_of_add_table(struct device *);
int	devm_pm_opp_set_supported_hw(struct device *, const u32 *, unsigned int);
int	devm_pm_opp_set_clkname(struct device *, const char *);
int	dev_pm_opp_add(struct device *, unsigned long, unsigned long);
int	dev_pm_opp_get_opp_count(struct device *);
struct dev_pm_opp *dev_pm_opp_find_freq_ceil(struct device *, unsigned long *);
struct dev_pm_opp *dev_pm_opp_find_freq_floor(struct device *, unsigned long *);
struct dev_pm_opp *dev_pm_opp_find_freq_exact(struct device *, unsigned long,
	    bool);
void	dev_pm_opp_put(struct dev_pm_opp *);
unsigned long dev_pm_opp_get_freq(struct dev_pm_opp *);
unsigned int dev_pm_opp_get_level(struct dev_pm_opp *);
int	dev_pm_opp_set_opp(struct device *, struct dev_pm_opp *);
int	dev_pm_opp_set_rate(struct device *, unsigned long);
int	dev_pm_opp_set_config(struct device *, struct dev_pm_opp_config *);
int	devm_pm_opp_set_config(struct device *, struct dev_pm_opp_config *);

/* no interconnects in the SM6115's GPU node */
static inline int
dev_pm_opp_of_find_icc_paths(struct device *dev, void *table)
{
	return (0);
}

static inline unsigned long
dev_pm_opp_get_voltage(struct dev_pm_opp *opp)
{
	return (0);
}

#endif
