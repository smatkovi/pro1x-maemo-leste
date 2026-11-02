/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * msm's generated register headers (gen_header.py) use assert() for array
 * indices: <linux/bug.h>'s BUG_ON under __KERNEL__, else <assert.h>.
 * FreeBSD's kernel build defines no __KERNEL__: this is the kernel's
 * KASSERT (checked with INVARIANTS). (freebsd/gpu/msm-port.md)
 */
#ifndef	_MSM_LKPI_ASSERT_H
#define	_MSM_LKPI_ASSERT_H
#include <sys/param.h>
#include <sys/systm.h>
#define	assert(x)	KASSERT((x), ("msm: assertion %s failed", #x))
#endif
