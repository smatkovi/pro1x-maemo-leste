/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _SHIM_LINUX_ARRAY_SIZE_H_
#define	_SHIM_LINUX_ARRAY_SIZE_H_
#define	ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))
#endif
