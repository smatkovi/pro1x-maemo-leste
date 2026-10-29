/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * LinuxKPI's linux/soc/qcom/qmi.h with QMI behind it (freebsd/dsp): the
 * declarations of the qrtr module's qmi_core.c and qmi_encdec.c, for Linux
 * drivers such as ath10k's qmi.c. Replaces FreeBSD's stub of this header,
 * whose functions only returned -ENXIO.
 */

#ifndef	_LINUXKPI_LINUX_SOC_QCOM_QMI_H
#define	_LINUXKPI_LINUX_SOC_QCOM_QMI_H

#include <linux/qrtr.h>
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/socket.h>
#include <arm64/qualcomm/dsp/qcom_qmi.h>

/*
 * A client connects its handle's socket to the server it found (ath10k's
 * qmi.c); LinuxKPI's kernel_connect is a stub for numbered sockets, this
 * one remembers the peer in the handle (qmi_connect).
 */
#undef	kernel_connect
#define	kernel_connect(sock, addr, len, flags)				\
	qmi_connect((sock), (const struct sockaddr_qrtr *)(addr), (len))

#endif	/* _LINUXKPI_LINUX_SOC_QCOM_QMI_H */
