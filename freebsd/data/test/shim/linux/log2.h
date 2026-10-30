/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _SHIM_LINUX_LOG2_H_
#define	_SHIM_LINUX_LOG2_H_
#define	is_power_of_2(n)	((n) != 0 && (((n) & ((n) - 1)) == 0))
#endif
