// SPDX-License-Identifier: GPL-2.0-only
/*
 * The LinuxKPI half of msm.ko (msm_freebsd.h, freebsd/gpu/msm-port.md):
 * the Linux interfaces Linux v6.12's msm (GPU part) uses that neither
 * LinuxKPI nor drm-kmod has, each as close to Linux's semantics as msm
 * needs, on msm_freebsd_bsd.c's FreeBSD side:
 *
 *  - platform drivers: adreno's and the GMU wrapper's on the device tree
 *    (freebsd/dsp's lkpi_platform.c), and the headless "msm" DRM device,
 *    which Linux 6.12 only makes for imx5's GPU: here for the SM6115's,
 *    which has no display driver of Linux's (FreeBSD keeps the display);
 *  - the component framework (adreno is a component of that device);
 *  - runtime PM: usage counts, the drivers' callbacks, device links,
 *    autosuspend (hw.msm.autosuspend);
 *  - power domains (the GMU wrapper's CX and GX GDSCs), clocks, OPPs (with
 *    the VDDCX level), the speed bin cell, device tree helpers;
 *  - Linux's PAS calls for mdt_loader.c and adreno's zap shader, on
 *    qcom_dsp's secure monitor calls;
 *  - dma_alloc_attrs (the VRAM carveout), memparse;
 *  - what msm.ko leaves out: IOMMU (decision A: no IOMMU, msm's VRAM
 *    carveout; msm_iommu_new() finds none), devfreq (the OPP stays), KMS
 *    and fbdev, debugfs's display parts, the a2xx..a5xx catalogs.
 */

#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/firmware/qcom/qcom_scm.h>

#include "msm_drv.h"
#include "msm_gpu.h"
#include "msm_kms.h"
#include "msm_mmu.h"
#include "adreno/adreno_gpu.h"

#include "msm_freebsd.h"

/* --- platform drivers -------------------------------------------------------------- */

#define	MSM_PF_MAX	4

static struct msm_pf_reg {
	struct platform_driver	*drv;
	int			(*probe)(struct platform_device *);
	struct device_driver	devdrv;		/* name, pm: dev->driver */
} msm_pf_regs[MSM_PF_MAX];
static int msm_pf_nregs;

static struct platform_device *msm_headless;
static struct platform_driver *msm_headless_drv;

/* the GMU wrapper (gmu@596a000): a6xx's GMU wrapper code maps its registers
 * and takes its power domains; nothing to do at its probe */
static const struct of_device_id msm_gmuwrap_match[] = {
	{ .compatible = "qcom,adreno-gmu-wrapper" },
	{ }
};

static int
msm_gmuwrap_probe(struct platform_device *pdev)
{
	return (0);
}

static struct platform_driver msm_gmuwrap_driver = {
	.probe = msm_gmuwrap_probe,
	.driver = {
		.name = "gmuwrap",
		.of_match_table = msm_gmuwrap_match,
	},
};
static bool msm_gmuwrap_registered;

/* the driver's probe, with dev->driver (its dev_pm_ops) set first */
static int
msm_pf_probe(struct platform_device *pdev)
{
	const struct of_device_id *id;
	struct msm_pf_reg *r;
	int i;

	for (i = 0; i < msm_pf_nregs; i++) {
		r = &msm_pf_regs[i];
		for (id = r->drv->driver.of_match_table;
		    id != NULL && id->compatible[0] != '\0'; id++)
			if (id == pdev->of_match) {
				pdev->dev.driver = &r->devdrv;
				return (r->probe(pdev));
			}
	}
	dev_err(&pdev->dev, "msm: no driver for this device\n");
	return (-ENODEV);
}

static void msm_rpm_forget_all(void);

static void
msm_headless_release(struct device *dev)
{
}

/*
 * The headless DRM device (Linux's adreno_device_register_headless): no
 * device tree node (msm_drm_init WARNs otherwise), the GPU's newbus device
 * for its DMA and sysctls, DMA as the GPU's (not cache coherent).
 */
static int
msm_headless_create(struct platform_driver *drv)
{
	struct platform_device *pdev, *gpu;
	struct device *dev;
	uintptr_t node;
	int ret;

	node = msm_bsd_of_find_compatible("qcom,adreno");
	gpu = node != 0 ? msm_bsd_find_pdev(node) : NULL;
	if (gpu == NULL) {
		pr_info("msm: no GPU attached: no DRM device\n");
		return (0);
	}
	pdev = kzalloc(sizeof(*pdev), GFP_KERNEL);
	if (pdev == NULL)
		return (-ENOMEM);
	dev = &pdev->dev;
	dev->bsddev = gpu->dev.bsddev;
	dev->parent = &gpu->dev;
	dev->release = msm_headless_release;
	INIT_LIST_HEAD(&dev->irqents);
	spin_lock_init(&dev->devres_lock);
	INIT_LIST_HEAD(&dev->devres_head);
	ret = kobject_init_and_add(&dev->kobj, &linux_dev_ktype,
	    &linux_root_device.kobj, "%s", drv->driver.name);
	if (ret != 0) {
		kfree(pdev);
		return (-ret);
	}
	pdev->name = drv->driver.name;
	pdev->id = -1;
	if ((ret = linux_dma_priv_init(dev)) != 0) {
		kobject_put(&dev->kobj);
		kfree(pdev);
		return (-ret);
	}
	linux_dma_set_noncoherent(dev);
	msm_headless = pdev;
	ret = drv->probe(pdev);
	if (ret != 0) {
		dev_err(dev, "msm: the DRM device's probe failed: %d\n", ret);
		lkpi_devres_release_free_list(dev);
		linux_dma_priv_uninit(dev);
		kobject_put(&dev->kobj);
		kfree(pdev);
		msm_headless = NULL;
	}
	return (ret);
}

static void
msm_headless_destroy(void)
{
	struct platform_device *pdev = msm_headless;

	if (pdev == NULL)
		return;
	if (msm_headless_drv != NULL && msm_headless_drv->remove_new != NULL)
		msm_headless_drv->remove_new(pdev);
	lkpi_devres_release_free_list(&pdev->dev);
	linux_dma_priv_uninit(&pdev->dev);
	kobject_put(&pdev->dev.kobj);
	kfree(pdev);
	msm_headless = NULL;
}

int
lkpi_msm_platform_driver_register(struct platform_driver *drv)
{
	struct msm_pf_reg *r;
	int ret;

	/* a driver without device tree match: the headless "msm" device */
	if (drv->driver.of_match_table == NULL) {
		msm_headless_drv = drv;
		return (msm_headless_create(drv));
	}
	/* the GMU wrapper attached before the GPU binds (a6xx_gmu_wrapper_init) */
	if (strcmp(drv->driver.name, "adreno") == 0 && !msm_gmuwrap_registered) {
		ret = lkpi_msm_platform_driver_register(&msm_gmuwrap_driver);
		if (ret != 0)
			return (ret);
		msm_gmuwrap_registered = true;
	}
	if (msm_pf_nregs == MSM_PF_MAX)
		return (-ENOSPC);
	r = &msm_pf_regs[msm_pf_nregs++];
	r->drv = drv;
	r->probe = drv->probe;
	r->devdrv.name = drv->driver.name;
	r->devdrv.pm = drv->driver.pm;
	drv->probe = msm_pf_probe;
	ret = lkpi_platform_driver_register(drv);
	if (ret != 0) {
		drv->probe = r->probe;
		msm_pf_nregs--;
	}
	return (ret);
}

void
lkpi_msm_platform_driver_unregister(struct platform_driver *drv)
{
	int i;

	if (drv->driver.of_match_table == NULL) {
		msm_headless_destroy();
		msm_headless_drv = NULL;
		return;
	}
	lkpi_platform_driver_unregister(drv);
	for (i = 0; i < msm_pf_nregs; i++)
		if (msm_pf_regs[i].drv == drv) {
			drv->probe = msm_pf_regs[i].probe;
			msm_pf_regs[i] = msm_pf_regs[--msm_pf_nregs];
			break;
		}
	if (strcmp(drv->driver.name, "adreno") == 0 && msm_gmuwrap_registered) {
		msm_gmuwrap_registered = false;
		lkpi_msm_platform_driver_unregister(&msm_gmuwrap_driver);
		msm_rpm_forget_all();
	}
}

/* --- the device tree ------------------------------------------------------------------ */

struct device_node *
of_parse_phandle(const struct device_node *np, const char *prop, int index)
{
	return (lkpi_platform_of_node(msm_bsd_of_parse_phandle(of_node_ptr(np),
	    prop, index)));
}

int
of_device_is_compatible(const struct device_node *np, const char *compat)
{
	return (msm_bsd_of_is_compatible(of_node_ptr(np), compat));
}

bool
of_device_is_available(const struct device_node *np)
{
	return (msm_bsd_of_is_available(of_node_ptr(np)) != 0);
}

/* the first node with one of the table's compatibles (from NULL only) */
struct device_node *
of_find_matching_node(struct device_node *from,
    const struct of_device_id *matches)
{
	uintptr_t n;

	if (from != NULL)
		return (NULL);
	for (; matches != NULL && matches->compatible[0] != '\0'; matches++)
		if ((n = msm_bsd_of_find_compatible(matches->compatible)) != 0)
			return (lkpi_platform_of_node(n));
	return (NULL);
}

struct device_node *
of_find_compatible_node(struct device_node *from, const char *type,
    const char *compat)
{
	if (from != NULL)
		return (NULL);
	return (lkpi_platform_of_node(msm_bsd_of_find_compatible(compat)));
}

int
of_machine_is_compatible(const char *compat)
{
	return (msm_bsd_of_machine_is_compatible(compat));
}

int
of_address_to_resource(struct device_node *np, int index, struct resource *r)
{
	uint64_t pa, size;
	int ret;

	if ((ret = msm_bsd_of_reg(of_node_ptr(np), index, &pa, &size)) != 0)
		return (ret);
	memset(r, 0, sizeof(*r));
	r->start = pa;
	r->end = pa + size - 1;
	r->flags = IORESOURCE_MEM;
	r->name = np->full_name;
	return (0);
}

int
of_property_count_elems_of_size(const struct device_node *np,
    const char *prop, int elem_size)
{
	int len;

	if (of_get_property(np, prop, &len) == NULL)
		return (-EINVAL);
	if (elem_size <= 0 || len % elem_size != 0)
		return (-EINVAL);
	return (len / elem_size);
}

int
of_property_read_u32_index(const struct device_node *np, const char *prop,
    u32 index, u32 *out)
{
	const __be32 *p;
	int len;

	if ((p = of_get_property(np, prop, &len)) == NULL)
		return (-EINVAL);
	if ((index + 1) * sizeof(*p) > (size_t)len)
		return (-EOVERFLOW);
	*out = be32_to_cpu(p[index]);
	return (0);
}

int
of_property_read_u64(const struct device_node *np, const char *prop, u64 *out)
{
	const __be32 *p;
	int len;

	if ((p = of_get_property(np, prop, &len)) == NULL)
		return (-EINVAL);
	if (len < 8)
		return (-EOVERFLOW);
	*out = (u64)be32_to_cpu(p[0]) << 32 | be32_to_cpu(p[1]);
	return (0);
}

/* the platform device msm.ko attached to the node, with a reference */
struct platform_device *
of_find_device_by_node(struct device_node *np)
{
	struct platform_device *pdev;

	pdev = msm_bsd_find_pdev(of_node_ptr(np));
	if (pdev != NULL)
		get_device(&pdev->dev);
	return (pdev);
}

/* --- clocks --------------------------------------------------------------------------- */

struct clk {
	int		id;		/* enum msm_clk_id; MSM_CLK_NONE: a dummy */
	const char	*name;
};

static struct clk msm_clks[MSM_CLK_COUNT];

struct clk *
lkpi_msm_clk_get(struct device *dev, const char *id, bool optional)
{
	uintptr_t node = of_node_ptr(dev->of_node);
	struct clk *c;
	int idx, cid;

	idx = id != NULL ? msm_bsd_of_string_index(node, "clock-names", id) : 0;
	if (node == 0 || idx < 0)
		return (optional ? NULL : ERR_PTR(-ENOENT));
	cid = msm_bsd_clk_of(node, idx);
	if (cid < 0)
		return (optional ? NULL : ERR_PTR(-ENOENT));
	c = &msm_clks[cid];
	c->id = cid;
	c->name = cid != MSM_CLK_NONE ? msm_bsd_clk_name(cid) : "(unknown)";
	if (cid == MSM_CLK_NONE)
		dev_warn(dev, "clock %s: not one of the GPU's known here, "
		    "left as it is\n", id != NULL ? id : "0");
	return (c);
}

int
lkpi_msm_clk_bulk_get_all(struct device *dev, struct clk_bulk_data **clks)
{
	struct clk_bulk_data *c;
	const char *name;
	int i, n;

	for (n = 0; of_property_read_string_index(dev->of_node, "clock-names",
	    n, &name) == 0; n++)
		;
	if (n == 0)
		return (0);
	c = devm_kcalloc(dev, n, sizeof(*c), GFP_KERNEL);
	if (c == NULL)
		return (-ENOMEM);
	for (i = 0; i < n; i++) {
		of_property_read_string_index(dev->of_node, "clock-names", i,
		    &c[i].id);
		c[i].clk = lkpi_msm_clk_get(dev, c[i].id, false);
		if (IS_ERR(c[i].clk))
			return (PTR_ERR(c[i].clk));
	}
	*clks = c;
	return (n);
}

int
clk_prepare_enable(struct clk *clk)
{
	if (IS_ERR_OR_NULL(clk))
		return (0);
	return (msm_bsd_clk_enable(clk->id));
}

void
clk_disable_unprepare(struct clk *clk)
{
	if (!IS_ERR_OR_NULL(clk))
		msm_bsd_clk_disable(clk->id);
}

int
clk_set_rate(struct clk *clk, unsigned long rate)
{
	if (IS_ERR_OR_NULL(clk))
		return (0);
	return (msm_bsd_clk_set_rate(clk->id, rate));
}

unsigned long
clk_get_rate(struct clk *clk)
{
	if (IS_ERR_OR_NULL(clk))
		return (0);
	return (msm_bsd_clk_get_rate(clk->id));
}

long
clk_round_rate(struct clk *clk, unsigned long rate)
{
	if (IS_ERR_OR_NULL(clk))
		return (rate);
	return (msm_bsd_clk_round_rate(clk->id, rate));
}

const char *
__clk_get_name(const struct clk *clk)
{
	return (IS_ERR_OR_NULL(clk) ? NULL : clk->name);
}

/* --- runtime PM ----------------------------------------------------------------------- */

struct msm_rpm {
	struct list_head	link;
	struct device		*dev;
	struct mutex		lock;
	int			usage;
	bool			enabled, active, use_auto;
	int			delay_ms;
	/* a power domain's virtual device: on/off instead of dev_pm_ops */
	int			(*power)(struct device *, bool on);
	/* a device link with DL_FLAG_PM_RUNTIME: resumed first */
	struct device		*supplier;
	struct delayed_work	work;
};

static LIST_HEAD(msm_rpms);
static DEFINE_MUTEX(msm_rpms_lock);

static void msm_rpm_work(struct work_struct *);

static struct msm_rpm *
msm_rpm_of(struct device *dev, bool create)
{
	struct msm_rpm *e;

	mutex_lock(&msm_rpms_lock);
	list_for_each_entry(e, &msm_rpms, link)
		if (e->dev == dev)
			goto out;
	e = NULL;
	if (!create)
		goto out;
	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (e == NULL)
		goto out;
	e->dev = dev;
	e->delay_ms = -1;
	mutex_init(&e->lock);
	INIT_DELAYED_WORK(&e->work, msm_rpm_work);
	list_add(&e->link, &msm_rpms);
out:
	mutex_unlock(&msm_rpms_lock);
	return (e);
}

static void
msm_rpm_forget(struct device *dev)
{
	struct msm_rpm *e;

	if ((e = msm_rpm_of(dev, false)) == NULL)
		return;
	cancel_delayed_work_sync(&e->work);
	mutex_lock(&msm_rpms_lock);
	list_del(&e->link);
	mutex_unlock(&msm_rpms_lock);
	mutex_destroy(&e->lock);
	kfree(e);
}

/* every device's entry, at the GPU driver's unregistration (unload) */
static void
msm_rpm_forget_all(void)
{
	struct msm_rpm *e;

	for (;;) {
		mutex_lock(&msm_rpms_lock);
		e = list_first_entry_or_null(&msm_rpms, struct msm_rpm, link);
		mutex_unlock(&msm_rpms_lock);
		if (e == NULL)
			break;
		msm_rpm_forget(e->dev);
	}
}

static int
msm_rpm_callback(struct msm_rpm *e, bool resume)
{
	const struct dev_pm_ops *ops;
	struct device *dev = e->dev;

	if (e->power != NULL)
		return (e->power(dev, resume));
	ops = dev->driver != NULL ? dev->driver->pm : NULL;
	if (ops == NULL)
		return (0);
	if (resume)
		return (ops->runtime_resume != NULL ? ops->runtime_resume(dev) :
		    0);
	return (ops->runtime_suspend != NULL ? ops->runtime_suspend(dev) : 0);
}

/* with e->lock held */
static int
msm_rpm_resume(struct msm_rpm *e)
{
	int ret;

	if (e->active)
		return (1);
	if (!e->enabled)
		return (-EACCES);
	if (e->supplier != NULL &&
	    (ret = lkpi_pm_runtime_get(e->supplier, true)) < 0) {
		lkpi_pm_runtime_put(e->supplier, LKPI_RPM_PUT_NOIDLE);
		return (ret);
	}
	ret = msm_rpm_callback(e, true);
	if (ret != 0) {
		if (e->supplier != NULL)
			lkpi_pm_runtime_put(e->supplier, LKPI_RPM_PUT_IDLE);
		return (ret);
	}
	e->active = true;
	return (0);
}

static int
msm_rpm_suspend(struct msm_rpm *e)
{
	int ret;

	if (!e->active)
		return (0);
	ret = msm_rpm_callback(e, false);
	if (ret != 0)
		return (ret);
	e->active = false;
	if (e->supplier != NULL)
		lkpi_pm_runtime_put(e->supplier, LKPI_RPM_PUT_IDLE);
	return (0);
}

static void
msm_rpm_work(struct work_struct *w)
{
	struct msm_rpm *e = container_of(to_delayed_work(w), struct msm_rpm,
	    work);

	mutex_lock(&e->lock);
	if (e->usage == 0 && e->enabled)
		msm_rpm_suspend(e);
	mutex_unlock(&e->lock);
}

int
lkpi_pm_runtime_get(struct device *dev, bool resume)
{
	struct msm_rpm *e;
	int ret = 0;

	if ((e = msm_rpm_of(dev, true)) == NULL)
		return (-ENOMEM);
	cancel_delayed_work(&e->work);
	mutex_lock(&e->lock);
	e->usage++;
	if (resume)
		ret = msm_rpm_resume(e);
	mutex_unlock(&e->lock);
	return (ret);
}

int
lkpi_pm_runtime_put(struct device *dev, int how)
{
	struct msm_rpm *e;
	int ret = 0;

	if ((e = msm_rpm_of(dev, false)) == NULL)
		return (0);
	mutex_lock(&e->lock);
	if (e->usage > 0)
		e->usage--;
	if (how == LKPI_RPM_PUT_NOIDLE || e->usage > 0 || !e->active ||
	    !e->enabled)
		goto out;
	/*
	 * Autosuspend after its delay, only with hw.msm.autosuspend=1; else
	 * the device stays on until it is put without autosuspend (a6xx's
	 * recovery does that) or goes
	 */
	if (how != LKPI_RPM_PUT_SUSPEND && e->use_auto) {
		if (msm_bsd_autosuspend() && e->delay_ms >= 0)
			schedule_delayed_work(&e->work,
			    msecs_to_jiffies(e->delay_ms));
		goto out;
	}
	ret = msm_rpm_suspend(e);
out:
	mutex_unlock(&e->lock);
	return (ret);
}

void
lkpi_pm_runtime_enable(struct device *dev, bool on)
{
	struct msm_rpm *e;

	if ((e = msm_rpm_of(dev, true)) == NULL)
		return;
	mutex_lock(&e->lock);
	e->enabled = on;
	mutex_unlock(&e->lock);
}

bool
lkpi_pm_runtime_enabled(struct device *dev)
{
	struct msm_rpm *e = msm_rpm_of(dev, false);

	return (e != NULL && e->enabled);
}

bool
lkpi_pm_runtime_active(struct device *dev)
{
	struct msm_rpm *e = msm_rpm_of(dev, false);

	return (e != NULL && e->active);
}

void
lkpi_pm_runtime_autosuspend(struct device *dev, int delay_ms, int use)
{
	struct msm_rpm *e;

	if ((e = msm_rpm_of(dev, true)) == NULL)
		return;
	mutex_lock(&e->lock);
	if (delay_ms >= 0)
		e->delay_ms = delay_ms;
	if (use >= 0)
		e->use_auto = use != 0;
	mutex_unlock(&e->lock);
}

void
lkpi_pm_runtime_set_active(struct device *dev, bool on)
{
	struct msm_rpm *e;

	if ((e = msm_rpm_of(dev, true)) == NULL)
		return;
	mutex_lock(&e->lock);
	e->active = on;
	mutex_unlock(&e->lock);
}

/* pm_runtime_force_suspend/resume: system sleep and adreno's shutdown */
int
lkpi_pm_runtime_force(struct device *dev, bool resume)
{
	struct msm_rpm *e;
	int ret = 0;

	if ((e = msm_rpm_of(dev, true)) == NULL)
		return (-ENOMEM);
	cancel_delayed_work_sync(&e->work);
	mutex_lock(&e->lock);
	if (!resume) {
		ret = msm_rpm_suspend(e);
		e->enabled = false;
	} else {
		e->enabled = true;
		if (e->usage > 0)
			ret = msm_rpm_resume(e);
		if (ret > 0)
			ret = 0;
	}
	mutex_unlock(&e->lock);
	return (ret);
}

/* --- power domains (the GMU wrapper's "cx" and "gx") ------------------------------- */

struct msm_pd {
	struct device		dev;	/* the virtual device */
	int			pd;	/* MSM_PD_* */
	bool			synced;	/* the next off waits for its status */
	struct notifier_block	*nb;
	char			name[16];
};

static int
msm_pd_power(struct device *dev, bool on)
{
	struct msm_pd *pd = container_of(dev, struct msm_pd, dev);
	int ret;

	ret = msm_bsd_gdsc(pd->pd, on, !on && pd->synced);
	if (!on) {
		pd->synced = false;
		if (ret == 0 && pd->nb != NULL)
			pd->nb->notifier_call(pd->nb, GENPD_NOTIFY_OFF, NULL);
	}
	return (ret);
}

static void
msm_pd_release(struct device *dev)
{
}

struct device *
lkpi_msm_pd_attach(struct device *dev, const char *name)
{
	struct msm_rpm *e;
	struct msm_pd *pd;
	int idx;

	idx = msm_bsd_pd_of(of_node_ptr(dev->of_node), name);
	if (idx == -ENOENT)
		return (NULL);
	if (idx < 0)
		return (ERR_PTR(idx));
	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (pd == NULL)
		return (ERR_PTR(-ENOMEM));
	pd->pd = idx;
	snprintf(pd->name, sizeof(pd->name), "genpd:%s", name);
	pd->dev.parent = dev;
	pd->dev.bsddev = dev->bsddev;
	pd->dev.release = msm_pd_release;
	kobject_init(&pd->dev.kobj, &linux_dev_ktype);
	kobject_set_name(&pd->dev.kobj, "%s", pd->name);
	if ((e = msm_rpm_of(&pd->dev, true)) == NULL) {
		kfree(pd);
		return (ERR_PTR(-ENOMEM));
	}
	/* genpd's virtual devices come with runtime PM enabled */
	e->power = msm_pd_power;
	e->enabled = true;
	return (&pd->dev);
}

void
lkpi_msm_pd_detach(struct device *dev)
{
	struct msm_pd *pd;
	struct msm_rpm *e;

	if (IS_ERR_OR_NULL(dev))
		return;
	pd = container_of(dev, struct msm_pd, dev);
	if ((e = msm_rpm_of(dev, false)) != NULL) {
		mutex_lock(&e->lock);
		msm_rpm_suspend(e);
		mutex_unlock(&e->lock);
	}
	msm_rpm_forget(dev);
	kfree(pd);
}

int
lkpi_msm_pd_notifier(struct device *dev, struct notifier_block *nb, bool add)
{
	struct msm_pd *pd;

	if (IS_ERR_OR_NULL(dev))
		return (-ENODEV);
	pd = container_of(dev, struct msm_pd, dev);
	pd->nb = add ? nb : NULL;
	return (0);
}

int
lkpi_msm_pd_synced_poweroff(struct device *dev)
{
	if (IS_ERR_OR_NULL(dev))
		return (-ENODEV);
	container_of(dev, struct msm_pd, dev)->synced = true;
	return (0);
}

/* DL_FLAG_PM_RUNTIME: the consumer's runtime resume resumes the supplier */
struct device_link *
lkpi_msm_device_link_add(struct device *consumer, struct device *supplier,
    u32 flags)
{
	struct msm_rpm *e;

	if (IS_ERR_OR_NULL(supplier) || (e = msm_rpm_of(consumer, true)) == NULL)
		return (NULL);
	if (flags & DL_FLAG_PM_RUNTIME) {
		mutex_lock(&e->lock);
		e->supplier = supplier;
		if (e->active || (flags & DL_FLAG_RPM_ACTIVE))
			lkpi_pm_runtime_get(supplier, true);
		mutex_unlock(&e->lock);
	}
	return ((struct device_link *)e);
}

void
lkpi_msm_device_link_del(struct device_link *link)
{
	struct msm_rpm *e = (struct msm_rpm *)link;

	if (e == NULL)
		return;
	mutex_lock(&e->lock);
	if (e->supplier != NULL && e->active)
		lkpi_pm_runtime_put(e->supplier, LKPI_RPM_PUT_IDLE);
	e->supplier = NULL;
	mutex_unlock(&e->lock);
}

/* --- OPPs ------------------------------------------------------------------------------ */

#define	MSM_OPP_MAX	16

struct dev_pm_opp {
	unsigned long	rate;
	unsigned int	level;
};

static struct msm_opp_table {
	struct device		*dev;
	struct dev_pm_opp	opps[MSM_OPP_MAX];	/* rising rates */
	int			n;
	u32			supported_hw;
	bool			have_supported_hw;
	struct clk		*clk;
	unsigned int		cur_level;
} msm_opp;

static struct msm_opp_table *
msm_opp_of(struct device *dev)
{
	if (msm_opp.dev == NULL)
		msm_opp.dev = dev;
	return (msm_opp.dev == dev ? &msm_opp : NULL);
}

static void
msm_opp_insert(struct msm_opp_table *t, unsigned long rate, unsigned int level)
{
	int i;

	if (t->n == MSM_OPP_MAX)
		return;
	for (i = t->n; i > 0 && t->opps[i - 1].rate > rate; i--)
		t->opps[i] = t->opps[i - 1];
	t->opps[i].rate = rate;
	t->opps[i].level = level;
	t->n++;
}

int
devm_pm_opp_set_supported_hw(struct device *dev, const u32 *hw,
    unsigned int count)
{
	struct msm_opp_table *t = msm_opp_of(dev);

	if (t == NULL || count == 0)
		return (-EINVAL);
	t->supported_hw = hw[0];
	t->have_supported_hw = true;
	return (0);
}

int
devm_pm_opp_set_clkname(struct device *dev, const char *name)
{
	struct msm_opp_table *t = msm_opp_of(dev);
	struct clk *c;

	if (t == NULL)
		return (-EINVAL);
	c = lkpi_msm_clk_get(dev, name, false);
	if (IS_ERR(c))
		return (PTR_ERR(c));
	t->clk = c;
	return (0);
}

/* the node's table, without OPPs the speed bin or hw.msm.max_freq rule out */
int
devm_pm_opp_of_add_table(struct device *dev)
{
	struct msm_opp_table *t = msm_opp_of(dev);
	struct msm_bsd_opp o[MSM_OPP_MAX];
	unsigned long max = msm_bsd_max_freq();
	int i, n;

	if (t == NULL)
		return (-EINVAL);
	n = msm_bsd_opp_table(of_node_ptr(dev->of_node), o, MSM_OPP_MAX);
	if (n < 0)
		return (n);
	for (i = 0; i < n; i++) {
		if (t->have_supported_hw && o[i].supported_hw != 0 &&
		    (o[i].supported_hw & t->supported_hw) == 0)
			continue;
		if (max != 0 && o[i].hz > max) {
			dev_info(dev, "OPP %ju Hz left out (hw.msm.max_freq %lu)\n",
			    (uintmax_t)o[i].hz, max);
			continue;
		}
		msm_opp_insert(t, o[i].hz, o[i].level);
	}
	for (i = 0; i < t->n; i++)
		dev_info(dev, "OPP %lu Hz, VDDCX level %u\n", t->opps[i].rate,
		    t->opps[i].level);
	if (t->clk == NULL)
		t->clk = lkpi_msm_clk_get(dev, NULL, true);
	return (t->n > 0 ? 0 : -ENODEV);
}

int
dev_pm_opp_add(struct device *dev, unsigned long freq, unsigned long u_volt)
{
	struct msm_opp_table *t = msm_opp_of(dev);

	if (t == NULL)
		return (-EINVAL);
	msm_opp_insert(t, freq, 0);
	return (0);
}

int
dev_pm_opp_get_opp_count(struct device *dev)
{
	struct msm_opp_table *t = msm_opp_of(dev);

	return (t != NULL ? t->n : -ENODEV);
}

struct dev_pm_opp *
dev_pm_opp_find_freq_ceil(struct device *dev, unsigned long *freq)
{
	struct msm_opp_table *t = msm_opp_of(dev);
	int i;

	for (i = 0; t != NULL && i < t->n; i++)
		if (t->opps[i].rate >= *freq) {
			*freq = t->opps[i].rate;
			return (&t->opps[i]);
		}
	return (ERR_PTR(-ERANGE));
}

struct dev_pm_opp *
dev_pm_opp_find_freq_floor(struct device *dev, unsigned long *freq)
{
	struct msm_opp_table *t = msm_opp_of(dev);
	int i;

	for (i = t != NULL ? t->n : 0; i-- > 0; )
		if (t->opps[i].rate <= *freq) {
			*freq = t->opps[i].rate;
			return (&t->opps[i]);
		}
	return (ERR_PTR(-ERANGE));
}

struct dev_pm_opp *
dev_pm_opp_find_freq_exact(struct device *dev, unsigned long freq,
    bool available)
{
	struct msm_opp_table *t = msm_opp_of(dev);
	int i;

	for (i = 0; t != NULL && i < t->n; i++)
		if (t->opps[i].rate == freq)
			return (&t->opps[i]);
	return (ERR_PTR(-ERANGE));
}

void
dev_pm_opp_put(struct dev_pm_opp *opp)
{
}

unsigned long
dev_pm_opp_get_freq(struct dev_pm_opp *opp)
{
	return (IS_ERR_OR_NULL(opp) ? 0 : opp->rate);
}

unsigned int
dev_pm_opp_get_level(struct dev_pm_opp *opp)
{
	return (IS_ERR_OR_NULL(opp) ? 0 : opp->level);
}

/*
 * The OPP: VDDCX's level up before the clock goes up, down after it went
 * down (Linux's _set_opp); NULL drops the level the GPU needs.
 */
int
dev_pm_opp_set_opp(struct device *dev, struct dev_pm_opp *opp)
{
	struct msm_opp_table *t = msm_opp_of(dev);
	int ret;

	if (t == NULL)
		return (-EINVAL);
	if (IS_ERR_OR_NULL(opp)) {
		t->cur_level = 0;
		return (msm_bsd_cx_level(0));
	}
	if (opp->level > t->cur_level &&
	    (ret = msm_bsd_cx_level(opp->level)) != 0)
		return (ret);
	if ((ret = clk_set_rate(t->clk, opp->rate)) != 0)
		return (ret);
	if (opp->level < t->cur_level)
		msm_bsd_cx_level(opp->level);
	t->cur_level = opp->level;
	return (0);
}

int
dev_pm_opp_set_rate(struct device *dev, unsigned long rate)
{
	struct dev_pm_opp *opp;
	unsigned long f = rate;

	if (rate == 0)
		return (dev_pm_opp_set_opp(dev, NULL));
	opp = dev_pm_opp_find_freq_ceil(dev, &f);
	if (IS_ERR(opp)) {
		f = rate;
		opp = dev_pm_opp_find_freq_floor(dev, &f);
	}
	if (IS_ERR(opp))
		return (PTR_ERR(opp));
	return (dev_pm_opp_set_opp(dev, opp));
}

int
dev_pm_opp_set_config(struct device *dev, struct dev_pm_opp_config *config)
{
	if (config->supported_hw != NULL && config->supported_hw_count > 0)
		devm_pm_opp_set_supported_hw(dev, config->supported_hw,
		    config->supported_hw_count);
	if (config->clk_names != NULL && config->clk_names[0] != NULL)
		devm_pm_opp_set_clkname(dev, config->clk_names[0]);
	return (0);
}

int
devm_pm_opp_set_config(struct device *dev, struct dev_pm_opp_config *config)
{
	return (dev_pm_opp_set_config(dev, config));
}

/* --- nvmem: the speed bin ----------------------------------------------------------- */

int
nvmem_cell_read_variable_le_u32(struct device *dev, const char *cell_id,
    u32 *val)
{
	uint32_t v;
	int ret;

	ret = msm_bsd_nvmem_cell_u32(of_node_ptr(dev->of_node), cell_id, &v);
	if (ret == 0)
		*val = v;
	return (ret);
}

/* --- the component framework ------------------------------------------------------- */

struct component {
	struct list_head		link;
	struct device			*dev;
	const struct component_ops	*ops;
	struct device			*master;	/* bound to */
};

struct component_match {
	int	n;
	struct {
		int	(*compare)(struct device *, void *);
		void	(*release)(struct device *, void *);
		void	*data;
		struct component *comp;
	} e[4];
};

struct component_master {
	struct list_head		link;
	struct device			*dev;
	const struct component_master_ops *ops;
	struct component_match		*match;
	bool				bound;
};

static LIST_HEAD(msm_components);
static LIST_HEAD(msm_masters);
static DEFINE_MUTEX(msm_component_lock);

static struct component_master *
master_of(struct device *dev)
{
	struct component_master *m;

	list_for_each_entry(m, &msm_masters, link)
		if (m->dev == dev)
			return (m);
	return (NULL);
}

/* a master whose components are all there binds (outside the lock) */
static int
master_try_bind(struct component_master *m)
{
	struct component *c;
	int i, ret;

	mutex_lock(&msm_component_lock);
	if (m->bound) {
		mutex_unlock(&msm_component_lock);
		return (0);
	}
	for (i = 0; i < m->match->n; i++) {
		m->match->e[i].comp = NULL;
		list_for_each_entry(c, &msm_components, link)
			if (m->match->e[i].compare(c->dev,
			    m->match->e[i].data)) {
				m->match->e[i].comp = c;
				break;
			}
		if (m->match->e[i].comp == NULL) {
			mutex_unlock(&msm_component_lock);
			return (0);
		}
	}
	m->bound = true;
	mutex_unlock(&msm_component_lock);
	ret = m->ops->bind(m->dev);
	if (ret != 0) {
		dev_err(m->dev, "binding the components failed: %d\n", ret);
		m->bound = false;
	}
	return (ret);
}

int
component_add(struct device *dev, const struct component_ops *ops)
{
	struct component_master *m;
	struct component *c;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (c == NULL)
		return (-ENOMEM);
	c->dev = dev;
	c->ops = ops;
	mutex_lock(&msm_component_lock);
	list_add_tail(&c->link, &msm_components);
	mutex_unlock(&msm_component_lock);
	list_for_each_entry(m, &msm_masters, link)
		master_try_bind(m);
	return (0);
}

void
component_del(struct device *dev, const struct component_ops *ops)
{
	struct component_master *m;
	struct component *c, *t;
	int i;

	/* a master bound with it unbinds first */
	list_for_each_entry(m, &msm_masters, link) {
		if (!m->bound)
			continue;
		for (i = 0; i < m->match->n; i++)
			if (m->match->e[i].comp != NULL &&
			    m->match->e[i].comp->dev == dev) {
				m->ops->unbind(m->dev);
				m->bound = false;
				break;
			}
	}
	mutex_lock(&msm_component_lock);
	list_for_each_entry_safe(c, t, &msm_components, link)
		if (c->dev == dev && c->ops == ops) {
			list_del(&c->link);
			kfree(c);
		}
	mutex_unlock(&msm_component_lock);
}

int
component_bind_all(struct device *master, void *data)
{
	struct component_master *m = master_of(master);
	struct component *c;
	int i, ret;

	if (m == NULL)
		return (-EINVAL);
	for (i = 0; i < m->match->n; i++) {
		c = m->match->e[i].comp;
		ret = c->ops->bind(c->dev, master, data);
		if (ret != 0) {
			dev_err(master, "failed to bind %s: %d\n",
			    dev_name(c->dev), ret);
			while (i-- > 0) {
				c = m->match->e[i].comp;
				c->ops->unbind(c->dev, master, data);
				c->master = NULL;
			}
			return (ret);
		}
		c->master = master;
	}
	return (0);
}

void
component_unbind_all(struct device *master, void *data)
{
	struct component_master *m = master_of(master);
	struct component *c;
	int i;

	if (m == NULL)
		return;
	for (i = m->match->n; i-- > 0; ) {
		c = m->match->e[i].comp;
		if (c != NULL && c->master == master) {
			c->ops->unbind(c->dev, master, data);
			c->master = NULL;
		}
	}
}

void
component_match_add_release(struct device *master,
    struct component_match **matchptr,
    void (*release)(struct device *, void *),
    int (*compare)(struct device *, void *), void *compare_data)
{
	struct component_match *match = *matchptr;

	if (IS_ERR(match))
		return;
	if (match == NULL) {
		match = kzalloc(sizeof(*match), GFP_KERNEL);
		if (match == NULL) {
			*matchptr = ERR_PTR(-ENOMEM);
			return;
		}
		*matchptr = match;
	}
	if (match->n == (int)nitems(match->e)) {
		*matchptr = ERR_PTR(-ENOSPC);
		return;
	}
	match->e[match->n].compare = compare;
	match->e[match->n].release = release;
	match->e[match->n].data = compare_data;
	match->n++;
}

int
component_master_add_with_match(struct device *dev,
    const struct component_master_ops *ops, struct component_match *match)
{
	struct component_master *m;
	int ret;

	if (IS_ERR_OR_NULL(match))
		return (match == NULL ? -EINVAL : PTR_ERR(match));
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (m == NULL)
		return (-ENOMEM);
	m->dev = dev;
	m->ops = ops;
	m->match = match;
	mutex_lock(&msm_component_lock);
	list_add_tail(&m->link, &msm_masters);
	mutex_unlock(&msm_component_lock);
	ret = master_try_bind(m);
	return (ret);
}

void
component_master_del(struct device *dev, const struct component_master_ops *ops)
{
	struct component_master *m;
	int i;

	mutex_lock(&msm_component_lock);
	m = master_of(dev);
	if (m != NULL)
		list_del(&m->link);
	mutex_unlock(&msm_component_lock);
	if (m == NULL)
		return;
	if (m->bound)
		m->ops->unbind(dev);
	for (i = 0; i < m->match->n; i++)
		if (m->match->e[i].release != NULL)
			m->match->e[i].release(dev, m->match->e[i].data);
	kfree(m->match);
	kfree(m);
}

int
component_compare_of(struct device *dev, void *data)
{
	return (dev->of_node == data);
}

void
component_release_of(struct device *dev, void *data)
{
	of_node_put(data);
}

/* --- secure monitor calls, Linux's way (lkpi/linux/firmware/qcom/qcom_scm.h) -------- */

bool
qcom_scm_is_available(void)
{
	return (msm_bsd_scm_available() != 0);
}

/* the metadata in memory TrustZone reads by physical address, kept with
 * the context until released (Linux's dma_alloc_coherent on the SCM) */
int
qcom_scm_pas_init_image(u32 peripheral, const void *metadata, size_t size,
    struct qcom_scm_pas_metadata *ctx)
{
	uint64_t pa;
	void *va;
	int ret;

	va = msm_bsd_contig_alloc(size, &pa);
	if (va == NULL)
		return (-ENOMEM);
	memcpy(va, metadata, size);
	ret = msm_bsd_pas_init_image(peripheral, pa);
	if (ret != 0 || ctx == NULL) {
		msm_bsd_contig_free(va, size);
		return (ret);
	}
	ctx->ptr = va;
	ctx->phys = pa;
	ctx->size = size;
	return (0);
}

void
qcom_scm_pas_metadata_release(struct qcom_scm_pas_metadata *ctx)
{
	if (ctx == NULL || ctx->ptr == NULL)
		return;
	msm_bsd_contig_free(ctx->ptr, ctx->size);
	ctx->ptr = NULL;
	ctx->phys = 0;
	ctx->size = 0;
}

int
qcom_scm_pas_mem_setup(u32 peripheral, phys_addr_t addr, phys_addr_t size)
{
	return (msm_bsd_pas_mem_setup(peripheral, addr, size));
}

int
qcom_scm_pas_auth_and_reset(u32 peripheral)
{
	return (msm_bsd_pas_auth_and_reset(peripheral));
}

int
qcom_scm_pas_shutdown(u32 peripheral)
{
	return (msm_bsd_pas_shutdown(peripheral));
}

bool
qcom_scm_pas_supported(u32 peripheral)
{
	return (msm_bsd_pas_supported(peripheral) != 0);
}

/* --- interrupts, capabilities ----------------------------------------------------- */

struct msm_irq_res {
	unsigned int	irq;
	void		*arg;
};

static void
msm_irq_release(struct device *dev, void *p)
{
	struct msm_irq_res *r = p;

	lkpi_platform_free_irq(r->irq, r->arg);
}

/* devm_request_irq on the platform glue's interrupts, freed with the device */
int
lkpi_msm_devm_request_irq(struct device *dev, unsigned int irq,
    irq_handler_t handler, unsigned long flags, const char *name, void *arg)
{
	struct msm_irq_res *r;
	int ret;

	r = lkpi_devres_alloc(msm_irq_release, sizeof(*r), GFP_KERNEL);
	if (r == NULL)
		return (-ENOMEM);
	ret = lkpi_platform_request_irq(irq, handler, flags, name, arg);
	if (ret != 0) {
		lkpi_devres_free(r);
		return (ret);
	}
	r->irq = irq;
	r->arg = arg;
	lkpi_devres_add(dev, r);
	return (0);
}

bool
lkpi_msm_rawio_capable(void)
{
	return (msm_bsd_unprivileged_submit() ||
	    priv_check(curthread, PRIV_IO) == 0);
}

/* --- DMA and memparse -------------------------------------------------------------- */

void *
lkpi_msm_dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *handle,
    gfp_t gfp, unsigned long attrs)
{
	uint64_t pa;
	void *cookie;

	if ((attrs & DMA_ATTR_NO_KERNEL_MAPPING) == 0)
		return (dma_alloc_coherent(dev, size, handle, gfp));
	/* the VRAM carveout: its pages by physical address (SMMU bypass) */
	cookie = msm_bsd_vram_alloc(size, &pa);
	if (cookie == NULL)
		return (NULL);
	*handle = pa;
	return (cookie);
}

void
lkpi_msm_dma_free_attrs(struct device *dev, size_t size, void *vaddr,
    dma_addr_t handle, unsigned long attrs)
{
	if ((attrs & DMA_ATTR_NO_KERNEL_MAPPING) == 0) {
		dma_free_coherent(dev, size, vaddr, handle);
		return;
	}
	msm_bsd_vram_free(handle, size);
}

unsigned long long
memparse(const char *ptr, char **retptr)
{
	char *end;
	unsigned long long v;

	v = simple_strtoull(ptr, &end, 0);
	switch (*end) {
	case 'E': case 'e':
		v <<= 10;
		/* FALLTHROUGH */
	case 'P': case 'p':
		v <<= 10;
		/* FALLTHROUGH */
	case 'T': case 't':
		v <<= 10;
		/* FALLTHROUGH */
	case 'G': case 'g':
		v <<= 10;
		/* FALLTHROUGH */
	case 'M': case 'm':
		v <<= 10;
		/* FALLTHROUGH */
	case 'K': case 'k':
		v <<= 10;
		end++;
		break;
	default:
		break;
	}
	if (retptr != NULL)
		*retptr = end;
	return (v);
}

/* --- what msm.ko leaves out ---------------------------------------------------------- */

/* the catalogs of the Adreno generations not built here */
const struct adreno_gpulist a2xx_gpulist = { NULL, 0 };
const struct adreno_gpulist a3xx_gpulist = { NULL, 0 };
const struct adreno_gpulist a4xx_gpulist = { NULL, 0 };
const struct adreno_gpulist a5xx_gpulist = { NULL, 0 };

/*
 * No IOMMU: device_iommu_mapped() is false, so msm_iommu_new() returns
 * NULL as Linux's does then, gpu->aspace stays NULL and msm allocates from
 * its VRAM carveout (msm-patches: a6xx without private address spaces)
 */
struct msm_mmu *
msm_iommu_new(struct device *dev, unsigned long quirks)
{
	return (NULL);
}

struct msm_mmu *
msm_iommu_gpu_new(struct device *dev, struct msm_gpu *gpu,
    unsigned long quirks)
{
	return (NULL);
}

struct msm_mmu *
msm_iommu_pagetable_create(struct msm_mmu *parent)
{
	return (ERR_PTR(-ENODEV));
}

int
msm_iommu_pagetable_params(struct msm_mmu *mmu, phys_addr_t *ttbr, int *asid)
{
	return (-EINVAL);
}

struct iommu_domain_geometry *
msm_iommu_get_geometry(struct msm_mmu *mmu)
{
	return (ERR_PTR(-ENODEV));
}

/* devfreq: none; the GPU stays at the OPP its power up set */
void
msm_devfreq_init(struct msm_gpu *gpu)
{
}

void
msm_devfreq_cleanup(struct msm_gpu *gpu)
{
}

void
msm_devfreq_resume(struct msm_gpu *gpu)
{
}

void
msm_devfreq_suspend(struct msm_gpu *gpu)
{
}

void
msm_devfreq_boost(struct msm_gpu *gpu, unsigned factor)
{
}

void
msm_devfreq_active(struct msm_gpu *gpu)
{
}

void
msm_devfreq_idle(struct msm_gpu *gpu)
{
}

/* KMS and fbdev: never asked for (priv->kms_init is NULL, headless) */
int
msm_drm_kms_init(struct device *dev, const struct drm_driver *drv)
{
	return (-ENODEV);
}

void
msm_drm_kms_uninit(struct device *dev)
{
}

void
msm_fbdev_setup(struct drm_device *dev)
{
}

/*
 * debugfs: msm_debugfs.c includes the display's snapshot code; its late
 * init's GPU part is rd (the command stream dump) and perf
 */
void
msm_debugfs_init(struct drm_minor *minor)
{
}

static int
msm_debugfs_late_init_minor(struct drm_minor *minor)
{
	int ret;

	if (minor == NULL)
		return (0);
	if ((ret = msm_rd_debugfs_init(minor)) != 0) {
		DRM_DEV_ERROR(minor->dev->dev, "could not install rd debugfs\n");
		return (ret);
	}
	if ((ret = msm_perf_debugfs_init(minor)) != 0) {
		DRM_DEV_ERROR(minor->dev->dev,
		    "could not install perf debugfs\n");
		return (ret);
	}
	return (0);
}

int
msm_debugfs_late_init(struct drm_device *dev)
{
	int ret;

	if ((ret = msm_debugfs_late_init_minor(dev->primary)) != 0)
		return (ret);
	return (msm_debugfs_late_init_minor(dev->render));
}
