# The DWC_usb3 in device mode on the SM6115 (freebsd/pm/README.md, stage 4):
# dwc3dci with its Pro1X glue; needs usb.ko (with patches/0034) and
# freebsd/dsp's qcom_dsp.ko; host mode: xhci.ko (generic_xhci.c built in)
.PATH: ${SRCTOP}/sys/dev/usb/controller

KMOD=	pro1x_usb
SRCS=	dwc3dci.c dwc3dci_pro1x.c dwc3dci_host.c generic_xhci.c
SRCS+=	opt_bus.h opt_usb.h opt_platform.h bus_if.h device_if.h usb_if.h \
	ofw_bus_if.h

.include <bsd.kmod.mk>
