/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's of_device_id, which LinuxKPI's linux/mod_devicetable.h lacks, and
 * MODULE_DEVICE_TABLE(of, ...) (no PNP information for devmatch(8) yet)
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

#define	MODULE_DEVICE_TABLE_BUS_of(_bus, _table)

#endif
