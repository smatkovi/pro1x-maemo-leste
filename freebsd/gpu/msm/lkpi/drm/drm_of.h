/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * drm_of_component_match_add (Linux's drm_of.h), on msm.ko's component
 * framework (lkpi/linux/component.h). (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_DRM_DRM_OF_H
#define	_MSM_LKPI_DRM_DRM_OF_H

#include <linux/component.h>
#include <linux/of.h>

static inline void
drm_of_component_match_add(struct device *master,
    struct component_match **matchptr,
    int (*compare)(struct device *, void *), struct device_node *node)
{
	of_node_get(node);
	component_match_add_release(master, matchptr, component_release_of,
	    compare, node);
}

#endif
