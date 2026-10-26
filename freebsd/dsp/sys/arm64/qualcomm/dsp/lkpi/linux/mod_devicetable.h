/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's of_device_id, which LinuxKPI's linux/mod_devicetable.h lacks
 * (freebsd/dsp: Linux platform drivers with LinuxKPI, lkpi_platform.c)
 */

#ifndef	_LKPI_PF_LINUX_MOD_DEVICETABLE_H
#define	_LKPI_PF_LINUX_MOD_DEVICETABLE_H

#include_next <linux/mod_devicetable.h>

struct of_device_id {
	char		name[32];
	char		type[32];
	char		compatible[128];
	const void	*data;
};

#endif
