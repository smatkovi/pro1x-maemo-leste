/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * LinuxKPI's capable() and CAP_SYS_RAWIO, which msm asks of a process that
 * submits without per-process page tables (msm_gem_submit.c; msm.ko has
 * none: its VRAM carveout, the GPU's SMMU in bypass, so a client's command
 * stream can make the GPU read and write all of memory): FreeBSD's PRIV_IO
 * as for raw I/O (root outside a jail), or anyone with
 * hw.msm.unprivileged_submit=1, which a desktop session as a user needs
 * and which gives every user who can open /dev/dri/renderD128 that power.
 * (freebsd/gpu/msm-port.md)
 */
#ifndef	_MSM_LKPI_LINUX_CAPABILITY_H
#define	_MSM_LKPI_LINUX_CAPABILITY_H
#include_next <linux/capability.h>
#ifndef CAP_SYS_RAWIO
#define	CAP_SYS_RAWIO	PRIV_IO
#endif
bool	lkpi_msm_rawio_capable(void);
#define	capable(cap)							\
	((cap) == CAP_SYS_RAWIO ? lkpi_msm_rawio_capable() :		\
	    (priv_check(curthread, (cap)) == 0))
#endif
