/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's net/sock.h, which ath10k's qmi.c includes for nothing it uses
 * here; Linux's brings linux/delay.h along, whose msleep qmi.c calls (else
 * it is FreeBSD's msleep(9) macro from sys/systm.h)
 * (freebsd/dsp: Linux platform drivers with LinuxKPI, lkpi_platform.c)
 */

#ifndef	_LKPI_PF_NET_SOCK_H
#define	_LKPI_PF_NET_SOCK_H
#include <linux/net.h>
#include <linux/delay.h>
#endif
