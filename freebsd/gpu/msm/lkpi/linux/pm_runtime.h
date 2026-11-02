/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's runtime PM for msm.ko, instead of LinuxKPI's stubs: msm powers
 * the GPU up and down through it (adreno_runtime_resume/suspend, the GMU
 * wrapper's device with its CX power domain linked, the GX domain's virtual
 * device). A usage count per device, the driver's runtime callbacks (a
 * platform driver's dev_pm_ops, a power domain's on/off), device links
 * (DL_FLAG_PM_RUNTIME: the supplier is resumed first), autosuspend after
 * the delay on a work queue; all in msm_freebsd.c. Synchronous: the async
 * variants (pm_runtime_get, pm_runtime_put) run their callbacks in the
 * caller, which msm does not hold locks against.
 * (freebsd/gpu/msm-port.md)
 */

#ifndef	_LINUXKPI_LINUX_PM_RUNTIME_H_
#define	_LINUXKPI_LINUX_PM_RUNTIME_H_

#include <linux/device.h>
#include <linux/pm.h>

int	lkpi_pm_runtime_get(struct device *, bool resume);
int	lkpi_pm_runtime_put(struct device *, int how);
#define	LKPI_RPM_PUT_IDLE	0	/* idle check, suspend now */
#define	LKPI_RPM_PUT_AUTO	1	/* after the autosuspend delay */
#define	LKPI_RPM_PUT_NOIDLE	2	/* count only */
#define	LKPI_RPM_PUT_SUSPEND	3	/* suspend now, even with autosuspend */
void	lkpi_pm_runtime_enable(struct device *, bool);
bool	lkpi_pm_runtime_enabled(struct device *);
bool	lkpi_pm_runtime_active(struct device *);
void	lkpi_pm_runtime_autosuspend(struct device *, int delay_ms, int use);
int	lkpi_pm_runtime_force(struct device *, bool resume);
void	lkpi_pm_runtime_set_active(struct device *, bool);

#define	pm_runtime_enable(d)		lkpi_pm_runtime_enable((d), true)
#define	pm_runtime_disable(d)		lkpi_pm_runtime_enable((d), false)
#define	pm_runtime_enabled(d)		lkpi_pm_runtime_enabled(d)
#define	pm_runtime_active(d)		lkpi_pm_runtime_active(d)
#define	pm_runtime_suspended(d)		(!lkpi_pm_runtime_active(d))
#define	pm_runtime_get_sync(d)		lkpi_pm_runtime_get((d), true)
#define	pm_runtime_get(d)		lkpi_pm_runtime_get((d), true)
#define	pm_runtime_get_noresume(d)	((void)lkpi_pm_runtime_get((d), false))
#define	pm_runtime_put(d)		lkpi_pm_runtime_put((d), LKPI_RPM_PUT_IDLE)
#define	pm_runtime_put_sync(d)		lkpi_pm_runtime_put((d), LKPI_RPM_PUT_IDLE)
#define	pm_runtime_put_autosuspend(d)	lkpi_pm_runtime_put((d), LKPI_RPM_PUT_AUTO)
#define	pm_runtime_put_noidle(d)	((void)lkpi_pm_runtime_put((d), LKPI_RPM_PUT_NOIDLE))
#define	pm_runtime_put_sync_suspend(d)	lkpi_pm_runtime_put((d), LKPI_RPM_PUT_SUSPEND)
#define	pm_runtime_set_autosuspend_delay(d, ms)				\
	lkpi_pm_runtime_autosuspend((d), (ms), -1)
#define	pm_runtime_use_autosuspend(d)	lkpi_pm_runtime_autosuspend((d), -1, 1)
#define	pm_runtime_dont_use_autosuspend(d)				\
	lkpi_pm_runtime_autosuspend((d), -1, 0)
#define	pm_runtime_force_suspend(d)	lkpi_pm_runtime_force((d), false)
#define	pm_runtime_force_resume(d)	lkpi_pm_runtime_force((d), true)
#define	pm_runtime_set_active(d)	lkpi_pm_runtime_set_active((d), true)
#define	pm_runtime_set_suspended(d)	lkpi_pm_runtime_set_active((d), false)
#define	pm_runtime_mark_last_busy(d)	((void)(d))
#define	pm_runtime_allow(d)		((void)(d))
#define	pm_runtime_forbid(d)		((void)(d))
#define	pm_runtime_irq_safe(d)		((void)(d))

static inline int
pm_runtime_resume_and_get(struct device *dev)
{
	int ret;

	ret = lkpi_pm_runtime_get(dev, true);
	if (ret < 0) {
		lkpi_pm_runtime_put(dev, LKPI_RPM_PUT_NOIDLE);
		return (ret);
	}
	return (0);
}

static inline int
pm_runtime_resume(struct device *dev)
{
	int ret = lkpi_pm_runtime_get(dev, true);

	lkpi_pm_runtime_put(dev, LKPI_RPM_PUT_NOIDLE);
	return (ret < 0 ? ret : 0);
}

static inline int
pm_runtime_get_if_in_use(struct device *dev)
{
	if (!lkpi_pm_runtime_active(dev))
		return (0);
	lkpi_pm_runtime_get(dev, false);
	return (1);
}

static inline int
pm_runtime_get_if_active(struct device *dev)
{
	return (pm_runtime_get_if_in_use(dev));
}

#endif	/* _LINUXKPI_LINUX_PM_RUNTIME_H_ */
