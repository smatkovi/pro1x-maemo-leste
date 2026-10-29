/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux platform drivers (struct platform_driver) on FreeBSD's device tree
 * buses, for drivers built with LinuxKPI such as ath10k's SNOC attachment
 * of the SoC's WCN3990 (freebsd/dsp/README.md). Two halves, since FreeBSD's
 * resource and Open Firmware headers do not mix with LinuxKPI's:
 * lkpi_platform.c (LinuxKPI: struct platform_device, the driver's probe and
 * remove, the shim headers' functions in lkpi/) and lkpi_platform_bsd.c
 * (FreeBSD: the newbus driver, memory and interrupt resources, the device
 * tree, the SoC's services in qcom_dsp). This is what they call of each
 * other, in plain C types; errors are Linux's negative errnos.
 */

#ifndef _LKPI_PLATFORM_H_
#define	_LKPI_PLATFORM_H_

#include <sys/types.h>

struct lkpi_pf_driver;

/* A platform driver, as the LinuxKPI half hands it to newbus */
struct lkpi_pf_ops {
	const char	*name;		/* the newbus driver's: name of the units */
	const char * const *compatible;	/* NULL-terminated */
	size_t		softc_size;	/* struct platform_device */
	/* match: the index into compatible[] */
	int		(*attach)(void *drv, void *bsddev, int match);
	void		(*detach)(void *drv, void *bsddev);
	void		(*shutdown)(void *drv, void *bsddev);
	void		*drv;		/* the struct platform_driver */
};

int	lkpi_pf_register(const struct lkpi_pf_ops *ops,
	    struct lkpi_pf_driver **out);
void	lkpi_pf_unregister(struct lkpi_pf_driver *d);

/*
 * The attached device's resources, by the device tree's reg (by its name
 * in reg-names, or the index) and interrupts; mappings of memory
 */
int	lkpi_pf_mem(void *bsddev, const char *name, int index, uint64_t *pa,
	    uint64_t *size);
int	lkpi_pf_irq(void *bsddev, int index);
#define	LKPI_PF_MAP_DEVICE	0
#define	LKPI_PF_MAP_WC		1	/* normal memory, uncached */
#define	LKPI_PF_MAP_WB		2	/* normal memory, cached */
void	*lkpi_pf_map(uint64_t pa, uint64_t size, int kind);
void	lkpi_pf_unmap(void *va, uint64_t size);
/* the device goes: its interrupts are released (after the driver's remove) */
void	lkpi_pf_device_gone(void *bsddev);

/*
 * Interrupts by the numbers lkpi_pf_irq gives (from LKPI_PF_IRQ_BASE up),
 * with Linux's semantics: a handler without autoen starts disabled, and
 * disable/enable nest; disable waits for a running handler.
 */
#define	LKPI_PF_IRQ_BASE	0x40000000u
#define	LKPI_PF_MAX_IRQS	64
int	lkpi_pf_request_irq(unsigned int irq, int (*handler)(int, void *),
	    void *arg, const char *name, bool autoen);
void	lkpi_pf_free_irq(unsigned int irq);
void	lkpi_pf_enable_irq(unsigned int irq);
void	lkpi_pf_disable_irq(unsigned int irq);

/* The device tree: nodes are phandles */
uintptr_t lkpi_pf_node(void *bsddev);
/* the node's name with its unit address ("wifi@c800000") */
int	lkpi_of_name(uintptr_t node, char *buf, size_t len);
bool	lkpi_of_has(uintptr_t node, const char *prop);
/* a property's bytes, kept until the last driver goes: its length, or -errno */
int	lkpi_of_getprop(uintptr_t node, const char *prop, const void **buf);
int	lkpi_of_read_u32(uintptr_t node, const char *prop, uint32_t *val);
int	lkpi_of_read_string(uintptr_t node, const char *prop, const char **s);
uintptr_t lkpi_of_child(uintptr_t node, const char *name);
int	lkpi_of_memory_region(uintptr_t node, int index, uint64_t *pa,
	    uint64_t *size);

/*
 * A regulator by the xref of its node (a <supply>-supply), on or off: the
 * RPM's through qcom_dsp (unless hw.lkpi_pf.regulators=0). 0 or -errno.
 */
int	lkpi_pf_regulator(uintptr_t xref, bool on, const char *who);

/* The SoC's services (qcom_dsp), with Linux's meanings */
int	lkpi_qcom_scm_assign_mem(uint64_t addr, size_t size, uint64_t *srcvm,
	    const int *vmids, const int *perms, unsigned int n);
void	*lkpi_qcom_smem_get(int host, unsigned int item, size_t *size,
	    int *error);

#endif /* _LKPI_PLATFORM_H_ */
