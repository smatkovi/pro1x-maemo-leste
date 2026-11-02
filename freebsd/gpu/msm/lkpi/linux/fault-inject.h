/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's fault injection (msm's fail_gem_alloc, fail_gem_iova): never.
 * (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_FAULT_INJECT_H
#define	_MSM_LKPI_LINUX_FAULT_INJECT_H

struct fault_attr {
	int	unused;
};

#define	DECLARE_FAULT_ATTR(name)	struct fault_attr name = { 0 }
#define	should_fail(attr, size)		(false)
#define	fault_create_debugfs_attr(name, parent, attr)	(NULL)

#endif
