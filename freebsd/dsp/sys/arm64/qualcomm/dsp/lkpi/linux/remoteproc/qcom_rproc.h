/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's subsystem restart notifications: none here (the modem is
 * started once, by pro1x-dsp; its crash is not passed on yet)
 * (freebsd/dsp: Linux platform drivers with LinuxKPI, lkpi_platform.c)
 */

#ifndef	_LKPI_PF_LINUX_REMOTEPROC_QCOM_RPROC_H
#define	_LKPI_PF_LINUX_REMOTEPROC_QCOM_RPROC_H

#include <linux/notifier.h>

enum qcom_ssr_notify_type {
	QCOM_SSR_BEFORE_POWERUP,
	QCOM_SSR_AFTER_POWERUP,
	QCOM_SSR_BEFORE_SHUTDOWN,
	QCOM_SSR_AFTER_SHUTDOWN,
};

struct qcom_ssr_notify_data {
	const char	*name;
	bool		crashed;
};

static inline void *
qcom_register_ssr_notifier(const char *name, struct notifier_block *nb)
{
	return (nb);
}

static inline int
qcom_unregister_ssr_notifier(void *notify, struct notifier_block *nb)
{
	return (0);
}

#endif
