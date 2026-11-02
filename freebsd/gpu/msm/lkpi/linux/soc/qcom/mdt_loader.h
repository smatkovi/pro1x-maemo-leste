/*-
 * SPDX-License-Identifier: GPL-2.0-only
 * Linux's mdt_loader.h after LinuxKPI's linux/firmware.h, which renames
 * struct firmware to struct linuxkpi_firmware (freebsd/gpu/msm-port.md)
 */
#ifndef	_MSM_LKPI_LINUX_SOC_QCOM_MDT_LOADER_H
#define	_MSM_LKPI_LINUX_SOC_QCOM_MDT_LOADER_H
#include <linux/firmware.h>
#include_next <linux/soc/qcom/mdt_loader.h>
#endif
