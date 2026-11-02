/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's OF graph (the display's ports and endpoints): msm uses it only
 * for KMS components, which msm.ko has not; a node here has no endpoints.
 * (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_OF_GRAPH_H
#define	_MSM_LKPI_LINUX_OF_GRAPH_H

#include <linux/of.h>

struct of_endpoint {
	unsigned int			port;
	unsigned int			id;
	const struct device_node	*local_node;
};

#define	for_each_endpoint_of_node(parent, child)			\
	for ((child) = NULL; (child) != NULL; )

static inline int
of_graph_parse_endpoint(const struct device_node *node,
    struct of_endpoint *endpoint)
{
	return (-EINVAL);
}

static inline struct device_node *
of_graph_get_remote_port_parent(const struct device_node *node)
{
	return (NULL);
}

static inline struct device_node *
of_graph_get_remote_node(const struct device_node *node, u32 port, u32 ep)
{
	return (NULL);
}

#endif
