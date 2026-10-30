# The apps processor's RPM sleep set at 0 (freebsd/pm/README.md, stage 3),
# through freebsd/dsp's qcom_dsp.ko
.PATH: ${SRCTOP}/sys/arm64/qualcomm/pm

KMOD=	pro1x_rpmsleep
SRCS=	pro1x_rpmsleep.c
SRCS+=	bus_if.h device_if.h ofw_bus_if.h opt_platform.h

.include <bsd.kmod.mk>
