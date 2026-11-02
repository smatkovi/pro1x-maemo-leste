/*-
 * SPDX-License-Identifier: GPL-2.0-only
 * LinuxKPI's GFP flags; direct reclaim is not one here (as LinuxKPI's other
 * reclaim flags are 0). (freebsd/gpu/msm-port.md)
 */
#ifndef	_MSM_LKPI_LINUX_GFP_H
#define	_MSM_LKPI_LINUX_GFP_H
#include_next <linux/gfp.h>
#ifndef __GFP_DIRECT_RECLAIM
#define	__GFP_DIRECT_RECLAIM	0
#endif
#endif
