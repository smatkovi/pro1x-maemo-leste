/*-
 * SPDX-License-Identifier: GPL-2.0-only
 * LinuxKPI's IDR and Linux's idr_alloc_u32 (msm's fence ids) on idr_alloc
 * (freebsd/gpu/msm-port.md)
 */
#ifndef	_MSM_LKPI_LINUX_IDR_H
#define	_MSM_LKPI_LINUX_IDR_H
#include_next <linux/idr.h>
static inline int
idr_alloc_u32(struct idr *idr, void *ptr, u32 *nextid, unsigned long max,
    gfp_t gfp)
{
	int id;

	/* idr_alloc's end is exclusive and an int */
	id = idr_alloc(idr, ptr, *nextid,
	    max >= INT_MAX ? 0 : (int)max + 1, gfp);
	if (id < 0)
		return (id);
	*nextid = id;
	return (0);
}
#endif
