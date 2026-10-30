/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The simulation's device tree: a flattened device tree blob (the Pro1X's,
 * or sim/pro1x-dsp.dts compiled by dtc) read into nodes, with FreeBSD's
 * OF_* and ofw_bus_* functions as the drivers use them: node handles are
 * indices from 1, cells come back in host order (OF_getencprop), "name"
 * is the node's name with its unit address (FreeBSD's ofw_fdt), an xref
 * is a node's phandle property (OF_node_from_xref of an unknown xref is
 * the xref itself, as in FreeBSD).
 */

#include "simkern.h"
#include "sim.h"

#undef malloc
#undef free

#define	FDT_MAGIC	0xd00dfeed
#define	FDT_BEGIN_NODE	1
#define	FDT_END_NODE	2
#define	FDT_PROP	3
#define	FDT_NOP		4
#define	FDT_END		9

struct prop {
	const char	*name;
	const uint8_t	*data;
	uint32_t	len;
};

struct node {
	const char	*name;
	int		parent, child, peer;	/* indices, 0: none */
	struct prop	*props;
	int		nprops;
	uint32_t	phandle;
};

static struct node *nodes;	/* [0] unused */
static int nnodes;
static uint8_t *blob;

static uint32_t
be32(const void *p)
{
	const uint8_t *b = p;

	return ((uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
}

int
sim_fdt_load(const char *path)
{
	FILE *f;
	long size;
	uint32_t off_struct, off_strings, *p, *end, tok;
	const char *strings;
	int stack[64], depth = 0, cur = 0, n, last_child[64];
	struct node *nd;

	if ((f = fopen(path, "rb")) == NULL)
		return (errno);
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	blob = malloc(size);
	if (fread(blob, 1, size, f) != (size_t)size) {
		fclose(f);
		return (EIO);
	}
	fclose(f);
	if (be32(blob) != FDT_MAGIC)
		return (EINVAL);
	off_struct = be32(blob + 8);
	off_strings = be32(blob + 12);
	strings = (const char *)blob + off_strings;
	nodes = calloc(4096, sizeof(*nodes));
	nnodes = 0;
	p = (uint32_t *)(blob + off_struct);
	end = (uint32_t *)(blob + size);
	while (p < end) {
		tok = be32(p++);
		switch (tok) {
		case FDT_BEGIN_NODE:
			n = ++nnodes;
			nd = &nodes[n];
			nd->name = (const char *)p;
			p += (strlen(nd->name) + 4) / 4;
			nd->parent = depth > 0 ? stack[depth - 1] : 0;
			if (nd->parent != 0) {
				if (last_child[depth - 1] == 0)
					nodes[nd->parent].child = n;
				else
					nodes[last_child[depth - 1]].peer = n;
				last_child[depth - 1] = n;
			}
			stack[depth] = n;
			last_child[depth] = 0;
			depth++;
			cur = n;
			break;
		case FDT_END_NODE:
			depth--;
			cur = depth > 0 ? stack[depth - 1] : 0;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 1);

			p += 2;
			nd = &nodes[cur];
			nd->props = realloc(nd->props,
			    (nd->nprops + 1) * sizeof(*nd->props));
			nd->props[nd->nprops].name = strings + nameoff;
			nd->props[nd->nprops].data = (const uint8_t *)p;
			nd->props[nd->nprops].len = len;
			nd->nprops++;
			if (strcmp(strings + nameoff, "phandle") == 0 && len == 4)
				nd->phandle = be32(p);
			p += (len + 3) / 4;
			break;
		}
		case FDT_NOP:
			break;
		case FDT_END:
			p = end;
			break;
		default:
			return (EINVAL);
		}
	}
	return (nnodes > 0 ? 0 : EINVAL);
}

static struct node *
nd(phandle_t h)
{
	return (h > 0 && (int)h <= nnodes ? &nodes[h] : NULL);
}

static const struct prop *
prop(phandle_t h, const char *name)
{
	struct node *n = nd(h);
	int i;

	if (n == NULL)
		return (NULL);
	for (i = 0; i < n->nprops; i++)
		if (strcmp(n->props[i].name, name) == 0)
			return (&n->props[i]);
	return (NULL);
}

phandle_t
OF_finddevice(const char *path)
{
	const char *p, *q;
	phandle_t h, c;
	size_t len;

	if (nnodes == 0 || path[0] != '/')
		return (-1);
	h = 1;
	for (p = path + 1; *p != '\0'; p = *q == '/' ? q + 1 : q) {
		q = strchr(p, '/');
		if (q == NULL)
			q = p + strlen(p);
		len = q - p;
		for (c = nodes[h].child; c != 0; c = nodes[c].peer)
			if (strncmp(nodes[c].name, p, len) == 0 &&
			    (nodes[c].name[len] == '\0' || nodes[c].name[len] == '@'))
				break;
		if (c == 0)
			return (-1);
		h = c;
	}
	return (h);
}

phandle_t
OF_child(phandle_t node)
{
	return (nd(node) != NULL ? nd(node)->child : 0);
}

phandle_t
OF_peer(phandle_t node)
{
	if (node == 0)
		return (nnodes > 0 ? 1 : 0);
	return (nd(node) != NULL ? nd(node)->peer : 0);
}

phandle_t
OF_parent(phandle_t node)
{
	return (nd(node) != NULL ? nd(node)->parent : 0);
}

ssize_t
OF_getproplen(phandle_t node, const char *name)
{
	const struct prop *p;

	if (strcmp(name, "name") == 0 && nd(node) != NULL)
		return (strlen(nd(node)->name) + 1);
	p = prop(node, name);
	return (p != NULL ? (ssize_t)p->len : -1);
}

ssize_t
OF_getprop(phandle_t node, const char *name, void *buf, size_t len)
{
	const struct prop *p;

	if (strcmp(name, "name") == 0 && nd(node) != NULL) {
		/* FreeBSD's ofw_fdt: the node's name, unit address and all */
		strncpy(buf, nd(node)->name, len);
		return (strlen(nd(node)->name) + 1);
	}
	if ((p = prop(node, name)) == NULL)
		return (-1);
	memcpy(buf, p->data, MIN(len, p->len));
	return (p->len);
}

ssize_t
OF_getencprop(phandle_t node, const char *name, pcell_t *buf, size_t len)
{
	ssize_t r;
	size_t i;

	r = OF_getprop(node, name, buf, len);
	if (r < 0)
		return (r);
	for (i = 0; i < MIN(len, (size_t)r) / 4; i++)
		buf[i] = be32(&buf[i]);
	return (r);
}

int
OF_hasprop(phandle_t node, const char *name)
{
	return (prop(node, name) != NULL);
}

ssize_t
OF_getencprop_alloc_multi(phandle_t node, const char *name, int elsz,
    void **buf)
{
	const struct prop *p;
	pcell_t *c;
	uint32_t i;

	*buf = NULL;
	if ((p = prop(node, name)) == NULL || elsz <= 0 || p->len % elsz != 0)
		return (-1);
	c = malloc(p->len + 4);
	memcpy(c, p->data, p->len);
	if (elsz == 4)
		for (i = 0; i < p->len / 4; i++)
			c[i] = be32(&c[i]);
	*buf = c;
	return (p->len / elsz);
}

void
OF_prop_free(void *buf)
{
	free(buf);
}

phandle_t
OF_node_from_xref(phandle_t xref)
{
	int i;

	for (i = 1; i <= nnodes; i++)
		if (nodes[i].phandle == xref)
			return (i);
	return (xref);
}

phandle_t
OF_xref_from_node(phandle_t node)
{
	return (nd(node) != NULL && nd(node)->phandle != 0 ?
	    nd(node)->phandle : node);
}

/* --- ofw_bus ------------------------------------------------------------------------- */

int
ofw_bus_node_is_compatible(phandle_t node, const char *compat)
{
	const struct prop *p = prop(node, "compatible");
	const char *s;

	if (p == NULL)
		return (0);
	for (s = (const char *)p->data; s < (const char *)p->data + p->len;
	    s += strlen(s) + 1)
		if (strcmp(s, compat) == 0)
			return (1);
	return (0);
}

phandle_t
ofw_bus_get_node(device_t dev)
{
	return (sim_device_node(dev));
}

int
ofw_bus_status_okay(device_t dev)
{
	char st[16];

	if (OF_getprop(ofw_bus_get_node(dev), "status", st, sizeof(st)) <= 0)
		return (1);
	return (strcmp(st, "okay") == 0 || strcmp(st, "ok") == 0);
}

int
ofw_bus_is_compatible(device_t dev, const char *compat)
{
	return (ofw_bus_node_is_compatible(ofw_bus_get_node(dev), compat));
}

const struct ofw_compat_data *
ofw_bus_search_compatible(device_t dev, const struct ofw_compat_data *cd)
{
	for (; cd->ocd_str != NULL; cd++)
		if (ofw_bus_is_compatible(dev, cd->ocd_str))
			break;
	return (cd);
}

/* FreeBSD's: the node's descendants, depth first, not the node itself */
phandle_t
ofw_bus_find_compatible(phandle_t node, const char *compat)
{
	phandle_t c, r;

	for (c = OF_child(node); c != 0; c = OF_peer(c)) {
		if (ofw_bus_node_is_compatible(c, compat))
			return (c);
		if ((r = ofw_bus_find_compatible(c, compat)) != 0)
			return (r);
	}
	return (0);
}

int
ofw_bus_find_string_index(phandle_t node, const char *name, const char *str,
    int *idx)
{
	const struct prop *p = prop(node, name);
	const char *s;
	int i = 0;

	if (p == NULL)
		return (ENOENT);
	for (s = (const char *)p->data; s < (const char *)p->data + p->len;
	    s += strlen(s) + 1, i++)
		if (strcmp(s, str) == 0) {
			*idx = i;
			return (0);
		}
	return (ENOENT);
}

phandle_t
ofw_bus_find_iparent(phandle_t node)
{
	pcell_t ip;

	for (; node != 0; node = OF_parent(node))
		if (OF_getencprop(node, "interrupt-parent", &ip, sizeof(ip)) == 4)
			return (ip);
	return (0);
}

/* GIC SPIs only (what the DSPs' nodes use): FreeBSD's IRQ numbers here */
int
ofw_bus_map_intr(device_t dev, phandle_t iparent, int icells, pcell_t *intr)
{
	if (icells == 3 && intr[0] == 0)
		return (32 + intr[1]);
	return (-1);
}

/* For newbus (simkern.c): the nodes below "/" and the buses */
int
sim_fdt_nnodes(void)
{
	return (nnodes);
}
