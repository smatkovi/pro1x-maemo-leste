/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's OCMEM (a3xx/a4xx graphics memory): none on the SM6115.
 * (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_SOC_QCOM_OCMEM_H
#define	_MSM_LKPI_LINUX_SOC_QCOM_OCMEM_H

#include <linux/device.h>
#include <linux/err.h>

struct ocmem;

enum ocmem_client {
	OCMEM_GRAPHICS = 0x0,
};

struct ocmem_buf {
	unsigned long	offset;
	unsigned long	addr;
	unsigned long	len;
};

static inline struct ocmem *
of_get_ocmem(struct device *dev)
{
	return (ERR_PTR(-ENODEV));
}

static inline struct ocmem_buf *
ocmem_allocate(struct ocmem *ocmem, enum ocmem_client client,
    unsigned long size)
{
	return (ERR_PTR(-ENODEV));
}

static inline void
ocmem_free(struct ocmem *ocmem, enum ocmem_client client,
    struct ocmem_buf *buf)
{
}

#endif
