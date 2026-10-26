# QRTR: the IPC router, its name service and sockets (freebsd/dsp/README.md)
.PATH: ${SRCTOP}/sys/arm64/qualcomm/dsp

KMOD=	qrtr
SRCS=	qrtr_mod.c qrtr_core.c qrtr_sock.c qrtr_glink.c
SRCS+=	opt_platform.h

.include <bsd.kmod.mk>
