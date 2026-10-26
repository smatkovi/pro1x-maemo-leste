# The Qualcomm DSP substrate (freebsd/dsp/README.md)
.PATH: ${SRCTOP}/sys/arm64/qualcomm/dsp

KMOD=	qcom_dsp
SRCS=	qcom_dsp.c qcom_scm.c qcom_smem.c qcom_ipc.c qcom_smp2p.c qcom_pas.c
SRCS+=	glink_core.c qcom_glink.c qcom_smmu.c qcom_fastrpc.c
SRCS+=	bus_if.h device_if.h ofw_bus_if.h opt_platform.h

.include <bsd.kmod.mk>
