# APR, the ADSP's audio services' packets, as /dev/apr (freebsd/dsp/README.md)
.PATH: ${SRCTOP}/sys/arm64/qualcomm/dsp

KMOD=	qcom_apr
SRCS=	qcom_apr.c
SRCS+=	bus_if.h device_if.h opt_platform.h

.include <bsd.kmod.mk>
