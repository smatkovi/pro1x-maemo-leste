/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The DSP glue's device tree accessors (freebsd/dsp lkpi/linux/of.h) and
 * what msm uses beyond them: phandles, compatible and status checks, the
 * first node matching a table, the machine's compatible (msm_freebsd.c on
 * msm_freebsd_bsd.c's FreeBSD side).
 * (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_OF_H
#define	_MSM_LKPI_LINUX_OF_H

#include_next <linux/of.h>

struct device_node *of_parse_phandle(const struct device_node *, const char *,
	    int);
int	of_device_is_compatible(const struct device_node *, const char *);
bool	of_device_is_available(const struct device_node *);
struct device_node *of_find_matching_node(struct device_node *,
	    const struct of_device_id *);
struct device_node *of_find_compatible_node(struct device_node *,
	    const char *, const char *);
int	of_machine_is_compatible(const char *);
int	of_property_count_elems_of_size(const struct device_node *,
	    const char *, int);
int	of_property_read_u32_index(const struct device_node *, const char *,
	    u32, u32 *);
int	of_property_read_u64(const struct device_node *, const char *, u64 *);

static inline const struct of_device_id *
of_match_device(const struct of_device_id *matches, const struct device *dev)
{
	for (; matches != NULL && matches->compatible[0] != '\0'; matches++)
		if (of_device_is_compatible(dev->of_node, matches->compatible))
			return (matches);
	return (NULL);
}

static inline int
of_device_compatible_match(const struct device_node *np,
    const char *const *compat)
{
	for (; compat != NULL && *compat != NULL; compat++)
		if (of_device_is_compatible(np, *compat))
			return (1);
	return (0);
}

static inline bool
of_node_name_eq(const struct device_node *np, const char *name)
{
	return (np != NULL && strcmp(np->name, name) == 0);
}

#endif
