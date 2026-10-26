/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The FreeBSD half of Linux platform drivers on the device tree
 * (lkpi_platform.h): a newbus driver per platform driver on simplebus,
 * added when the LinuxKPI half registers it (module_init, as LinuxKPI adds
 * its PCI drivers), the device's memory and interrupt resources, its device
 * tree node, and the SoC's services. Before the Linux driver probes, the
 * device's streams (iommus) are switched to bypass at the SMMU, which no
 * FreeBSD driver drives: DMA addresses are physical (qcom_smmu.c).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/sx.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "qcom_dsp.h"
#include "lkpi_platform.h"

static MALLOC_DEFINE(M_LKPI_PF, "lkpi_pf", "Linux platform drivers");

struct lkpi_pf_driver {
	driver_t		driver;		/* first: device_get_driver() */
	const struct lkpi_pf_ops *ops;
	devclass_t		dc;
};

/* --- newbus ------------------------------------------------------------------------- */

static struct lkpi_pf_driver *
pf_driver(device_t dev)
{
	return ((struct lkpi_pf_driver *)device_get_driver(dev));
}

static int
pf_match(device_t dev)
{
	const struct lkpi_pf_ops *ops = pf_driver(dev)->ops;
	int i;

	for (i = 0; ops->compatible[i] != NULL; i++)
		if (ofw_bus_is_compatible(dev, ops->compatible[i]))
			return (i);
	return (-1);
}

static int
lkpi_pf_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev) || pf_match(dev) < 0)
		return (ENXIO);
	device_set_desc(dev, pf_driver(dev)->ops->name);
	return (BUS_PROBE_DEFAULT);
}

/* The device's streams through bypass at the SMMU (iommus = <smmu sid mask>) */
static void
pf_smmu_bypass(device_t dev)
{
	pcell_t cells[3 * 4];
	ssize_t len;
	int i, error;

	len = OF_getencprop(ofw_bus_get_node(dev), "iommus", cells,
	    sizeof(cells));
	for (i = 0; len > 0 && (i + 3) * (ssize_t)sizeof(pcell_t) <= len; i += 3) {
		error = qcom_smmu_bypass(cells[i], cells[i + 1], cells[i + 2],
		    device_get_nameunit(dev));
		if (error != 0)
			device_printf(dev, "SMMU stream %#x/%#x: no bypass (%d)\n",
			    cells[i + 1], cells[i + 2], error);
	}
}

static int
lkpi_pf_attach(device_t dev)
{
	const struct lkpi_pf_ops *ops = pf_driver(dev)->ops;
	int ret;

	pf_smmu_bypass(dev);
	ret = ops->attach(ops->drv, dev, pf_match(dev));
	return (ret < 0 ? -ret : ret);
}

static int
lkpi_pf_detach(device_t dev)
{
	const struct lkpi_pf_ops *ops = pf_driver(dev)->ops;

	ops->detach(ops->drv, dev);
	return (0);
}

static int
lkpi_pf_shutdown(device_t dev)
{
	const struct lkpi_pf_ops *ops = pf_driver(dev)->ops;

	if (ops->shutdown != NULL)
		ops->shutdown(ops->drv, dev);
	return (0);
}

static device_method_t lkpi_pf_methods[] = {
	DEVMETHOD(device_probe,		lkpi_pf_probe),
	DEVMETHOD(device_attach,	lkpi_pf_attach),
	DEVMETHOD(device_detach,	lkpi_pf_detach),
	DEVMETHOD(device_shutdown,	lkpi_pf_shutdown),
	DEVMETHOD_END
};

int
lkpi_pf_register(const struct lkpi_pf_ops *ops, struct lkpi_pf_driver **out)
{
	struct lkpi_pf_driver *d;
	devclass_t bus;
	int error;

	bus = devclass_find("simplebus");
	if (bus == NULL)
		return (-ENXIO);
	d = malloc(sizeof(*d), M_LKPI_PF, M_WAITOK | M_ZERO);
	d->ops = ops;
	d->driver.name = ops->name;
	d->driver.methods = lkpi_pf_methods;
	d->driver.size = ops->softc_size;
	bus_topo_lock();
	error = devclass_add_driver(bus, &d->driver, BUS_PASS_DEFAULT, &d->dc);
	bus_topo_unlock();
	if (error != 0) {
		free(d, M_LKPI_PF);
		return (-error);
	}
	*out = d;
	return (0);
}

void
lkpi_pf_unregister(struct lkpi_pf_driver *d)
{
	devclass_t bus;

	if (d == NULL)
		return;
	bus_topo_lock();
	if ((bus = devclass_find("simplebus")) != NULL)
		devclass_delete_driver(bus, &d->driver);
	bus_topo_unlock();
	free(d, M_LKPI_PF);
}

/* --- memory ------------------------------------------------------------------------- */

int
lkpi_pf_mem(void *bsddev, const char *name, uint64_t *pa, uint64_t *size)
{
	device_t dev = bsddev;
	rman_res_t start, count;
	int idx = 0;

	if (name != NULL &&
	    ofw_bus_find_string_index(ofw_bus_get_node(dev), "reg-names", name,
	    &idx) != 0)
		return (-ENOENT);
	if (bus_get_resource(dev, SYS_RES_MEMORY, idx, &start, &count) != 0)
		return (-ENOENT);
	*pa = start;
	*size = count;
	return (0);
}

void *
lkpi_pf_map(uint64_t pa, uint64_t size, bool device)
{
	return (pmap_mapdev_attr(pa, size, device ? VM_MEMATTR_DEVICE :
	    VM_MEMATTR_WRITE_THROUGH));
}

void
lkpi_pf_unmap(void *va, uint64_t size)
{
	pmap_unmapdev(va, size);
}

/* --- interrupts ---------------------------------------------------------------------- */

struct pf_irq {
	device_t	dev;
	int		rid;
	struct resource	*res;
	void		*tag;
	int		(*handler)(int, void *);
	void		*arg;
	unsigned int	irq;
	int		depth;		/* disabled this often */
};

#define	PF_MAX_IRQS	64
static struct pf_irq *pf_irqs[PF_MAX_IRQS];
static struct sx pf_irq_lock;
SX_SYSINIT(lkpi_pf_irq, &pf_irq_lock, "lkpi_pf irqs");

/* A number for the device's interrupt index (the same on every call) */
int
lkpi_pf_irq(void *bsddev, int index)
{
	device_t dev = bsddev;
	rman_res_t start, count;
	struct pf_irq *e;
	int i, slot = -1;

	if (bus_get_resource(dev, SYS_RES_IRQ, index, &start, &count) != 0)
		return (-ENXIO);
	sx_xlock(&pf_irq_lock);
	for (i = 0; i < PF_MAX_IRQS; i++) {
		if (pf_irqs[i] != NULL && pf_irqs[i]->dev == dev &&
		    pf_irqs[i]->rid == index) {
			sx_xunlock(&pf_irq_lock);
			return (LKPI_PF_IRQ_BASE + i);
		}
		if (pf_irqs[i] == NULL && slot < 0)
			slot = i;
	}
	if (slot < 0) {
		sx_xunlock(&pf_irq_lock);
		return (-ENOSPC);
	}
	e = malloc(sizeof(*e), M_LKPI_PF, M_WAITOK | M_ZERO);
	e->dev = dev;
	e->rid = index;
	e->irq = LKPI_PF_IRQ_BASE + slot;
	pf_irqs[slot] = e;
	sx_xunlock(&pf_irq_lock);
	return (e->irq);
}

static struct pf_irq *
pf_irq_lookup(unsigned int irq)
{
	if (irq < LKPI_PF_IRQ_BASE || irq >= LKPI_PF_IRQ_BASE + PF_MAX_IRQS)
		return (NULL);
	return (pf_irqs[irq - LKPI_PF_IRQ_BASE]);
}

static void
pf_intr(void *arg)
{
	struct pf_irq *e = arg;

	e->handler((int)e->irq, e->arg);
}

static int
pf_irq_setup(struct pf_irq *e)
{
	return (bus_setup_intr(e->dev, e->res, INTR_TYPE_NET | INTR_MPSAFE,
	    NULL, pf_intr, e, &e->tag));
}

int
lkpi_pf_request_irq(unsigned int irq, int (*handler)(int, void *), void *arg,
    const char *name, bool autoen)
{
	struct pf_irq *e;
	int error = 0, rid;

	sx_xlock(&pf_irq_lock);
	if ((e = pf_irq_lookup(irq)) == NULL || e->res != NULL) {
		sx_xunlock(&pf_irq_lock);
		return (e == NULL ? -EINVAL : -EBUSY);
	}
	rid = e->rid;
	e->res = bus_alloc_resource_any(e->dev, SYS_RES_IRQ, &rid, RF_ACTIVE);
	if (e->res == NULL) {
		sx_xunlock(&pf_irq_lock);
		return (-ENXIO);
	}
	e->handler = handler;
	e->arg = arg;
	e->depth = autoen ? 0 : 1;
	if (e->depth == 0)
		error = pf_irq_setup(e);
	if (error == 0 && name != NULL && e->tag != NULL)
		bus_describe_intr(e->dev, e->res, e->tag, "%s", name);
	if (error != 0) {
		bus_release_resource(e->dev, SYS_RES_IRQ, e->rid, e->res);
		e->res = NULL;
	}
	sx_xunlock(&pf_irq_lock);
	return (-error);
}

void
lkpi_pf_free_irq(unsigned int irq)
{
	struct pf_irq *e;

	sx_xlock(&pf_irq_lock);
	if ((e = pf_irq_lookup(irq)) != NULL && e->res != NULL) {
		if (e->tag != NULL)
			bus_teardown_intr(e->dev, e->res, e->tag);
		e->tag = NULL;
		bus_release_resource(e->dev, SYS_RES_IRQ, e->rid, e->res);
		e->res = NULL;
	}
	sx_xunlock(&pf_irq_lock);
}

/* arm64's nexus cannot mask one interrupt: the handler goes and comes */
void
lkpi_pf_disable_irq(unsigned int irq)
{
	struct pf_irq *e;

	sx_xlock(&pf_irq_lock);
	if ((e = pf_irq_lookup(irq)) != NULL && e->res != NULL &&
	    e->depth++ == 0 && e->tag != NULL) {
		bus_teardown_intr(e->dev, e->res, e->tag);
		e->tag = NULL;
	}
	sx_xunlock(&pf_irq_lock);
}

void
lkpi_pf_enable_irq(unsigned int irq)
{
	struct pf_irq *e;

	sx_xlock(&pf_irq_lock);
	if ((e = pf_irq_lookup(irq)) != NULL && e->res != NULL &&
	    e->depth > 0 && --e->depth == 0 && pf_irq_setup(e) != 0)
		device_printf(e->dev, "interrupt %d not set up again\n", e->rid);
	sx_xunlock(&pf_irq_lock);
}

/* The device's slots go with it (lkpi_platform.c, after the remove) */
static void
pf_irqs_forget(device_t dev)
{
	int i;

	sx_xlock(&pf_irq_lock);
	for (i = 0; i < PF_MAX_IRQS; i++)
		if (pf_irqs[i] != NULL && pf_irqs[i]->dev == dev) {
			if (pf_irqs[i]->res != NULL) {
				if (pf_irqs[i]->tag != NULL)
					bus_teardown_intr(dev, pf_irqs[i]->res,
					    pf_irqs[i]->tag);
				bus_release_resource(dev, SYS_RES_IRQ,
				    pf_irqs[i]->rid, pf_irqs[i]->res);
			}
			free(pf_irqs[i], M_LKPI_PF);
			pf_irqs[i] = NULL;
		}
	sx_xunlock(&pf_irq_lock);
}

/* --- the device tree --------------------------------------------------------------------- */

uintptr_t
lkpi_pf_node(void *bsddev)
{
	return ((uintptr_t)ofw_bus_get_node((device_t)bsddev));
}

bool
lkpi_of_has(uintptr_t node, const char *prop)
{
	return (node != 0 && OF_hasprop((phandle_t)node, prop));
}

int
lkpi_of_read_u32(uintptr_t node, const char *prop, uint32_t *val)
{
	pcell_t v;

	if (node == 0 || OF_getencprop((phandle_t)node, prop, &v, sizeof(v)) !=
	    sizeof(v))
		return (-EINVAL);
	*val = v;
	return (0);
}

/* The string stays valid: OF_getprop_alloc's copy is kept (calls at probe) */
int
lkpi_of_read_string(uintptr_t node, const char *prop, const char **s)
{
	char *str;

	if (node == 0 || OF_getprop_alloc((phandle_t)node, prop,
	    (void **)&str) <= 0)
		return (-EINVAL);
	*s = str;
	return (0);
}

uintptr_t
lkpi_of_child(uintptr_t node, const char *name)
{
	phandle_t child;

	if (node == 0)
		return (0);
	child = ofw_bus_find_child((phandle_t)node, name);
	return (child > 0 ? (uintptr_t)child : 0);
}

/* memory-region[index]: the reserved memory node's reg (2 + 2 cells) */
int
lkpi_of_memory_region(uintptr_t node, int index, uint64_t *pa, uint64_t *size)
{
	pcell_t xrefs[8], reg[4];
	phandle_t mem;
	ssize_t len;

	len = OF_getencprop((phandle_t)node, "memory-region", xrefs,
	    sizeof(xrefs));
	if (len <= 0 || index >= len / (ssize_t)sizeof(pcell_t))
		return (-ENODEV);
	mem = OF_node_from_xref(xrefs[index]);
	if (OF_getencprop(mem, "reg", reg, sizeof(reg)) != sizeof(reg))
		return (-EINVAL);
	*pa = (uint64_t)reg[0] << 32 | reg[1];
	*size = (uint64_t)reg[2] << 32 | reg[3];
	return (0);
}

/* --- the SoC ------------------------------------------------------------------------------ */

/* Only from this processor (HLOS) to others: qcom_scm_assign_mem's case */
int
lkpi_qcom_scm_assign_mem(uint64_t addr, size_t size, uint64_t *srcvm,
    const int *vmids, const int *perms, unsigned int n)
{
	uint32_t v[8], p[8];
	uint64_t newvm = 0;
	unsigned int i;
	int error;

	if (*srcvm != (1ULL << QCOM_SCM_VMID_HLOS) || n == 0 || n > nitems(v)) {
		printf("lkpi_pf: assigning memory from VMIDs %#jx: not here\n",
		    (uintmax_t)*srcvm);
		return (-EOPNOTSUPP);
	}
	for (i = 0; i < n; i++) {
		v[i] = vmids[i];
		p[i] = perms[i];
		newvm |= 1ULL << vmids[i];
	}
	error = qcom_scm_assign_mem(addr, size, v, p, n);
	if (error != 0)
		return (-error);
	*srcvm = newvm;
	return (0);
}

void *
lkpi_qcom_smem_get(int host, unsigned int item, size_t *size, int *error)
{
	return (qcom_smem_get(host, item, size, error));
}

void
lkpi_pf_device_gone(void *bsddev)
{
	pf_irqs_forget(bsddev);
}
