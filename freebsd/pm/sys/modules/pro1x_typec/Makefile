# The Pro1X's USB-C port: the PMI632's Type-C block over SPMI
# (freebsd/pm/README.md, stage 4b); needs the kernel's qcom_spmi
.PATH: ${SRCTOP}/sys/dev/qcom_spmi

KMOD=	pro1x_typec
SRCS=	pro1x_typec.c
SRCS+=	bus_if.h device_if.h ofw_bus_if.h opt_platform.h

.include <bsd.kmod.mk>
