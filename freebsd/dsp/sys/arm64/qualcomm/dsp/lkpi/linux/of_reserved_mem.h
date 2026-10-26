/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's reserved memory regions (memory-region)
 * (freebsd/dsp: Linux platform drivers with LinuxKPI, lkpi_platform.c)
 */

#ifndef	_LKPI_PF_LINUX_OF_RESERVED_MEM_H
#define	_LKPI_PF_LINUX_OF_RESERVED_MEM_H

#include <linux/of.h>
#include <linux/ioport.h>

int	lkpi_platform_of_memory_region(uintptr_t, unsigned int, struct resource *);

static inline int
of_reserved_mem_region_to_resource(const struct device_node *np,
    unsigned int idx, struct resource *res)
{
	return (lkpi_platform_of_memory_region(of_node_ptr(np), idx, res));
}

#endif
