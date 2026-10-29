/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's device tree accessors, on FreeBSD's device tree: a struct
 * device_node per node (made once by lkpi_platform.c and kept), with the
 * node's phandle behind it; dev->of_node is set for platform devices
 * (patches/0031) and NULL for all others
 * (freebsd/dsp: Linux platform drivers with LinuxKPI, lkpi_platform.c)
 */

#ifndef	_LKPI_PF_LINUX_OF_H
#define	_LKPI_PF_LINUX_OF_H

#include_next <linux/of.h>
#include <linux/mod_devicetable.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fwnode.h>

struct device_node {
	struct fwnode_handle	fwnode;
	const char		*name;		/* without the unit address */
	const char		*full_name;
	uintptr_t		lkpi_phandle;	/* FreeBSD's phandle_t */
};

struct device_node *lkpi_platform_of_node(uintptr_t);
bool	lkpi_platform_of_has(uintptr_t, const char *);
int	lkpi_platform_of_read_u32(uintptr_t, const char *, u32 *);
int	lkpi_platform_of_read_string(uintptr_t, const char *, const char **);
const void *lkpi_platform_of_get_property(uintptr_t, const char *, int *);
uintptr_t	lkpi_platform_of_child(uintptr_t, const char *);

#define	of_node_ptr(np)		((np) != NULL ? (np)->lkpi_phandle : 0)

static inline struct device_node *
dev_of_node(struct device *dev)
{
	return (dev->of_node);
}

static inline bool
of_property_read_bool(const struct device_node *np, const char *name)
{
	return (lkpi_platform_of_has(of_node_ptr(np), name));
}

static inline bool
of_property_present(const struct device_node *np, const char *name)
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

/* the property's bytes (kept while the module is loaded) and their count */
static inline const void *
of_get_property(const struct device_node *np, const char *name, int *lenp)
{
	return (lkpi_platform_of_get_property(of_node_ptr(np), name, lenp));
}

/* the index-th string of a list of them */
static inline int
of_property_read_string_index(const struct device_node *np, const char *name,
    int index, const char **out)
{
	const char *p, *end;
	int len;

	if ((p = of_get_property(np, name, &len)) == NULL)
		return (-EINVAL);
	for (end = p + len; p < end; p += strnlen(p, end - p) + 1) {
		if (strnlen(p, end - p) == (size_t)(end - p))
			return (-EILSEQ);	/* not terminated */
		if (index-- == 0) {
			*out = p;
			return (0);
		}
	}
	return (-ENODATA);
}

static inline int
of_property_read_u8_array(const struct device_node *np, const char *name,
    u8 *out, size_t sz)
{
	const void *p;
	int len;

	if ((p = of_get_property(np, name, &len)) == NULL)
		return (-EINVAL);
	if ((size_t)len < sz)
		return (-EOVERFLOW);
	memcpy(out, p, sz);
	return (0);
}

static inline int
of_property_read_u8(const struct device_node *np, const char *name, u8 *out)
{
	return (of_property_read_u8_array(np, name, out, 1));
}

static inline struct device_node *
of_get_child_by_name(const struct device_node *np, const char *name)
{
	return (lkpi_platform_of_node(lkpi_platform_of_child(of_node_ptr(np),
	    name)));
}

static inline struct device_node *
of_node_get(struct device_node *np)
{
	return (np);
}

static inline void
of_node_put(struct device_node *np)
{
}

/* DMA goes to physical addresses here (the SMMU's streams in bypass) */
static inline int
of_dma_configure(struct device *dev, struct device_node *np, bool force)
{
	return (-ENODEV);
}

#endif
