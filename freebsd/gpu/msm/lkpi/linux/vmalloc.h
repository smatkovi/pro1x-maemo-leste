/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * LinuxKPI's vmalloc and Linux's vmap purge notifier (msm's shrinker
 * unmaps idle objects when vmalloc space runs low): FreeBSD's kernel map
 * has no such notifier, the registration succeeds and is never called.
 * (freebsd/gpu/msm-port.md)
 */
#ifndef	_MSM_LKPI_LINUX_VMALLOC_H
#define	_MSM_LKPI_LINUX_VMALLOC_H
#include_next <linux/vmalloc.h>
#include <linux/notifier.h>
static inline int
register_vmap_purge_notifier(struct notifier_block *nb)
{
	return (0);
}
static inline int
unregister_vmap_purge_notifier(struct notifier_block *nb)
{
	return (0);
}
#endif
