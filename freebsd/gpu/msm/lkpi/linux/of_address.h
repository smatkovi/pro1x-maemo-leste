/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's of_address_to_resource: a node's reg, as an IORESOURCE_MEM
 * (msm_freebsd.c). (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_OF_ADDRESS_H
#define	_MSM_LKPI_LINUX_OF_ADDRESS_H

#include <linux/of.h>
#include <linux/ioport.h>

int	of_address_to_resource(struct device_node *, int, struct resource *);

#endif
