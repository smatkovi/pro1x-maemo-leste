/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's interconnect consumer API: the SM6115's GPU node names no
 * interconnects (Linux's of_icc_get() returns NULL then, and the bandwidth
 * votes are no-ops); the GPU's memory bandwidth stays as the RPM has it.
 * (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_INTERCONNECT_H
#define	_MSM_LKPI_LINUX_INTERCONNECT_H

#include <linux/device.h>
#include <linux/err.h>

struct icc_path;

#define	Bps_to_icc(x)	((x) / 1000)
#define	kBps_to_icc(x)	(x)
#define	MBps_to_icc(x)	((x) * 1000)
#define	GBps_to_icc(x)	((x) * 1000 * 1000)
#define	icc_units_to_bps(bw)	((bw) * 1000ULL)

static inline struct icc_path *
of_icc_get(struct device *dev, const char *name)
{
	return (NULL);
}

static inline struct icc_path *
devm_of_icc_get(struct device *dev, const char *name)
{
	return (NULL);
}

static inline void
icc_put(struct icc_path *path)
{
}

static inline int
icc_set_bw(struct icc_path *path, u32 avg, u32 peak)
{
	return (0);
}

static inline int
icc_enable(struct icc_path *path)
{
	return (0);
}

static inline int
icc_disable(struct icc_path *path)
{
	return (0);
}

#endif
