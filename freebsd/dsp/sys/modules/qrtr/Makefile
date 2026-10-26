# QRTR: the IPC router, its name service and sockets (freebsd/dsp/README.md)
.PATH: ${SRCTOP}/sys/arm64/qualcomm/dsp

KMOD=	qrtr
SRCS=	qrtr_mod.c qrtr_core.c qrtr_sock.c qrtr_glink.c
# QMI on top: clients and servers, Linux's encoder (which does arithmetic
# on void pointers and casts const away, as Linux builds it)
SRCS+=	qmi_core.c qmi_encdec.c
CWARNFLAGS.qmi_encdec.c= -Wno-pointer-arith -Wno-sign-compare -Wno-cast-qual
SRCS+=	opt_platform.h

# its functions for the modules on top (the Wi-Fi (QMI in the kernel))
EXPORT_SYMS=	YES

.include <bsd.kmod.mk>
