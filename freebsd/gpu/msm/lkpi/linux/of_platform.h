/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's of_find_device_by_node: the platform device msm.ko attached to
 * the node (the GPU, the GMU wrapper), with a reference.
 * (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_OF_PLATFORM_H
#define	_MSM_LKPI_LINUX_OF_PLATFORM_H

#include <linux/of.h>
#include <linux/platform_device.h>

struct platform_device *of_find_device_by_node(struct device_node *);

#endif
