# The GENI UART of the Pro1X's Bluetooth/FM chip as a uart(4) class
# (freebsd/fm/README.md). The PRO1X kernel has no uart(4), so the module
# carries uart(4)'s core with its device tree attachment: uart(4) finds
# its classes in linker sets, which are per module. uart_cpu_fdt.c is the
# console glue of the FDT platforms (with ns8250, which it names; nothing
# on the Pro1X is an ns16550).
.PATH: ${SRCTOP}/sys/dev/uart

KMOD=	qcom_geni_uart
SRCS=	uart_dev_qcom_geni.c
SRCS+=	uart_bus_fdt.c uart_core.c uart_cpu_fdt.c \
	uart_dev_ns8250.c uart_if.c uart_if.h uart_subr.c uart_tty.c
SRCS+=	bus_if.h device_if.h ofw_bus_if.h serdev_if.h
SRCS+=	opt_acpi.h opt_platform.h opt_uart.h

.include <bsd.kmod.mk>
