# The LPASS SoundWire masters' interrupts for pro1x-audiod (/dev/qcom_swrN,
# freebsd/audio/README.md, "The headset jack")
.PATH: ${SRCTOP}/sys/arm64/qualcomm/audio

KMOD=	qcom_swr
SRCS=	qcom_swr.c
SRCS+=	bus_if.h device_if.h ofw_bus_if.h pic_if.h opt_platform.h

.include <bsd.kmod.mk>
