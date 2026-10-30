/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _SHIM_LINUX_BUG_H_
#define	_SHIM_LINUX_BUG_H_
#include <stdio.h>
#define	WARN(c, ...)	({ int _c = !!(c); if (_c) fprintf(stderr, __VA_ARGS__); _c; })
#define	WARN_ON(c)	({ int _c = !!(c); if (_c) fprintf(stderr, "WARN_ON %s\n", #c); _c; })
#define	__ffs(x)	((unsigned long)__builtin_ctzl(x))
#endif
