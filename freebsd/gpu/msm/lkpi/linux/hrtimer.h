/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * LinuxKPI's hrtimers (relative, on CLOCK_MONOTONIC, the mode a constant)
 * and what msm needs beyond them: HRTIMER_MODE_ABS deadlines on ktime_get()
 * (msm_fence.c's deadline timer, msm_hrtimer_work), the mode as a variable.
 * An absolute expiry is started as the time left until it.
 * (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_HRTIMER_H
#define	_MSM_LKPI_LINUX_HRTIMER_H

#include_next <linux/hrtimer.h>

#define	HRTIMER_MODE_ABS	((enum hrtimer_mode)1)

static inline ktime_t
lkpi_msm_hrtimer_rel(ktime_t time, enum hrtimer_mode mode)
{
	ktime_t left;

	if (mode != HRTIMER_MODE_ABS)
		return (time);
	left = ktime_sub(time, ktime_get());
	return (ktime_to_ns(left) > 0 ? left : 0);
}

#undef	hrtimer_init
#define	hrtimer_init(hrtimer, clock, mode)	linux_hrtimer_init(hrtimer)
#undef	hrtimer_start
#define	hrtimer_start(hrtimer, time, mode)				\
	linux_hrtimer_start((hrtimer), lkpi_msm_hrtimer_rel((time), (mode)))
#undef	hrtimer_start_range_ns
#define	hrtimer_start_range_ns(hrtimer, time, prec, mode)		\
	linux_hrtimer_start_range_ns((hrtimer),				\
	    lkpi_msm_hrtimer_rel((time), (mode)), (prec))

#endif
