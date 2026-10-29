/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's linux/platform_device.h for platform drivers on the device tree:
 * the driver and device structures, their resources, module_platform_driver.
 * Replaces LinuxKPI's stub of this header for the modules built with lkpi/.
 * (freebsd/dsp: Linux platform drivers with LinuxKPI, lkpi_platform.c)
 */

#ifndef	_LINUXKPI_LINUX_PLATFORM_DEVICE_H
#define	_LINUXKPI_LINUX_PLATFORM_DEVICE_H

#include <machine/resource.h>

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>

/* as LinuxKPI's linux/pci.h has them */
#ifndef IORESOURCE_MEM
#define	IORESOURCE_MEM	(1 << SYS_RES_MEMORY)
#define	IORESOURCE_IO	(1 << SYS_RES_IOPORT)
#define	IORESOURCE_IRQ	(1 << SYS_RES_IRQ)
#endif

struct platform_device {
	struct device			dev;	/* first: the newbus softc */
	const char			*name;
	int				id;
	bool				id_auto;
	/* FreeBSD: the matching entry of the driver's of_match_table */
	const struct of_device_id	*of_match;
};

struct platform_driver {
	int	(*probe)(struct platform_device *);
	void	(*remove)(struct platform_device *);
	void	(*shutdown)(struct platform_device *);
	struct {
		const char			*name;
		const struct of_device_id	*of_match_table;
		const struct dev_pm_ops		*pm;
		bool				suppress_bind_attrs;
	} driver;
	void			*lkpi_priv;	/* lkpi_platform.c's */
};

#define	to_platform_device(d)	container_of((d), struct platform_device, dev)
#define	dev_is_platform(d)	(true)

static inline void *
platform_get_drvdata(const struct platform_device *pdev)
{
	return (dev_get_drvdata(&pdev->dev));
}

static inline void
platform_set_drvdata(struct platform_device *pdev, void *data)
{
	dev_set_drvdata(&pdev->dev, data);
}

static inline void *
dev_get_platdata(struct device *dev)
{
	return (NULL);
}

int	lkpi_platform_driver_register(struct platform_driver *);
void	lkpi_platform_driver_unregister(struct platform_driver *);
#define	platform_driver_register(d)	lkpi_platform_driver_register(d)
#define	platform_driver_unregister(d)	lkpi_platform_driver_unregister(d)

#define	module_platform_driver(_drv)					\
	static int __init _drv##_register(void)				\
	{ return (lkpi_platform_driver_register(&(_drv))); }		\
	static void __exit _drv##_unregister(void)			\
	{ lkpi_platform_driver_unregister(&(_drv)); }			\
	module_init(_drv##_register);					\
	module_exit(_drv##_unregister)

struct resource	*platform_get_resource_byname(struct platform_device *,
		    unsigned int type, const char *name);
struct resource	*platform_get_resource(struct platform_device *,
		    unsigned int type, unsigned int num);
int	platform_get_irq(struct platform_device *, unsigned int num);

/* Children made by drivers (ath10k's "wifi-firmware" node): not here */
struct platform_device_info {
	struct device		*parent;
	const char		*name;
	int			id;
	u64			dma_mask;
	struct fwnode_handle	*fwnode;
};

static inline struct platform_device *
platform_device_register_full(const struct platform_device_info *info)
{
	return (ERR_PTR(-ENODEV));
}

static inline void
platform_device_unregister(struct platform_device *pdev)
{
}

/*
 * Mappings that go with the device (LinuxKPI's devm_ioremap is a stub):
 * device memory; memremap's write-through is device memory as on Linux's
 * arm64 (ioremap_wt is ioremap), its write-back and write-combining are
 * normal memory. devm_memremap returns an ERR_PTR on failure, as Linux's.
 */
#define	MEMREMAP_WB	(1 << 0)
#define	MEMREMAP_WT	(1 << 1)
#define	MEMREMAP_WC	(1 << 2)
void	*lkpi_platform_devm_ioremap(struct device *, resource_size_t,
	    resource_size_t, int flags);
#undef	devm_ioremap
#define	devm_ioremap(dev, offset, size)					\
	lkpi_platform_devm_ioremap((dev), (offset), (size), 0)
#define	devm_memremap(dev, offset, size, flags)				\
	({ void *__p = lkpi_platform_devm_ioremap((dev), (offset),	\
	    (size), (flags)); __p != NULL ? __p : ERR_PTR(-ENOMEM); })

#endif	/* _LINUXKPI_LINUX_PLATFORM_DEVICE_H */
