/*
 * Linux's <linux/soc/qcom/qmi.h> for the host test: qcom_qmi.h and the
 * kernel's fixed-size types, so that ath10k's message tables
 * (qmi_wlfw_v01.c) compile unchanged.
 */
#ifndef _TEST_LINUX_SOC_QCOM_QMI_H
#define	_TEST_LINUX_SOC_QCOM_QMI_H
#include <stddef.h>
#include <stdint.h>
#include <linux/types.h>
#include "qcom_qmi.h"
typedef uint8_t		u8;
typedef uint16_t	u16;
typedef uint32_t	u32;
typedef uint64_t	u64;
#endif
