/*-
 * SPDX-License-Identifier: GPL-2.0-only
 * LinuxKPI's linux/pm.h and Linux's SYSTEM_SLEEP_PM_OPS and RUNTIME_PM_OPS
 * initializers (adreno's dev_pm_ops) (freebsd/gpu/msm-port.md)
 */
#ifndef	_MSM_LKPI_LINUX_PM_H
#define	_MSM_LKPI_LINUX_PM_H
#include_next <linux/pm.h>
#ifndef SYSTEM_SLEEP_PM_OPS
#define	SYSTEM_SLEEP_PM_OPS(_suspendfunc, _resumefunc)			\
	.suspend = _suspendfunc, .resume = _resumefunc,			\
	.freeze = _suspendfunc, .thaw = _resumefunc,			\
	.poweroff = _suspendfunc, .restore = _resumefunc,
#endif
#ifndef RUNTIME_PM_OPS
#define	RUNTIME_PM_OPS(_suspendfunc, _resumefunc, _idlefunc)		\
	.runtime_suspend = _suspendfunc, .runtime_resume = _resumefunc,	\
	.runtime_idle = _idlefunc,
#endif
#endif
