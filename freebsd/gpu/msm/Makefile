# msm.ko: Linux v6.12's msm DRM driver, GPU part only (Adreno a6xx for the
# SM6115's Adreno 610), on drm-kmod 6.12 and LinuxKPI, for the F(x)tec
# Pro1X (freebsd/gpu/msm-port.md). GPL-2.0, as Linux's msm.
#
# Built by freebsd/gpu/build.sh (target msm) in the cross build's
# environment, with
#   LINUX     the Linux v6.12 sources build.sh fetched (drivers/gpu/drm/msm
#             with freebsd/gpu/msm-patches applied, generated/ register
#             headers, mdt_loader)
#   DRM_KMOD  drm-kmod's checkout (headers, kconfig.mk: the same CONFIG_*
#             as drm.ko, the structures must match)
#   DSPDIR    freebsd/dsp/sys/arm64/qualcomm/dsp (the Linux platform driver
#             glue lkpi_platform*.c, its shim headers, qcom_dsp.h)
# No KMS, DPU, DSI, HDMI or DP: the display stays FreeBSD's (scfb on the
# framebuffer Linux left).

DSPDIR?=	${.CURDIR:H:H}/dsp/sys/arm64/qualcomm/dsp
.if !defined(LINUX) || !defined(DRM_KMOD)
.error "LINUX and DRM_KMOD (freebsd/gpu/build.sh msm sets them)"
.endif
MSMDIR=		${LINUX}/drivers/gpu/drm/msm

# drm.ko's CONFIG_* (struct layouts in the drm headers depend on them)
.include "${DRM_KMOD}/kconfig.mk"
# and msm's own, as a GPU-only Linux build sets them
KCONFIG+=	DRM_MSM \
		ARCH_QCOM \
		QCOM_SCM \
		QCOM_MDT_LOADER

.PATH:	${MSMDIR} ${MSMDIR}/adreno ${LINUX}/drivers/soc/qcom ${DSPDIR}

KMOD=	msm

# msm's core without KMS (msm_kms.c, msm_fb.c, msm_atomic*.c, msm_fbdev.c,
# msm_mdss.c, disp/, dsi/, hdmi/, dp/): the GEM objects, the submit path,
# the scheduler's rings, fences, debugfs's rd and perf files (msm_debugfs.c
# itself includes the display's snapshot code: msm_freebsd.c sets up rd and
# perf as its late init does)
SRCS=	msm_drv.c \
	msm_fence.c \
	msm_gem.c \
	msm_gem_prime.c \
	msm_gem_shrinker.c \
	msm_gem_submit.c \
	msm_gem_vma.c \
	msm_gpu.c \
	msm_io_utils.c \
	msm_perf.c \
	msm_rd.c \
	msm_ringbuffer.c \
	msm_submitqueue.c

# Adreno: the device, the common part, a6xx (a6xx_gmu.c for the GMU
# wrapper, a6xx_hfi.c for what a6xx_gmu.c calls); no a2xx..a5xx (their
# catalogs are empty in msm_freebsd.c), no a6xx_gpu_state.c (no
# CONFIG_DRM_MSM_GPU_STATE)
SRCS+=	adreno_device.c \
	adreno_gpu.c \
	a6xx_catalog.c \
	a6xx_gpu.c \
	a6xx_gmu.c \
	a6xx_hfi.c

# the zap shader's loader (Linux's drivers/soc/qcom/mdt_loader.c)
SRCS+=	mdt_loader.c

# FreeBSD: the platform drivers, the Linux interfaces msm uses that LinuxKPI
# lacks (msm_freebsd.c), the GPU's power, clocks and SMMU (msm_freebsd_bsd.c)
SRCS+=	msm_freebsd.c msm_freebsd_bsd.c
SRCS+=	lkpi_platform.c lkpi_platform_bsd.c

SRCS+=	device_if.h bus_if.h ofw_bus_if.h vnode_if.h pci_if.h \
	opt_platform.h opt_acpi.h opt_drm.h opt_vm.h

# nothing depends on msm.ko: its symbols stay its own (lkpi_platform*.c
# is in ath10k_snoc.ko too, the Linux interfaces of msm_freebsd.c might
# come to LinuxKPI)
EXPORT_SYMS=	NO

CLEANFILES+= ${KMOD}.ko.full ${KMOD}.ko.debug

.include <bsd.kmod.mk>

.include "${DRM_KMOD}/linuxkpi_version.mk"
.include "${DRM_KMOD}/compiler_flags.mk"

# Shim headers first (lkpi/: msm's; the DSP glue's), then drm-kmod's and
# LinuxKPI's, then the few Linux headers neither has (adreno-smmu-priv.h,
# soc/qcom/mdt_loader.h, uapi/drm/msm_drm.h as <drm/msm_drm.h>)
CFLAGS+= -I${.CURDIR}/lkpi
CFLAGS+= -I${DSPDIR}/lkpi -I${DSPDIR}
CFLAGS+= -I${DRM_KMOD}/linuxkpi/gplv2/include
CFLAGS+= -I${DRM_KMOD}/linuxkpi/bsd/include
CFLAGS+= -I${SYSDIR}/compat/linuxkpi/common/include
CFLAGS+= -I${SYSDIR}/compat/linuxkpi/dummy/include
CFLAGS+= -I${MSMDIR} -I${LINUX}/generated
CFLAGS+= -I${DRM_KMOD}/include
CFLAGS+= -I${DRM_KMOD}/include/drm
CFLAGS+= -I${DRM_KMOD}/include/uapi
CFLAGS+= -I${DRM_KMOD}/drivers/gpu
CFLAGS+= -I${LINUX}/include/uapi
CFLAGS+= -I${LINUX}/include
# msm_drm.h includes "drm.h" from its own directory, which only has msm_drm.h
CFLAGS+= -I${DRM_KMOD}/include/uapi/drm
CFLAGS+= -I${.CURDIR}

CFLAGS+= '-DKBUILD_MODNAME="${KMOD}"'
CFLAGS+= '-DLINUXKPI_PARAM_PREFIX=msm_' -DDRM_SYSCTL_PARAM_PREFIX=_${KMOD}
CFLAGS+= ${KCONFIG:C/(.*)/-DCONFIG_\1/}
# lkpi_platform_bsd.c: the module's dependency on qcom_dsp, none on qrtr; no
# SMMU bypass at attach (the GPU's SMMU is powered with the GPU's CX
# domain: msm_freebsd_bsd.c switches it after the power-up)
CFLAGS+= -DLKPI_PF_MODULE=msm -DLKPI_PF_NO_QRTR -DLKPI_PF_NO_SMMU_BYPASS

CWARNFLAGS+=	-Wno-format -Wno-gnu-pointer-arith -Wno-cast-qual
CWARNFLAGS+=	-Wno-missing-prototypes -Wno-unused-function
CWARNFLAGS+=	-Wno-unused-variable -Wno-unused-but-set-variable
CWARNFLAGS+=	-Wno-unused-const-variable -Wno-missing-include-dirs
