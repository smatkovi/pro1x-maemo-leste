# The Pro1X cameras (freebsd/camera/README.md): the CCI as iicbus(4)
# buses, the camera clocks, pins and power domain, /dev/camss
.PATH: ${SRCTOP}/sys/arm64/qualcomm/camss

KMOD=	qcom_camss
SRCS=	qcom_cci.c qcom_camss.c camss_stream.c camss_gcc.c camss_tlmm.c \
	camss_smmu.c
SRCS+=	bus_if.h device_if.h iicbus_if.h ofw_bus_if.h opt_platform.h

.include <bsd.kmod.mk>
