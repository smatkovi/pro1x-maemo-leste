/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's qcom_scm_assign_mem, on qcom_dsp's (from this processor only)
 * (freebsd/dsp: Linux platform drivers with LinuxKPI, lkpi_platform.c)
 */

#ifndef	_LKPI_PF_LINUX_FIRMWARE_QCOM_QCOM_SCM_H
#define	_LKPI_PF_LINUX_FIRMWARE_QCOM_QCOM_SCM_H

#include <linux/types.h>

#define	QCOM_SCM_VMID_HLOS	0x3
#define	QCOM_SCM_VMID_MSS_MSA	0xF
#define	QCOM_SCM_VMID_WLAN	0x18
#define	QCOM_SCM_VMID_WLAN_CE	0x19
#define	QCOM_SCM_VMID_NAV	0x2B
#define	QCOM_SCM_PERM_READ	0x4
#define	QCOM_SCM_PERM_WRITE	0x2
#define	QCOM_SCM_PERM_EXEC	0x1
#define	QCOM_SCM_PERM_RW	(QCOM_SCM_PERM_READ | QCOM_SCM_PERM_WRITE)
#define	QCOM_SCM_PERM_RWX	(QCOM_SCM_PERM_RW | QCOM_SCM_PERM_EXEC)

struct qcom_scm_vmperm {
	int	vmid;
	int	perm;
};

int	lkpi_platform_scm_assign_mem(phys_addr_t, size_t, u64 *,
	    const struct qcom_scm_vmperm *, unsigned int);
#define	qcom_scm_assign_mem(a, s, src, newvm, n)			\
	lkpi_platform_scm_assign_mem((a), (s), (src), (newvm), (n))

#endif
