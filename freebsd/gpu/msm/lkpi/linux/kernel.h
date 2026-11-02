/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * LinuxKPI's linux/kernel.h and Linux's memparse() (msm's vram= size:
 * "16m"), in msm_freebsd.c. (freebsd/gpu/msm-port.md)
 */
#ifndef	_MSM_LKPI_LINUX_KERNEL_H
#define	_MSM_LKPI_LINUX_KERNEL_H
#include_next <linux/kernel.h>
unsigned long long memparse(const char *, char **);
#ifndef in_range
/* Linux's in_range(): val in [start, start + len) */
#define	in_range(val, start, len)					\
	((typeof(start))((val) - (start)) < (typeof(start))(len))
#endif
#ifndef PHYS_ADDR_MAX
#define	PHYS_ADDR_MAX	(~(phys_addr_t)0)
#endif
#endif
