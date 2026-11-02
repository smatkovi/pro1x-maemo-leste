/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's LLCC (system cache) slices: the SM6115 has none, and a6xx skips
 * them for a GMU wrapper anyway; these fail as Linux's do without LLCC.
 * (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_SOC_QCOM_LLCC_QCOM_H
#define	_MSM_LKPI_LINUX_SOC_QCOM_LLCC_QCOM_H

#include <linux/err.h>

#define	LLCC_GPUHTW	11
#define	LLCC_GPU	12

struct llcc_slice_desc {
	u32	slice_id;
	size_t	slice_size;
};

static inline struct llcc_slice_desc *
llcc_slice_getd(u32 uid)
{
	return (ERR_PTR(-ENODEV));
}

static inline void
llcc_slice_putd(struct llcc_slice_desc *desc)
{
}

static inline int
llcc_get_slice_id(struct llcc_slice_desc *desc)
{
	return (-EINVAL);
}

static inline int
llcc_slice_activate(struct llcc_slice_desc *desc)
{
	return (-EINVAL);
}

static inline int
llcc_slice_deactivate(struct llcc_slice_desc *desc)
{
	return (-EINVAL);
}

#endif
