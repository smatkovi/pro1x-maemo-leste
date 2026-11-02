/*-
 * SPDX-License-Identifier: GPL-2.0-only
 * LinuxKPI's read_poll_timeout() and Linux's MMIO wrappers on it (msm's
 * a6xx and GMU waits), as in Linux's linux/iopoll.h (freebsd/gpu/msm-port.md)
 */
#ifndef	_MSM_LKPI_LINUX_IOPOLL_H
#define	_MSM_LKPI_LINUX_IOPOLL_H
#include_next <linux/iopoll.h>
#include <linux/io.h>
#ifndef readl_poll_timeout
#define	readx_poll_timeout_atomic(op, addr, val, cond, delay_us, timeout_us) \
	read_poll_timeout_atomic(op, val, cond, delay_us, timeout_us, false, addr)
#define	readl_poll_timeout(addr, val, cond, delay_us, timeout_us)	\
	readx_poll_timeout(readl, addr, val, cond, delay_us, timeout_us)
#define	readl_poll_timeout_atomic(addr, val, cond, delay_us, timeout_us) \
	readx_poll_timeout_atomic(readl, addr, val, cond, delay_us, timeout_us)
#define	readl_relaxed_poll_timeout(addr, val, cond, delay_us, timeout_us) \
	readx_poll_timeout(readl_relaxed, addr, val, cond, delay_us, timeout_us)
#endif
#endif
