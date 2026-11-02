/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's AOSS QMP: only a real GMU (not the SM6115's GMU wrapper) talks to
 * it; absent here. (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_SOC_QCOM_QCOM_AOSS_H
#define	_MSM_LKPI_LINUX_SOC_QCOM_QCOM_AOSS_H

#include <linux/device.h>
#include <linux/err.h>

struct qmp;

static inline struct qmp *
qmp_get(struct device *dev)
{
	return (ERR_PTR(-ENODEV));
}

static inline void
qmp_put(struct qmp *qmp)
{
}

static inline int
qmp_send(struct qmp *qmp, const char *fmt, ...)
{
	return (-ENODEV);
}

#endif
