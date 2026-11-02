/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's generic power domains, as much as msm's GMU wrapper uses: a
 * device's power domains by name ("power-domains", "power-domain-names")
 * as virtual devices whose runtime resume and suspend switch the domain:
 * the GPU clock controller's GDSCs (CX, GX) natively (msm_freebsd_bsd.c,
 * Linux's gdsc.c sequence); the GENPD_NOTIFY_OFF notifier for a domain
 * that went off (a6xx_recover waits for CX to collapse).
 * (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_PM_DOMAIN_H
#define	_MSM_LKPI_LINUX_PM_DOMAIN_H

#include <linux/device.h>
#include <linux/err.h>
#include <linux/notifier.h>

enum {
	GENPD_NOTIFY_PRE_OFF = 0,
	GENPD_NOTIFY_OFF,
	GENPD_NOTIFY_PRE_ON,
	GENPD_NOTIFY_ON,
};

#define	DL_FLAG_STATELESS		BIT(0)
#define	DL_FLAG_AUTOREMOVE_CONSUMER	BIT(1)
#define	DL_FLAG_PM_RUNTIME		BIT(2)
#define	DL_FLAG_RPM_ACTIVE		BIT(3)
#define	DL_FLAG_AUTOREMOVE_SUPPLIER	BIT(4)

struct device_link;

struct device	*lkpi_msm_pd_attach(struct device *, const char *);
void	lkpi_msm_pd_detach(struct device *);
int	lkpi_msm_pd_notifier(struct device *, struct notifier_block *, bool add);
int	lkpi_msm_pd_synced_poweroff(struct device *);
struct device_link *lkpi_msm_device_link_add(struct device *consumer,
	    struct device *supplier, u32 flags);
void	lkpi_msm_device_link_del(struct device_link *);

#define	dev_pm_domain_attach_by_name(dev, name)	lkpi_msm_pd_attach((dev), (name))
#define	dev_pm_domain_detach(dev, off)		lkpi_msm_pd_detach(dev)
#define	dev_pm_genpd_add_notifier(dev, nb)	lkpi_msm_pd_notifier((dev), (nb), true)
#define	dev_pm_genpd_remove_notifier(dev)	lkpi_msm_pd_notifier((dev), NULL, false)
#define	dev_pm_genpd_synced_poweroff(dev)	lkpi_msm_pd_synced_poweroff(dev)
#define	device_link_add(c, s, f)		lkpi_msm_device_link_add((c), (s), (f))
#define	device_link_del(l)			lkpi_msm_device_link_del(l)

#endif
