/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * LinuxKPI's linux/netdevice.h and what ath10k's SNOC attachment uses of
 * Linux's beyond it: threaded NAPI, which LinuxKPI's NAPI is anyway (its
 * polls run on a task queue)
 * (freebsd/dsp: Linux platform drivers with LinuxKPI, lkpi_platform.c)
 */

#ifndef	_LKPI_PF_LINUX_NETDEVICE_H
#define	_LKPI_PF_LINUX_NETDEVICE_H

#include_next <linux/netdevice.h>

static inline void
netif_threaded_enable(struct net_device *dev)
{
}

#endif
