/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The DSP glue's IOMMU stubs (freebsd/dsp lkpi/linux/iommu.h) and what msm
 * names beyond them. No device is behind a Linux IOMMU here:
 * device_iommu_mapped() is false, so msm_iommu_new() (msm_freebsd.c) is not
 * reached and msm falls back to its VRAM carveout (decision A,
 * freebsd/gpu/msm-port.md).
 */

#ifndef	_MSM_LKPI_LINUX_IOMMU_H
#define	_MSM_LKPI_LINUX_IOMMU_H

#include_next <linux/iommu.h>

#ifndef IOMMU_CACHE
#define	IOMMU_CACHE	(1 << 2)
#endif
#define	IOMMU_NOEXEC	(1 << 3)
#define	IOMMU_MMIO	(1 << 4)
#define	IOMMU_PRIV	(1 << 5)

#define	IOMMU_FAULT_READ	0x0
#define	IOMMU_FAULT_WRITE	0x1

enum iommu_cap {
	IOMMU_CAP_CACHE_COHERENCY,
	IOMMU_CAP_NOEXEC,
	IOMMU_CAP_PRE_BOOT_PROTECTION,
	IOMMU_CAP_ENFORCE_CACHE_COHERENCY,
	IOMMU_CAP_DEFERRED_FLUSH,
	IOMMU_CAP_DIRTY_TRACKING,
};

struct iommu_domain_geometry {
	dma_addr_t	aperture_start;
	dma_addr_t	aperture_end;
	bool		force_aperture;
};

typedef int (*iommu_fault_handler_t)(struct iommu_domain *, struct device *,
    unsigned long, int, void *);

/* device_iommu_mapped(): LinuxKPI's linux/device.h, always false */

static inline bool
device_iommu_capable(struct device *dev, enum iommu_cap cap)
{
	return (false);
}

#endif
