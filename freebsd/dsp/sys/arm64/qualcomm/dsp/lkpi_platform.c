/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The LinuxKPI half of Linux platform drivers on the device tree
 * (lkpi_platform.h): platform_driver_register hands the driver to newbus
 * (lkpi_platform_bsd.c), which attaches it to its device tree nodes on
 * simplebus; this sets up the struct platform_device (the newbus softc)
 * the way LinuxKPI sets up a PCI device (its kobject named after the
 * newbus unit, its DMA tags, its devres list), calls the driver's probe,
 * remove and shutdown, and implements the functions of the shim headers in
 * lkpi/: resources, interrupts, mappings, device tree nodes, regulators,
 * SCM and SMEM.
 *
 * The newbus driver is named after the Linux driver up to its first
 * underscore ("ath10k_snoc" attaches as ath10k0), which is also the name
 * net80211 gives the Wi-Fi's parent device and what devd(8) knows.
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/soc/qcom/smem.h>

#include "lkpi_platform.h"

#define	PF_MAX_COMPAT	8

struct pf_drv {
	struct lkpi_pf_ops	ops;
	const char		*compat[PF_MAX_COMPAT + 1];
	char			name[32];
	struct platform_driver	*pdrv;
	struct lkpi_pf_driver	*bsd;
};

/* --- device tree nodes ------------------------------------------------------------- */

struct pf_node {
	struct device_node	np;
	struct list_head	link;
	char			full_name[64];
	char			name[64];
};

static LIST_HEAD(pf_nodes);
static DEFINE_MUTEX(pf_nodes_lock);
static int pf_ndrivers;

/* one struct device_node per node, kept until the last driver goes */
struct device_node *
lkpi_platform_of_node(uintptr_t ph)
{
	struct pf_node *n;
	char *at;

	if (ph == 0)
		return (NULL);
	mutex_lock(&pf_nodes_lock);
	list_for_each_entry(n, &pf_nodes, link)
		if (n->np.lkpi_phandle == ph)
			goto out;
	n = kzalloc(sizeof(*n), GFP_KERNEL);
	if (n == NULL)
		goto out;
	n->np.lkpi_phandle = ph;
	if (lkpi_of_name(ph, n->full_name, sizeof(n->full_name)) != 0)
		strscpy(n->full_name, "?", sizeof(n->full_name));
	strscpy(n->name, n->full_name, sizeof(n->name));
	if ((at = strchr(n->name, '@')) != NULL)
		*at = '\0';
	n->np.name = n->name;
	n->np.full_name = n->full_name;
	list_add(&n->link, &pf_nodes);
out:
	mutex_unlock(&pf_nodes_lock);
	return (n != NULL ? &n->np : NULL);
}

static void
pf_nodes_free(void)
{
	struct pf_node *n, *t;

	mutex_lock(&pf_nodes_lock);
	list_for_each_entry_safe(n, t, &pf_nodes, link) {
		list_del(&n->link);
		kfree(n);
	}
	mutex_unlock(&pf_nodes_lock);
}

bool
lkpi_platform_of_has(uintptr_t node, const char *prop)
{
	return (lkpi_of_has(node, prop));
}

int
lkpi_platform_of_read_u32(uintptr_t node, const char *prop, u32 *val)
{
	uint32_t v;
	int ret;

	if ((ret = lkpi_of_read_u32(node, prop, &v)) == 0)
		*val = v;
	return (ret);
}

int
lkpi_platform_of_read_string(uintptr_t node, const char *prop, const char **s)
{
	return (lkpi_of_read_string(node, prop, s));
}

const void *
lkpi_platform_of_get_property(uintptr_t node, const char *prop, int *lenp)
{
	const void *buf;
	int len;

	if ((len = lkpi_of_getprop(node, prop, &buf)) < 0)
		return (NULL);
	if (lenp != NULL)
		*lenp = len;
	/* present but empty: a pointer all the same, as Linux's */
	return (buf != NULL ? buf : (const void *)"");
}

uintptr_t
lkpi_platform_of_child(uintptr_t node, const char *name)
{
	return (lkpi_of_child(node, name));
}

int
lkpi_platform_of_memory_region(uintptr_t node, unsigned int idx,
    struct resource *res)
{
	uint64_t pa, size;
	int ret;

	if ((ret = lkpi_of_memory_region(node, idx, &pa, &size)) != 0)
		return (ret);
	memset(res, 0, sizeof(*res));
	res->start = pa;
	res->end = pa + size - 1;
	res->flags = IORESOURCE_MEM;
	return (0);
}

const void *
lkpi_platform_match_data(const struct device *dev)
{
	const struct platform_device *pdev;

	pdev = container_of(dev, struct platform_device, dev);
	return (pdev->of_match != NULL ? pdev->of_match->data : NULL);
}

/* --- devices ------------------------------------------------------------------------- */

/* the platform_device is the newbus softc: nothing to free */
static void
pf_dev_release(struct device *dev)
{
}

static int
pf_attach(void *drv, void *bsddev, int match)
{
	struct pf_drv *pd = drv;
	struct platform_device *pdev;
	struct device *dev;
	int error, ret;

	linux_set_current(curthread);
	pdev = device_get_softc(bsddev);
	dev = &pdev->dev;
	dev->bsddev = bsddev;
	dev->parent = &linux_root_device;
	dev->release = pf_dev_release;
	INIT_LIST_HEAD(&dev->irqents);
	spin_lock_init(&dev->devres_lock);
	INIT_LIST_HEAD(&dev->devres_head);
	error = kobject_init_and_add(&dev->kobj, &linux_dev_ktype,
	    &linux_root_device.kobj, "%s", device_get_nameunit(bsddev));
	if (error != 0)
		return (-error);
	dev->of_node = lkpi_platform_of_node(lkpi_pf_node(bsddev));
	pdev->name = pd->pdrv->driver.name;
	pdev->id = device_get_unit(bsddev);
	pdev->of_match = &pd->pdrv->driver.of_match_table[match];
	error = linux_dma_priv_init(dev);
	if (error != 0) {
		kobject_put(&dev->kobj);
		return (-error);
	}
	ret = pd->pdrv->probe(pdev);
	if (ret != 0) {
		dev_err(dev, "probe failed: %d\n", ret);
		lkpi_devres_release_free_list(dev);
		lkpi_pf_device_gone(bsddev);
		linux_dma_priv_uninit(dev);
		kobject_put(&dev->kobj);
		return (ret);
	}
	return (0);
}

static void
pf_detach(void *drv, void *bsddev)
{
	struct pf_drv *pd = drv;
	struct platform_device *pdev = device_get_softc(bsddev);
	struct device *dev = &pdev->dev;

	linux_set_current(curthread);
	if (pd->pdrv->remove != NULL)
		pd->pdrv->remove(pdev);
	lkpi_devres_release_free_list(dev);
	lkpi_pf_device_gone(bsddev);
	linux_dma_priv_uninit(dev);
	kobject_put(&dev->kobj);
}

static void
pf_shutdown(void *drv, void *bsddev)
{
	struct pf_drv *pd = drv;

	linux_set_current(curthread);
	if (pd->pdrv->shutdown != NULL)
		pd->pdrv->shutdown(device_get_softc(bsddev));
}

int
lkpi_platform_driver_register(struct platform_driver *pdrv)
{
	const struct of_device_id *id;
	struct pf_drv *pd;
	size_t i;
	int ret;

	if (pdrv->driver.of_match_table == NULL)
		return (-EINVAL);
	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (pd == NULL)
		return (-ENOMEM);
	for (i = 0, id = pdrv->driver.of_match_table;
	    i < PF_MAX_COMPAT && id->compatible[0] != '\0'; i++, id++)
		pd->compat[i] = id->compatible;
	strscpy(pd->name, pdrv->driver.name, sizeof(pd->name));
	pd->name[strcspn(pd->name, "_")] = '\0';
	pd->pdrv = pdrv;
	pd->ops.name = pd->name;
	pd->ops.compatible = pd->compat;
	pd->ops.softc_size = sizeof(struct platform_device);
	pd->ops.attach = pf_attach;
	pd->ops.detach = pf_detach;
	pd->ops.shutdown = pf_shutdown;
	pd->ops.drv = pd;
	ret = lkpi_pf_register(&pd->ops, &pd->bsd);
	if (ret != 0) {
		kfree(pd);
		return (ret);
	}
	pdrv->lkpi_priv = pd;
	pf_ndrivers++;
	return (0);
}

void
lkpi_platform_driver_unregister(struct platform_driver *pdrv)
{
	struct pf_drv *pd = pdrv->lkpi_priv;

	if (pd == NULL)
		return;
	/* detaches the devices: their remove */
	lkpi_pf_unregister(pd->bsd);
	pdrv->lkpi_priv = NULL;
	kfree(pd);
	if (--pf_ndrivers == 0)
		pf_nodes_free();
}

/* --- resources ------------------------------------------------------------------------ */

static struct resource *
pf_resource(struct platform_device *pdev, unsigned long flags,
    resource_size_t start, resource_size_t size, const char *name)
{
	struct resource *r;

	r = devm_kzalloc(&pdev->dev, sizeof(*r), GFP_KERNEL);
	if (r == NULL)
		return (NULL);
	r->start = start;
	r->end = start + size - 1;
	r->flags = flags;
	r->name = name;
	return (r);
}

struct resource *
platform_get_resource_byname(struct platform_device *pdev, unsigned int type,
    const char *name)
{
	uint64_t pa, size;

	if (type != IORESOURCE_MEM ||
	    lkpi_pf_mem(pdev->dev.bsddev, name, 0, &pa, &size) != 0)
		return (NULL);
	return (pf_resource(pdev, IORESOURCE_MEM, pa, size, name));
}

struct resource *
platform_get_resource(struct platform_device *pdev, unsigned int type,
    unsigned int num)
{
	uint64_t pa, size;
	int irq;

	if (type == IORESOURCE_MEM) {
		if (lkpi_pf_mem(pdev->dev.bsddev, NULL, num, &pa, &size) != 0)
			return (NULL);
		return (pf_resource(pdev, IORESOURCE_MEM, pa, size, NULL));
	}
	if (type == IORESOURCE_IRQ) {
		if ((irq = lkpi_pf_irq(pdev->dev.bsddev, num)) < 0)
			return (NULL);
		return (pf_resource(pdev, IORESOURCE_IRQ, irq, 1, NULL));
	}
	return (NULL);
}

int
platform_get_irq(struct platform_device *pdev, unsigned int num)
{
	int irq;

	irq = lkpi_pf_irq(pdev->dev.bsddev, num);
	if (irq < 0)
		dev_err(&pdev->dev, "no interrupt %u: %d\n", num, irq);
	return (irq);
}

/* --- mappings ------------------------------------------------------------------------- */

struct pf_map {
	void		*va;
	uint64_t	size;
};

static void
pf_map_release(struct device *dev, void *p)
{
	struct pf_map *m = p;

	lkpi_pf_unmap(m->va, m->size);
}

void *
lkpi_platform_devm_ioremap(struct device *dev, resource_size_t offset,
    resource_size_t size, int flags)
{
	struct pf_map *m;
	int kind;

	if (size == 0)
		return (NULL);
	m = lkpi_devres_alloc(pf_map_release, sizeof(*m), GFP_KERNEL);
	if (m == NULL)
		return (NULL);
	if (flags & MEMREMAP_WB)
		kind = LKPI_PF_MAP_WB;
	else if (flags & MEMREMAP_WC)
		kind = LKPI_PF_MAP_WC;
	else
		kind = LKPI_PF_MAP_DEVICE;	/* ioremap, MEMREMAP_WT */
	m->va = lkpi_pf_map(offset, size, kind);
	m->size = size;
	lkpi_devres_add(dev, m);
	return (m->va);
}

/* --- interrupts ------------------------------------------------------------------------ */

static struct pf_irq_ent {
	irq_handler_t	handler;
	void		*arg;
} pf_irq_ents[LKPI_PF_MAX_IRQS];

static bool
pf_irq_ours(unsigned int irq)
{
	return (irq >= LKPI_PF_IRQ_BASE &&
	    irq < LKPI_PF_IRQ_BASE + LKPI_PF_MAX_IRQS);
}

/* as LinuxKPI's own interrupt handler: a Linux task for the thread */
static int
pf_irq_tramp(int irq, void *arg)
{
	struct pf_irq_ent *e = arg;

	if (linux_set_current_flags(curthread, M_NOWAIT))
		return (IRQ_NONE);
	return (e->handler(irq, e->arg));
}

int
lkpi_platform_request_irq(unsigned int irq, irq_handler_t handler,
    unsigned long flags, const char *name, void *arg)
{
	struct pf_irq_ent *e;

	if (!pf_irq_ours(irq))
		return (lkpi_request_irq(NULL, irq, handler, NULL, flags, name,
		    arg));
	e = &pf_irq_ents[irq - LKPI_PF_IRQ_BASE];
	e->handler = handler;
	e->arg = arg;
	return (lkpi_pf_request_irq(irq, pf_irq_tramp, e, name,
	    (flags & IRQF_NO_AUTOEN) == 0));
}

void
lkpi_platform_free_irq(unsigned int irq, void *arg)
{
	if (!pf_irq_ours(irq)) {
		lkpi_free_irq(irq, arg);
		return;
	}
	lkpi_pf_free_irq(irq);
}

int
lkpi_platform_enable_irq(unsigned int irq)
{
	if (!pf_irq_ours(irq))
		return (lkpi_enable_irq(irq));
	lkpi_pf_enable_irq(irq);
	return (0);
}

void
lkpi_platform_disable_irq(unsigned int irq)
{
	if (!pf_irq_ours(irq)) {
		lkpi_disable_irq(irq);
		return;
	}
	lkpi_pf_disable_irq(irq);
}

/* --- regulators ------------------------------------------------------------------------ */

struct regulator {
	uintptr_t	xref;		/* 0: a dummy */
	bool		on;
	char		who[64];
};

int
lkpi_platform_regulators(struct device *dev, int n,
    struct regulator_bulk_data *r)
{
	struct regulator *reg;
	char prop[64];
	uint32_t xref;
	int i;

	for (i = 0; i < n; i++) {
		reg = devm_kzalloc(dev, sizeof(*reg), GFP_KERNEL);
		if (reg == NULL)
			return (-ENOMEM);
		snprintf(prop, sizeof(prop), "%s-supply", r[i].supply);
		if (lkpi_of_read_u32(of_node_ptr(dev->of_node), prop, &xref) != 0)
			xref = 0;
		reg->xref = xref;
		snprintf(reg->who, sizeof(reg->who), "%s %s", dev_name(dev),
		    r[i].supply);
		if (xref == 0)
			dev_info(dev, "supply %s not found, using dummy "
			    "regulator\n", r[i].supply);
		r[i].consumer = reg;
		r[i].ret = 0;
	}
	return (0);
}

int
lkpi_platform_regulator_bulk(int n, struct regulator_bulk_data *r, bool on)
{
	struct regulator *reg;
	int i, ret, err = 0;

	for (i = 0; i < n; i++) {
		reg = r[i].consumer;
		if (reg == NULL || reg->xref == 0 || reg->on == on)
			continue;
		ret = lkpi_pf_regulator(reg->xref, on, reg->who);
		r[i].ret = ret;
		if (ret == 0) {
			reg->on = on;
			continue;
		}
		err = ret;
		if (on) {
			/* Linux's bulk enable: all or none */
			while (i-- > 0) {
				reg = r[i].consumer;
				if (reg != NULL && reg->xref != 0 && reg->on &&
				    lkpi_pf_regulator(reg->xref, false,
				    reg->who) == 0)
					reg->on = false;
			}
			return (err);
		}
	}
	return (err);
}

/* --- the SoC --------------------------------------------------------------------------- */

int
lkpi_platform_scm_assign_mem(phys_addr_t addr, size_t size, u64 *srcvm,
    const struct qcom_scm_vmperm *newvm, unsigned int n)
{
	int vmids[8], perms[8];
	unsigned int i;

	if (n > nitems(vmids))
		return (-EINVAL);
	for (i = 0; i < n; i++) {
		vmids[i] = newvm[i].vmid;
		perms[i] = newvm[i].perm;
	}
	return (lkpi_qcom_scm_assign_mem(addr, size, srcvm, vmids, perms, n));
}

void *
lkpi_platform_smem_get(int host, unsigned int item, size_t *size)
{
	void *p;
	int error = 0;

	p = lkpi_qcom_smem_get(host, item, size, &error);
	if (p == NULL)
		return (ERR_PTR(error != 0 ? -error : -ENOENT));
	return (p);
}
