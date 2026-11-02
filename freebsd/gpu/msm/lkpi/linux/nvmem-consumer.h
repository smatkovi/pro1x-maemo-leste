/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's nvmem consumer API, for the GPU's speed bin: the node's
 * "nvmem-cells" / "nvmem-cell-names" cell, read from the QFPROM's corrected
 * region as Linux's qfprom driver reads it (bytes at the cell's reg, then
 * its "bits"), in msm_freebsd_bsd.c. hw.msm.speedbin overrides it.
 * (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_NVMEM_CONSUMER_H
#define	_MSM_LKPI_LINUX_NVMEM_CONSUMER_H

#include <linux/device.h>
#include <linux/err.h>

int	nvmem_cell_read_variable_le_u32(struct device *, const char *, u32 *);

#endif
