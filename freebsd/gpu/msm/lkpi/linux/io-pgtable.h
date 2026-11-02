/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's io-pgtable types adreno-smmu-priv.h and msm name. msm.ko has no
 * IOMMU page tables (decision A, freebsd/gpu/msm-port.md): the GPU's SMMU
 * is in bypass, buffers come from the VRAM carveout by physical address.
 */

#ifndef	_MSM_LKPI_LINUX_IO_PGTABLE_H
#define	_MSM_LKPI_LINUX_IO_PGTABLE_H

#include <linux/types.h>
#include <linux/bitops.h>

#define	IO_PGTABLE_QUIRK_ARM_NS			BIT(0)
#define	IO_PGTABLE_QUIRK_NO_PERMS		BIT(1)
#define	IO_PGTABLE_QUIRK_ARM_MTK_EXT		BIT(3)
#define	IO_PGTABLE_QUIRK_ARM_TTBR1		BIT(5)
#define	IO_PGTABLE_QUIRK_ARM_OUTER_WBWA		BIT(6)

struct io_pgtable_cfg {
	unsigned long	quirks;
	unsigned long	pgsize_bitmap;
	unsigned int	ias;
	unsigned int	oas;
	bool		coherent_walk;
	const void	*tlb;
	struct device	*iommu_dev;
	struct {
		u64	ttbr;
		struct {
			u32	ips:3;
			u32	tg:2;
			u32	sh:2;
			u32	orgn:2;
			u32	irgn:2;
			u32	tsz:6;
		} tcr;
		u64	mair;
	} arm_lpae_s1_cfg;
};

struct iommu_iotlb_gather;

struct iommu_flush_ops {
	void	(*tlb_flush_all)(void *cookie);
	void	(*tlb_flush_walk)(unsigned long iova, size_t size,
		    size_t granule, void *cookie);
	void	(*tlb_add_page)(struct iommu_iotlb_gather *gather,
		    unsigned long iova, size_t granule, void *cookie);
};

#endif
