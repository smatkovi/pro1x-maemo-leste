/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _SHIM_LINUX_STDDEF_H_
#define	_SHIM_LINUX_STDDEF_H_
#include <stddef.h>
#define	sizeof_field(t, m)	sizeof(((t *)0)->m)
#endif
