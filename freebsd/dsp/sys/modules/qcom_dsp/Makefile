# The Qualcomm DSP substrate (freebsd/dsp/README.md)
.PATH: ${SRCTOP}/sys/arm64/qualcomm/dsp

KMOD=	qcom_dsp
SRCS=	qcom_dsp.c qcom_scm.c qcom_smem.c qcom_ipc.c qcom_smp2p.c qcom_pas.c
SRCS+=	glink_core.c qcom_glink.c qcom_smmu.c qcom_fastrpc.c qcom_rmtfs.c
# votes at the RPM (the Wi-Fi's rails)
SRCS+=	rpm_core.c qcom_rpm.c
SRCS+=	bus_if.h device_if.h ofw_bus_if.h opt_platform.h

# its functions for the modules on top (qrtr and the Wi-Fi)
EXPORT_SYMS=	YES

.include <bsd.kmod.mk>
