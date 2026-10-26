/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's device tree accessors, on FreeBSD's device tree (phandles)
 * (freebsd/dsp: Linux platform drivers with LinuxKPI, lkpi_platform.c)
 */

#ifndef	_LKPI_PF_LINUX_OF_H
#define	_LKPI_PF_LINUX_OF_H

#include_next <linux/of.h>
#include <linux/mod_devicetable.h>
#include <linux/device.h>
#include <linux/err.h>

/* A node is its phandle, as a pointer */
struct device_node;

uintptr_t	lkpi_platform_dev_node(struct device *);
bool	lkpi_platform_of_has(uintptr_t, const char *);
int	lkpi_platform_of_read_u32(uintptr_t, const char *, u32 *);
int	lkpi_platform_of_read_string(uintptr_t, const char *, const char **);
uintptr_t	lkpi_platform_of_child(uintptr_t, const char *);

#define	of_node_ptr(n)		((uintptr_t)(n))

static inline struct device_node *
dev_of_node(struct device *dev)
{
	return ((struct device_node *)lkpi_platform_dev_node(dev));
}

static inline bool
of_property_read_bool(const struct device_node *np, const char *name)
{
	return (lkpi_platform_of_has(of_node_ptr(np), name));
}

static inline int
of_property_read_u32(const struct device_node *np, const char *name, u32 *v)
{
	return (lkpi_platform_of_read_u32(of_node_ptr(np), name, v));
}

static inline int
of_property_read_string(const struct device_node *np, const char *name,
    const char **s)
{
	return (lkpi_platform_of_read_string(of_node_ptr(np), name, s));
}

static inline struct device_node *
of_get_child_by_name(const struct device_node *np, const char *name)
{
	return ((struct device_node *)lkpi_platform_of_child(of_node_ptr(np),
	    name));
}

static inline void
of_node_put(struct device_node *np)
{
}

static inline int
of_dma_configure(struct device *dev, struct device_node *np, bool force)
{
	return (-ENODEV);
}

#endif
