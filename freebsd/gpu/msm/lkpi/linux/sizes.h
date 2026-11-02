/*-
 * SPDX-License-Identifier: GPL-2.0-only
 * LinuxKPI's sizes, with the ones of 2 GiB and more as 64-bit constants
 * (LinuxKPI's SZ_2G.. are int products that overflow: SZ_4G is 0, which
 * adreno's private address space size would be), and those up to 256 GiB
 * a6xx's catalog names (freebsd/gpu/msm-port.md)
 */
#ifndef	_MSM_LKPI_LINUX_SIZES_H
#define	_MSM_LKPI_LINUX_SIZES_H
#include_next <linux/sizes.h>
#undef	SZ_2G
#define	SZ_2G	(2ULL << 30)
#undef	SZ_4G
#define	SZ_4G	(4ULL << 30)
#undef	SZ_8G
#define	SZ_8G	(8ULL << 30)
#undef	SZ_16G
#define	SZ_16G	(16ULL << 30)
#undef	SZ_32G
#define	SZ_32G	(32ULL << 30)
#undef	SZ_64G
#define	SZ_64G	(64ULL << 30)
#undef	SZ_128G
#define	SZ_128G	(128ULL << 30)
#undef	SZ_256G
#define	SZ_256G	(256ULL << 30)
#endif
