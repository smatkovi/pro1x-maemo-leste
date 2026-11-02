/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * LinuxKPI's DMA API and dma_alloc_attrs/dma_free_attrs, which it lacks.
 * msm allocates its VRAM carveout with DMA_ATTR_NO_KERNEL_MAPPING |
 * DMA_ATTR_WRITE_COMBINE: physically contiguous pages without a VM object
 * (they are mapped into user space page by page by the GEM fault, which
 * LinuxKPI's vmf_insert_pfn() moves into the mapping's object), write
 * combining, addressed physically (the GPU's SMMU is in bypass); other
 * attributes: dma_alloc_coherent() (msm_freebsd.c, msm_freebsd_bsd.c).
 * (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_DMA_MAPPING_H
#define	_MSM_LKPI_LINUX_DMA_MAPPING_H

#include_next <linux/dma-mapping.h>

void	*lkpi_msm_dma_alloc_attrs(struct device *, size_t, dma_addr_t *, gfp_t,
	    unsigned long);
void	lkpi_msm_dma_free_attrs(struct device *, size_t, void *, dma_addr_t,
	    unsigned long);
#define	dma_alloc_attrs(dev, size, handle, gfp, attrs)			\
	lkpi_msm_dma_alloc_attrs((dev), (size), (handle), (gfp), (attrs))
#define	dma_free_attrs(dev, size, vaddr, handle, attrs)			\
	lkpi_msm_dma_free_attrs((dev), (size), (vaddr), (handle), (attrs))

#endif
