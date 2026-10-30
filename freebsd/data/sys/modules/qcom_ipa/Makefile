# The IPA v4.2, the modem's data path (freebsd/data/README.md); on qcom_dsp
# (SCM, SMEM, SMP2P, RPM, SMMU) and qrtr (the kernel's QMI)
.PATH: ${SRCTOP}/sys/arm64/qualcomm/ipa

KMOD=	qcom_ipa
SRCS=	ipa_fbsd.c ipa_hw.c gsi.c ipa_qmi.c ipa_qmi_msg.c qmap.c
SRCS+=	bus_if.h device_if.h ofw_bus_if.h opt_platform.h opt_inet.h opt_inet6.h
CFLAGS+= -I${SRCTOP}/sys/arm64/qualcomm/dsp
# Linux's QMI element tables cast const away, as Linux builds them
CWARNFLAGS.ipa_qmi_msg.c= -Wno-cast-qual

.include <bsd.kmod.mk>
