/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * LinuxKPI's firmware loading (firmware(9): /boot/firmware/<name>, e.g.
 * /boot/firmware/qcom/a630_sqe.fw) and Linux's request_firmware_into_buf
 * (mdt_loader.c's split segments), on LinuxKPI's partial read from 0.
 * (freebsd/gpu/msm-port.md)
 */
#ifndef	_MSM_LKPI_LINUX_FIRMWARE_H
#define	_MSM_LKPI_LINUX_FIRMWARE_H
#include_next <linux/firmware.h>
static inline int
request_firmware_into_buf(const struct linuxkpi_firmware **fw,
    const char *name, struct device *dev, void *buf, size_t size)
{
	return (request_partial_firmware_into_buf(fw, name, dev, buf, size,
	    0));
}
#endif
