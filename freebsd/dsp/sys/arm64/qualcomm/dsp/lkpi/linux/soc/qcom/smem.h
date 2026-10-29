/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's qcom_smem_get, on qcom_dsp's: an ERR_PTR when the item is absent
 * (freebsd/dsp: Linux platform drivers with LinuxKPI, lkpi_platform.c)
 */

#ifndef	_LKPI_PF_LINUX_SOC_QCOM_SMEM_H
#define	_LKPI_PF_LINUX_SOC_QCOM_SMEM_H

#include <linux/types.h>
#include <linux/err.h>

#define	QCOM_SMEM_HOST_ANY	-1

void	*lkpi_platform_smem_get(int, unsigned int, size_t *);
#define	qcom_smem_get(host, item, size)	lkpi_platform_smem_get((host), (item), (size))

#endif
