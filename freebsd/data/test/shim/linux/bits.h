/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _SHIM_LINUX_BITS_H_
#define	_SHIM_LINUX_BITS_H_
#define	BIT(n)		(1UL << (n))
#define	GENMASK(h, l)	(((~0UL) << (l)) & (~0UL >> (8 * sizeof(long) - 1 - (h))))
#define	GENMASK_ULL(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#endif
