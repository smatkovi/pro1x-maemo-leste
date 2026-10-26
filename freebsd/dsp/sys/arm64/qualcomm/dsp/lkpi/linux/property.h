/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's device properties, from the device tree
 * (freebsd/dsp: Linux platform drivers with LinuxKPI, lkpi_platform.c)
 */

#ifndef	_LKPI_PF_LINUX_PROPERTY_H
#define	_LKPI_PF_LINUX_PROPERTY_H

#include <linux/of.h>

static inline int
device_property_read_u32(struct device *dev, const char *name, u32 *val)
{
	return (of_property_read_u32(dev_of_node(dev), name, val));
}

static inline bool
device_property_read_bool(struct device *dev, const char *name)
{
	return (of_property_read_bool(dev_of_node(dev), name));
}

/* the of_device_id the platform driver matched with */
const void	*lkpi_platform_match_data(const struct device *);
#define	device_get_match_data(dev)	lkpi_platform_match_data(dev)
#define	of_device_get_match_data(dev)	lkpi_platform_match_data(dev)

#endif
