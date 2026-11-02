/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * drm-kmod's module parameters (compat.linuxkpi.msm_<name> and hw.msm.<name>
 * with -DDRM_SYSCTL_PARAM_PREFIX=_msm, the node in msm_freebsd_bsd.c) and
 * the string ones msm has (vram="16m"), which drm-kmod's header lacks: a
 * handler that keeps a copy of the tunable's or sysctl's string in the
 * char * (msm_freebsd_bsd.c). (freebsd/gpu/msm-port.md)
 */
#ifndef	_MSM_LKPI_LINUX_MODULEPARAM_H
#define	_MSM_LKPI_LINUX_MODULEPARAM_H

#include_next <linux/moduleparam.h>

SYSCTL_DECL(_hw_msm);

int	lkpi_msm_charp_sysctl(SYSCTL_HANDLER_ARGS);

#undef	LINUXKPI_PARAM_charp
#define	LINUXKPI_PARAM_charp(name, var, perm)				\
	extern const char LINUXKPI_PARAM_DESC(name)[];			\
	LINUXKPI_PARAM_PASS(SYSCTL_PROC(LINUXKPI_PARAM_PARENT, OID_AUTO,\
	LINUXKPI_PARAM_NAME(name), CTLTYPE_STRING | CTLFLAG_MPSAFE |	\
	LINUXKPI_PARAM_PERM(perm), &(var), 0, lkpi_msm_charp_sysctl, "A",\
	LINUXKPI_PARAM_DESC(name)));					\
	LINUXKPI_PARAM_PASS(SYSCTL_PROC(DRM_PARAM_NAME, OID_AUTO, name,	\
	CTLTYPE_STRING | CTLFLAG_MPSAFE | LINUXKPI_PARAM_PERM(perm),	\
	&(var), 0, lkpi_msm_charp_sysctl, "A", LINUXKPI_PARAM_DESC(name)))

#endif
