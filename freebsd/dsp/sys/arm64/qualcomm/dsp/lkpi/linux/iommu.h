/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's IOMMU API: none here (the SMMU's streams are in bypass,
 * lkpi_platform_bsd.c); what drivers only use for nodes this SoC lacks
 * (freebsd/dsp: Linux platform drivers with LinuxKPI, lkpi_platform.c)
 */

#ifndef	_LKPI_PF_LINUX_IOMMU_H
#define	_LKPI_PF_LINUX_IOMMU_H

#include_next <linux/iommu.h>
#include <linux/err.h>

#define	IOMMU_READ	(1 << 0)
#define	IOMMU_WRITE	(1 << 1)

static inline struct iommu_domain *
iommu_paging_domain_alloc(struct device *dev)
{
	return (ERR_PTR(-ENODEV));
}

static inline void
iommu_domain_free(struct iommu_domain *d)
{
}

static inline int
iommu_attach_device(struct iommu_domain *d, struct device *dev)
{
	return (-ENODEV);
}

static inline void
iommu_detach_device(struct iommu_domain *d, struct device *dev)
{
}

static inline int
iommu_map(struct iommu_domain *d, unsigned long iova, phys_addr_t paddr,
    size_t size, int prot, gfp_t gfp)
{
	return (-ENODEV);
}

static inline size_t
iommu_unmap(struct iommu_domain *d, unsigned long iova, size_t size)
{
	return (0);
}

#endif
