/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's component framework, as msm's headless GPU uses it: the GPU
 * (adreno) is a component, the DRM device a master that matches it by its
 * device tree node; once both are there the master binds and binds its
 * components (msm_freebsd.c). One master, few components.
 * (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_COMPONENT_H
#define	_MSM_LKPI_LINUX_COMPONENT_H

#include <linux/device.h>

struct component_match;

struct component_ops {
	int	(*bind)(struct device *comp, struct device *master, void *data);
	void	(*unbind)(struct device *comp, struct device *master,
		    void *data);
};

struct component_master_ops {
	int	(*bind)(struct device *master);
	void	(*unbind)(struct device *master);
};

int	component_add(struct device *, const struct component_ops *);
void	component_del(struct device *, const struct component_ops *);
int	component_bind_all(struct device *master, void *data);
void	component_unbind_all(struct device *master, void *data);
int	component_master_add_with_match(struct device *,
	    const struct component_master_ops *, struct component_match *);
void	component_master_del(struct device *,
	    const struct component_master_ops *);
void	component_match_add_release(struct device *master,
	    struct component_match **matchptr,
	    void (*release)(struct device *, void *),
	    int (*compare)(struct device *, void *), void *compare_data);
int	component_compare_of(struct device *, void *);
void	component_release_of(struct device *, void *);

static inline void
component_match_add(struct device *master, struct component_match **matchptr,
    int (*compare)(struct device *, void *), void *compare_data)
{
	component_match_add_release(master, matchptr, NULL, compare,
	    compare_data);
}

#endif
