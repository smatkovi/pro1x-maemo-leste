/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's devfreq types msm's structures embed. msm_gpu_devfreq.c is not
 * built: msm_freebsd.c's msm_devfreq_*() keep the GPU at the OPP the power
 * up picked (gpu->fast_rate, capped by hw.msm.max_freq). No frequency
 * scaling, no thermal cooling device.
 * (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_DEVFREQ_H
#define	_MSM_LKPI_LINUX_DEVFREQ_H

#include <linux/device.h>
#include <linux/pm_qos.h>

struct devfreq;
struct thermal_cooling_device;

struct devfreq_simple_ondemand_data {
	unsigned int	upthreshold;
	unsigned int	downdifferential;
};

struct devfreq_dev_status {
	unsigned long	total_time;
	unsigned long	busy_time;
	unsigned long	current_frequency;
	void		*private_data;
};

/* dev_pm_qos (msm_gpu_devfreq's boost): a type only */
struct dev_pm_qos_request {
	int		type;
	s32		value;
	struct device	*dev;
};

#endif
