/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * LinuxKPI's interrupts, and for platform devices (numbers from
 * platform_get_irq) FreeBSD's through lkpi_platform_bsd.c
 * (freebsd/dsp: Linux platform drivers with LinuxKPI, lkpi_platform.c)
 */

#ifndef	_LKPI_PF_LINUX_INTERRUPT_H
#define	_LKPI_PF_LINUX_INTERRUPT_H

#include_next <linux/interrupt.h>

#ifndef IRQF_NO_AUTOEN
#define	IRQF_NO_AUTOEN	0x00080000
#endif

int	lkpi_platform_request_irq(unsigned int, irq_handler_t, unsigned long,
	    const char *, void *);
void	lkpi_platform_free_irq(unsigned int, void *);
int	lkpi_platform_enable_irq(unsigned int);
void	lkpi_platform_disable_irq(unsigned int);

#define	request_irq(irq, h, flags, name, arg)				\
	lkpi_platform_request_irq((irq), (h), (flags), (name), (arg))
#define	free_irq(irq, arg)	lkpi_platform_free_irq((irq), (arg))
#define	enable_irq(irq)		lkpi_platform_enable_irq(irq)
#define	disable_irq(irq)	lkpi_platform_disable_irq(irq)
#define	disable_irq_nosync(irq)	lkpi_platform_disable_irq(irq)

static inline int
lkpi_platform_irq_wake(unsigned int irq)
{
	return (-EOPNOTSUPP);
}
#define	enable_irq_wake(irq)	lkpi_platform_irq_wake(irq)
#define	disable_irq_wake(irq)	lkpi_platform_irq_wake(irq)

#endif
