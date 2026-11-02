/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The DSP glue's platform devices (freebsd/dsp lkpi/linux/platform_device.h)
 * with msm.ko's registration in front (msm_freebsd.c): the driver's
 * dev_pm_ops reach the device (dev->driver, for runtime PM), the GMU
 * wrapper's driver comes before adreno's, and a driver without a device
 * tree match ("msm", the headless DRM device) gets its device made.
 * (freebsd/gpu/msm-port.md)
 */
#ifndef	_MSM_LKPI_LINUX_PLATFORM_DEVICE_H
#define	_MSM_LKPI_LINUX_PLATFORM_DEVICE_H
#include_next <linux/platform_device.h>
int	lkpi_msm_platform_driver_register(struct platform_driver *);
void	lkpi_msm_platform_driver_unregister(struct platform_driver *);
#undef	platform_driver_register
#undef	platform_driver_unregister
#define	platform_driver_register(d)	lkpi_msm_platform_driver_register(d)
#define	platform_driver_unregister(d)	lkpi_msm_platform_driver_unregister(d)
#endif
