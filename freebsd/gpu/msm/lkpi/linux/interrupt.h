/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The DSP glue's interrupts (freebsd/dsp lkpi/linux/interrupt.h: numbers
 * from platform_get_irq on FreeBSD's interrupt resources) and what msm
 * uses beyond them: devm_request_irq (msm_gpu_init's GPU interrupt), which
 * in LinuxKPI only knows PCI devices' interrupts, here the platform glue's
 * request with a devres that frees it (msm_freebsd.c); the trigger flags
 * (the device tree's trigger is what counts). (freebsd/gpu/msm-port.md)
 */
#ifndef	_MSM_LKPI_LINUX_INTERRUPT_H
#define	_MSM_LKPI_LINUX_INTERRUPT_H
#include_next <linux/interrupt.h>
#ifndef IRQF_TRIGGER_HIGH
#define	IRQF_TRIGGER_RISING	0x00000001
#define	IRQF_TRIGGER_FALLING	0x00000002
#define	IRQF_TRIGGER_HIGH	0x00000004
#define	IRQF_TRIGGER_LOW	0x00000008
#endif
int	lkpi_msm_devm_request_irq(struct device *, unsigned int, irq_handler_t,
	    unsigned long, const char *, void *);
#define	devm_request_irq(dev, irq, handler, flags, name, arg)		\
	lkpi_msm_devm_request_irq((dev), (irq), (handler), (flags), (name), \
	    (arg))
#endif
