# ath10k for the SoC's WCN3990 core on the Pro1X (freebsd/dsp/README.md):
# FreeBSD's ath10k (LinuxKPI) with Linux's SNOC attachment instead of PCI,
# its QMI control path (WLFW over QRTR, the qrtr module) and Linux
# platform drivers on the device tree (lkpi_platform*.c, shim headers in
# lkpi/ in front of LinuxKPI's). Settings as sys/modules/ath10k's.
DEVATH10KDIR=	${SRCTOP}/sys/contrib/dev/athk/ath10k
DSPDIR=		${SRCTOP}/sys/arm64/qualcomm/dsp

.PATH: ${DEVATH10KDIR} ${DSPDIR}

KMOD=	ath10k_snoc

SRCS+=	core.c debug.c mac.c ce.c hw.c bmi.c
SRCS+=	htc.c htt.c htt_rx.c htt_tx.c
SRCS+=	txrx.c wmi.c wmi-tlv.c
SRCS+=	p2p.c swap.c
SRCS+=	fwlog.c
CFLAGS+=	-DCONFIG_FWLOG

# The SoC attachment, its QMI client, the platform driver glue
SRCS+=	snoc.c qmi.c qmi_wlfw_v01.c
SRCS+=	lkpi_platform.c lkpi_platform_bsd.c
# ACPICA's two functions ath10k links against, on a kernel without ACPI
SRCS+=	lkpi_noacpi.c
# the device tree parts of ath10k's core (the calibration variant)
CFLAGS+=	-DCONFIG_OF
# the module's dependencies on qcom_dsp and qrtr (lkpi_platform_bsd.c)
CFLAGS+=	-DLKPI_PF_MODULE=ath10k

# Always on for now (as sys/modules/ath10k)
CFLAGS+=	-DCONFIG_ATH10K_DEBUG
CFLAGS+=	-DLINUXKPI_WANT_LINUX_ACPI

SRCS+=	${LINUXKPI_GENSRCS}
SRCS+=	opt_wlan.h opt_inet6.h opt_inet.h opt_acpi.h opt_platform.h
SRCS+=	bus_if.h device_if.h ofw_bus_if.h

CFLAGS+=	-I${DSPDIR}/lkpi -I${DSPDIR}
CFLAGS+=	-I${DEVATH10KDIR}
CFLAGS+=	-I${DEVATH10KDIR}/..
CFLAGS+=	${LINUXKPI_INCLUDES}

CFLAGS+=	-DKBUILD_MODNAME='"ath10k"'
CFLAGS+=	-DLINUXKPI_VERSION=70000

.include <bsd.kmod.mk>
