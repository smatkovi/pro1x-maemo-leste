/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The Qualcomm GENI UART ("qcom,geni-uart", a QUP v3 serial engine in
 * UART mode) as a uart(4) class: on the F(x)tec Pro1X the Bluetooth UART
 * at 4a8c000, which the WCN3950 (Bluetooth and FM) hangs on
 * (freebsd/fm/README.md). Modelled on Linux's qcom_geni_serial.c in its
 * FIFO mode (the one its console uses: no SE DMA, so no SMMU stream) and
 * qcom-geni-se.c; register names are Linux's.
 *
 * FreeBSD has no clock or pin control drivers for the SM6115, and Linux's
 * boot menu may leave the engine with its clock gated and its pins in the
 * "sleep" state (the UART is not open there). Reading an engine register
 * with the clock gated hangs the bus, so before the first access:
 *
 * - the serial engine clock (GCC_QUPV3_WRAP0_S3_CLK, the only one known
 *   here: halt register 0x1f4d4, vote bit 13 of 0x7900c, RCG 0x1f4d8, from
 *   Linux's gcc-sm6115.c) is checked and, if gated, voted on
 *   (hw.qcom_geni_uart.clk_enable=0: never; then a gated clock refuses
 *   the attach);
 * - the pins of the node's "default" pinctrl state are put in their
 *   function (qup3 on gpio8-11, function 1 in Linux's pinctrl-sm6115.c),
 *   with its bias and drive strength (hw.qcom_geni_uart.pinmux=0: left
 *   as they are).
 *
 * The baud rate as Linux sets it: the serial engine's source clock picked
 * from the RCG's frequency table (uart_qcom_geni_calc.h) and programmed,
 * then the engine's divider (hw.qcom_geni_uart.set_rate=0: the RCG left
 * as it is and only the divider changed). Hardware flow control (RFR from
 * the RX FIFO's level, CTS gating TX) follows CRTS_IFLOW and CCTS_OFLOW;
 * without it RTS is the manual RFR.
 */

#include "opt_platform.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>

#include <machine/bus.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_subr.h>

#include <dev/uart/uart.h>
#include <dev/uart/uart_cpu.h>
#include <dev/uart/uart_cpu_fdt.h>
#include <dev/uart/uart_bus.h>
#include <dev/uart/uart_qcom_geni_calc.h>

#include "uart_if.h"

/* GENI serial engine (include/linux/soc/qcom/geni-se.h) */
#define	GENI_FORCE_DEFAULT_REG	0x20
#define	 FORCE_DEFAULT		(1u << 0)
#define	GENI_OUTPUT_CTRL	0x24
#define	 DEFAULT_IO_OUTPUT_CTRL_MSK 0x7fu
#define	SE_GENI_CGC_CTRL	0x28
#define	 DEFAULT_CGC_EN		0x7fu
#define	SE_GENI_STATUS		0x40
#define	 M_GENI_CMD_ACTIVE	(1u << 0)
#define	 S_GENI_CMD_ACTIVE	(1u << 12)
#define	GENI_SER_M_CLK_CFG	0x48
#define	GENI_SER_S_CLK_CFG	0x4c
#define	GENI_IF_DISABLE_RO	0x64
#define	 FIFO_IF_DISABLE	(1u << 0)
#define	GENI_FW_REVISION_RO	0x68
#define	 FW_REV_PROTOCOL(x)	(((x) >> 8) & 0xff)
#define	 GENI_SE_UART		2
#define	SE_GENI_CLK_SEL		0x7c
#define	SE_GENI_BYTE_GRAN	0x254
#define	SE_GENI_DMA_MODE_EN	0x258
#define	 GENI_DMA_MODE_EN	(1u << 0)
#define	SE_GENI_TX_PACKING_CFG0	0x260
#define	SE_GENI_TX_PACKING_CFG1	0x264
#define	SE_GENI_RX_PACKING_CFG0	0x284
#define	SE_GENI_RX_PACKING_CFG1	0x288
#define	SE_GENI_M_CMD0		0x600
#define	 M_OPCODE_SHFT		27
#define	SE_GENI_M_CMD_CTRL_REG	0x604
#define	 M_GENI_CMD_CANCEL	(1u << 2)
#define	 M_GENI_CMD_ABORT	(1u << 1)
#define	SE_GENI_M_IRQ_STATUS	0x610
#define	SE_GENI_M_IRQ_EN	0x614
#define	SE_GENI_M_IRQ_CLEAR	0x618
#define	 M_CMD_DONE_EN		(1u << 0)
#define	 M_ILLEGAL_CMD_EN	(1u << 2)
#define	 M_CMD_CANCEL_EN	(1u << 4)
#define	 M_CMD_ABORT_EN		(1u << 5)
#define	 M_IO_DATA_DEASSERT_EN	(1u << 22)
#define	 M_IO_DATA_ASSERT_EN	(1u << 23)
#define	 M_RX_FIFO_RD_ERR_EN	(1u << 24)
#define	 M_RX_FIFO_WR_ERR_EN	(1u << 25)
#define	 M_RX_FIFO_WATERMARK_EN	(1u << 26)
#define	 M_RX_FIFO_LAST_EN	(1u << 27)
#define	 M_TX_FIFO_RD_ERR_EN	(1u << 28)
#define	 M_TX_FIFO_WR_ERR_EN	(1u << 29)
#define	 M_TX_FIFO_WATERMARK_EN	(1u << 30)
#define	 M_COMMON_GENI_M_IRQ_EN	(0x7eu | M_IO_DATA_DEASSERT_EN | \
				 M_IO_DATA_ASSERT_EN | M_RX_FIFO_RD_ERR_EN | \
				 M_RX_FIFO_WR_ERR_EN | M_TX_FIFO_RD_ERR_EN | \
				 M_TX_FIFO_WR_ERR_EN)
#define	SE_GENI_S_CMD0		0x630
#define	 S_OPCODE_MSK		(0x1fu << 27)
#define	 S_PARAMS_MSK		0x07ffffffu
#define	SE_GENI_S_CMD_CTRL_REG	0x634
#define	 S_GENI_CMD_CANCEL	(1u << 2)
#define	 S_GENI_CMD_ABORT	(1u << 1)
#define	SE_GENI_S_IRQ_STATUS	0x640
#define	SE_GENI_S_IRQ_EN	0x644
#define	SE_GENI_S_IRQ_CLEAR	0x648
#define	 S_CMD_DONE_EN		(1u << 0)
#define	 S_CMD_CANCEL_EN	(1u << 4)
#define	 S_CMD_ABORT_EN		(1u << 5)
#define	 S_GP_IRQ_0_EN		(1u << 9)
#define	 S_GP_IRQ_1_EN		(1u << 10)
#define	 S_GP_IRQ_2_EN		(1u << 11)
#define	 S_GP_IRQ_3_EN		(1u << 12)
#define	 S_RX_FIFO_RD_ERR_EN	(1u << 24)
#define	 S_RX_FIFO_WR_ERR_EN	(1u << 25)
#define	 S_RX_FIFO_WATERMARK_EN	(1u << 26)
#define	 S_RX_FIFO_LAST_EN	(1u << 27)
#define	 S_COMMON_GENI_S_IRQ_EN	(0x3eu | 0x3e00u | S_RX_FIFO_RD_ERR_EN | \
				 S_RX_FIFO_WR_ERR_EN)
#define	SE_GENI_TX_FIFOn	0x700
#define	SE_GENI_RX_FIFOn	0x780
#define	SE_GENI_TX_FIFO_STATUS	0x800
#define	SE_GENI_RX_FIFO_STATUS	0x804
#define	 RX_LAST		(1u << 31)
#define	 RX_LAST_BYTE_VALID(x)	(((x) >> 28) & 0x7)
#define	 RX_FIFO_WC_MSK		0x01ffffffu
#define	SE_GENI_TX_WATERMARK_REG 0x80c
#define	SE_GENI_RX_WATERMARK_REG 0x810
#define	SE_GENI_RX_RFR_WATERMARK_REG 0x814
#define	SE_GENI_IOS		0x908
#define	 IO2_DATA_IN		(1u << 1)
#define	SE_DMA_TX_IRQ_STAT	0xc40
#define	SE_DMA_TX_IRQ_CLR	0xc44
#define	SE_DMA_RX_IRQ_STAT	0xd40
#define	SE_DMA_RX_IRQ_CLR	0xd44
#define	SE_GSI_EVENT_EN		0xe18
#define	SE_IRQ_EN		0xe1c
#define	 SE_IRQ_ALL		0xfu	/* DMA RX, DMA TX, GENI M, GENI S */
#define	SE_HW_PARAM_0		0xe24
#define	SE_HW_PARAM_1		0xe28
#define	SE_DMA_GENERAL_CFG	0xe30
#define	 DMA_GENERAL_CGC_ON	0xfu

/* UART (drivers/tty/serial/qcom_geni_serial.c) */
#define	SE_UART_LOOPBACK_CFG	0x22c
#define	 RX_TX_CTS_RTS_SORTED	0x3u	/* TX to RX, RTS to CTS inside */
#define	SE_UART_TX_TRANS_CFG	0x25c
#define	 UART_TX_PAR_EN		(1u << 0)
#define	 UART_CTS_MASK		(1u << 1)
#define	SE_UART_TX_WORD_LEN	0x268
#define	SE_UART_TX_STOP_BIT_LEN	0x26c
#define	SE_UART_TX_TRANS_LEN	0x270
#define	SE_UART_RX_TRANS_CFG	0x280
#define	 UART_RX_PAR_EN		(1u << 4)
#define	SE_UART_RX_WORD_LEN	0x28c
#define	SE_UART_RX_STALE_CNT	0x294
#define	SE_UART_TX_PARITY_CFG	0x2a4
#define	SE_UART_RX_PARITY_CFG	0x2a8
#define	 PAR_CALC_EN		(1u << 0)
#define	 PAR_ODD		0x01u
#define	SE_UART_MANUAL_RFR	0x2ac
#define	 UART_MANUAL_RFR_EN	(1u << 31)
#define	 UART_RFR_NOT_READY	(1u << 1)
#define	UART_START_TX		0x1
#define	UART_START_READ		0x1
#define	UART_RX_WM		2
#define	STALE_TIMEOUT		16
#define	DEFAULT_BITS_PER_CHAR	10

/* the QUP v3 wrapper */
#define	QUPV3_HW_VER_REG	0x4

/* GCC_QUPV3_WRAP0_S3_CLK (dt-bindings: 98), gcc-sm6115.c */
#define	GCC_S3_CLK_INDEX	98
#define	GCC_S3_CBCR		0x1f4d4		/* halt_reg */
#define	 CBCR_CLK_OFF		(1u << 31)
#define	GCC_S3_RCG		0x1f4d8		/* cmd_rcgr */
#define	GCC_VOTE		0x7900c		/* APCS clock branch votes */
#define	 GCC_VOTE_S3		(1u << 13)

/* TLMM pins (pinctrl-msm.c): CTL at 0x1000 * gpio of the pin's tile */
#define	TLMM_FUNC_SHIFT		2
#define	TLMM_DRV_SHIFT		6
#define	TLMM_MASK		0x3ffu		/* pull, function, drive, OE */
#define	TLMM_NO_PULL		0
#define	TLMM_PULL_DOWN		1
#define	TLMM_KEEPER		2
#define	TLMM_PULL_UP		3

static int geni_clk_enable = 1;
TUNABLE_INT("hw.qcom_geni_uart.clk_enable", &geni_clk_enable);
static int geni_pinmux = 1;
TUNABLE_INT("hw.qcom_geni_uart.pinmux", &geni_pinmux);
static int geni_set_rate = 1;
TUNABLE_INT("hw.qcom_geni_uart.set_rate", &geni_set_rate);

struct geni_softc {
	struct uart_softc base;
	volatile uint32_t *gcc_rcg_page;	/* GCC + 0x1f000 */
	volatile uint32_t *gcc_vote_page;	/* GCC + 0x79000 */
	uint32_t	qup_version;
	u_int		tx_depth, rx_depth;	/* FIFO words */
	u_int		baud;
	uint32_t	rate;			/* the engine's clock */
	u_int		clk_index, clk_div;
	bool		iflow, oflow, rts;
	int		loopback;
	uint32_t	m_irq_en;
	u_long		rx_bytes, tx_bytes, overruns, parity, breaks, illegal;
	char		pins[96];		/* the pins' CTL, as found */
};

/* --- before the engine: clock and pins ------------------------------------ */

static volatile uint32_t *
map_reg(vm_paddr_t pa)
{
	return ((volatile uint32_t *)pmap_mapdev(trunc_page(pa), PAGE_SIZE));
}

static void
unmap_reg(volatile uint32_t *page)
{
	if (page != NULL)
		pmap_unmapdev((void *)(uintptr_t)page, PAGE_SIZE);
}

#define	REG(page, off)	((page)[((off) & PAGE_MASK) / 4])

/* The GCC node and the clock's index, if it is the clock known here */
static int
geni_gcc(phandle_t node, vm_paddr_t *gcc)
{
	pcell_t cells[2];
	phandle_t g;
	vm_size_t size;
	vm_paddr_t pa;

	if (OF_getencprop(node, "clocks", cells, sizeof(cells)) !=
	    sizeof(cells))
		return (ENOENT);
	g = OF_node_from_xref(cells[0]);
	if (!ofw_bus_node_is_compatible(g, "qcom,gcc-sm6115") ||
	    cells[1] != GCC_S3_CLK_INDEX)
		return (EOPNOTSUPP);
	if (ofw_reg_to_paddr(g, 0, &pa, &size, NULL) != 0)
		return (ENXIO);
	*gcc = pa;
	return (0);
}

/*
 * The serial engine clock: 0 if it runs (voted on first if allowed), else
 * ENXIO: then no engine register may be touched.
 */
static int
geni_clock_check(device_t dev, phandle_t node, bool quiet)
{
	volatile uint32_t *rcg, *vote;
	vm_paddr_t gcc;
	uint32_t cbcr;
	int error, i;

	if ((error = geni_gcc(node, &gcc)) != 0) {
		if (!quiet)
			device_printf(dev, "clock: not GCC_QUPV3_WRAP0_S3_CLK "
			    "of the SM6115 (%d): taken as running\n", error);
		return (0);
	}
	rcg = map_reg(gcc + GCC_S3_CBCR);
	vote = map_reg(gcc + GCC_VOTE);
	cbcr = REG(rcg, GCC_S3_CBCR);
	if (!quiet)
		device_printf(dev, "clock: CBCR %#x (%s), vote %#x, RCG cfg %#x "
		    "m %#x n %#x: %ju Hz\n", cbcr, cbcr & CBCR_CLK_OFF ?
		    "off" : "on", REG(vote, GCC_VOTE), REG(rcg, GCC_S3_RCG +
		    RCG_CFG), REG(rcg, GCC_S3_RCG + RCG_M), REG(rcg, GCC_S3_RCG +
		    RCG_N), (uintmax_t)geni_rcg_rate(REG(rcg, GCC_S3_RCG +
		    RCG_CFG), REG(rcg, GCC_S3_RCG + RCG_M), REG(rcg,
		    GCC_S3_RCG + RCG_N)));
	if ((cbcr & CBCR_CLK_OFF) != 0 && geni_clk_enable) {
		REG(vote, GCC_VOTE) |= GCC_VOTE_S3;
		wmb();
		for (i = 0; i < 200 && (REG(rcg, GCC_S3_CBCR) & CBCR_CLK_OFF);
		    i++)
			DELAY(5);
		cbcr = REG(rcg, GCC_S3_CBCR);
		device_printf(dev, "clock: voted on: CBCR %#x (%s)\n", cbcr,
		    cbcr & CBCR_CLK_OFF ? "still off" : "on");
	}
	unmap_reg(rcg);
	unmap_reg(vote);
	if (cbcr & CBCR_CLK_OFF) {
		device_printf(dev, "the serial engine's clock is off: not "
		    "touched (boot menu: keep 4a8c000.serial runtime active, "
		    "or hw.qcom_geni_uart.clk_enable=1)\n");
		return (ENXIO);
	}
	return (0);
}

/* function 1 for qup3 on gpio8-11 (pinctrl-sm6115.c, EAST tile) */
static int
geni_pin_function(u_int gpio, const char *func)
{
	if (gpio >= 8 && gpio <= 11 && strcmp(func, "qup3") == 0)
		return (1);
	if (strcmp(func, "gpio") == 0)
		return (0);
	return (-1);
}

/* the "default" pinctrl state into the TLMM's CTL registers */
static void
geni_pinmux_apply(struct geni_softc *gsc, device_t dev, phandle_t node)
{
	char func[16], pins[16];
	phandle_t state, tlmm, child;
	pcell_t xref, drv;
	vm_paddr_t east;
	vm_size_t size;
	volatile uint32_t *p;
	uint32_t before, ctl, pull;
	u_int gpio;
	int idx, f;

	if (OF_getencprop(node, "pinctrl-0", &xref, sizeof(xref)) <= 0)
		return;
	state = OF_node_from_xref(xref);
	tlmm = OF_parent(state);
	if (!ofw_bus_node_is_compatible(tlmm, "qcom,sm6115-tlmm") ||
	    ofw_bus_find_string_index(tlmm, "reg-names", "east", &idx) != 0 ||
	    ofw_reg_to_paddr(tlmm, idx, &east, &size, NULL) != 0) {
		device_printf(dev, "pins: no SM6115 TLMM, left as they are\n");
		return;
	}
	for (child = OF_child(state); child != 0; child = OF_peer(child)) {
		if (OF_getprop(child, "pins", pins, sizeof(pins)) <= 0 ||
		    OF_getprop(child, "function", func, sizeof(func)) <= 0)
			continue;
		pins[sizeof(pins) - 1] = func[sizeof(func) - 1] = '\0';
		if (strncmp(pins, "gpio", 4) != 0)
			continue;
		gpio = strtoul(pins + 4, NULL, 10);
		if ((f = geni_pin_function(gpio, func)) < 0) {
			device_printf(dev, "pins: %s %s unknown here\n", pins,
			    func);
			continue;
		}
		pull = TLMM_NO_PULL;
		if (OF_hasprop(child, "bias-pull-up"))
			pull = TLMM_PULL_UP;
		else if (OF_hasprop(child, "bias-pull-down"))
			pull = TLMM_PULL_DOWN;
		else if (OF_hasprop(child, "bias-bus-hold"))
			pull = TLMM_KEEPER;
		if (OF_getencprop(child, "drive-strength", &drv,
		    sizeof(drv)) <= 0 || drv < 2 || drv > 16)
			drv = 2;
		p = map_reg(east + 0x1000 * gpio);
		before = p[0];
		ctl = (before & ~TLMM_MASK) | pull | (uint32_t)f <<
		    TLMM_FUNC_SHIFT | (drv / 2 - 1) << TLMM_DRV_SHIFT;
		if (geni_pinmux && ctl != before)
			p[0] = ctl;
		wmb();
		snprintf(gsc->pins + strlen(gsc->pins), sizeof(gsc->pins) -
		    strlen(gsc->pins), "%s%u:%#x>%#x", gsc->pins[0] ? " " : "",
		    gpio, before, p[0]);
		unmap_reg(p);
	}
	device_printf(dev, "pins (CTL before>after%s): %s\n", geni_pinmux ?
	    "" : ", pinmux off", gsc->pins);
}

/* --- the engine ------------------------------------------------------------- */

static uint32_t
qup_version(phandle_t node)
{
	phandle_t qup = OF_parent(node);
	volatile uint32_t *p;
	vm_paddr_t pa;
	vm_size_t size;
	uint32_t v;

	if (!ofw_bus_node_is_compatible(qup, "qcom,geni-se-qup") ||
	    ofw_reg_to_paddr(qup, 0, &pa, &size, NULL) != 0)
		return (0);
	p = map_reg(pa + QUPV3_HW_VER_REG);
	v = REG(p, pa + QUPV3_HW_VER_REG);
	unmap_reg(p);
	return (v);
}

static void
fifo_depths(struct uart_bas *bas, uint32_t ver, u_int *tx, u_int *rx)
{
	uint32_t major = ver >> 28, minor = (ver >> 16) & 0xfff;
	uint32_t mask = (major == 3 && minor >= 10) || major > 3 ? 0xffu : 0x3fu;

	*tx = (uart_getreg(bas, SE_HW_PARAM_0) >> 16) & mask;
	*rx = (uart_getreg(bas, SE_HW_PARAM_1) >> 16) & mask;
}

static bool
poll_bit(struct uart_bas *bas, int reg, uint32_t bit, bool set)
{
	int i;

	for (i = 0; i < 2000; i++) {
		if (((uart_getreg(bas, reg) & bit) != 0) == set)
			return (true);
		DELAY(10);
	}
	return (false);
}

/* qcom_geni_serial_stop_rx_fifo with the abort */
static void
stop_rx(struct uart_bas *bas)
{
	if (!(uart_getreg(bas, SE_GENI_STATUS) & S_GENI_CMD_ACTIVE))
		return;
	uart_setreg(bas, SE_GENI_S_CMD_CTRL_REG, S_GENI_CMD_CANCEL);
	poll_bit(bas, SE_GENI_S_IRQ_STATUS, S_CMD_CANCEL_EN, true);
	uart_setreg(bas, SE_GENI_S_IRQ_CLEAR,
	    uart_getreg(bas, SE_GENI_S_IRQ_STATUS));
	if (uart_getreg(bas, SE_GENI_STATUS) & S_GENI_CMD_ACTIVE) {
		uart_setreg(bas, SE_GENI_S_CMD_CTRL_REG, S_GENI_CMD_ABORT);
		poll_bit(bas, SE_GENI_S_CMD_CTRL_REG, S_GENI_CMD_ABORT, false);
		uart_setreg(bas, SE_GENI_S_IRQ_CLEAR, S_CMD_DONE_EN |
		    S_CMD_ABORT_EN);
		uart_setreg(bas, GENI_FORCE_DEFAULT_REG, FORCE_DEFAULT);
	}
}

static void
start_rx(struct uart_bas *bas)
{
	uint32_t s;

	stop_rx(bas);
	s = uart_getreg(bas, SE_GENI_S_CMD0);
	s &= ~(S_OPCODE_MSK | S_PARAMS_MSK);
	s |= (uint32_t)UART_START_READ << 27;
	uart_setreg(bas, SE_GENI_S_CMD0, s);
	uart_setreg(bas, SE_GENI_S_IRQ_EN, uart_getreg(bas, SE_GENI_S_IRQ_EN) |
	    S_RX_FIFO_WATERMARK_EN | S_RX_FIFO_LAST_EN);
}

/* qcom_geni_serial_cancel_tx_cmd */
static void
cancel_tx(struct uart_bas *bas)
{
	if (!(uart_getreg(bas, SE_GENI_STATUS) & M_GENI_CMD_ACTIVE))
		return;
	uart_setreg(bas, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_CANCEL);
	if (!poll_bit(bas, SE_GENI_M_IRQ_STATUS, M_CMD_CANCEL_EN, true)) {
		uart_setreg(bas, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_ABORT);
		poll_bit(bas, SE_GENI_M_IRQ_STATUS, M_CMD_ABORT_EN, true);
		uart_setreg(bas, SE_GENI_M_IRQ_CLEAR, M_CMD_ABORT_EN);
	}
	uart_setreg(bas, SE_GENI_M_IRQ_CLEAR, M_CMD_CANCEL_EN);
}

/* the manual RFR: RTS deasserted only without input flow control */
static void
set_rfr(struct geni_softc *gsc)
{
	struct uart_bas *bas = &gsc->base.sc_bas;

	uart_setreg(bas, SE_UART_MANUAL_RFR, !gsc->iflow && !gsc->rts ?
	    UART_MANUAL_RFR_EN | UART_RFR_NOT_READY : 0);
}

static void
set_cts(struct geni_softc *gsc)
{
	struct uart_bas *bas = &gsc->base.sc_bas;
	uint32_t v = uart_getreg(bas, SE_UART_TX_TRANS_CFG);

	if (gsc->oflow)
		v &= ~UART_CTS_MASK;
	else
		v |= UART_CTS_MASK;
	uart_setreg(bas, SE_UART_TX_TRANS_CFG, v);
}

/* The engine's clock and divider for baud (geni_serial_set_rate) */
static int
set_rate(struct geni_softc *gsc, u_int baud)
{
	struct uart_bas *bas = &gsc->base.sc_bas;
	const struct geni_clk_freq *f;
	struct geni_rcg_regs r;
	volatile uint32_t *p = gsc->gcc_rcg_page;
	uint32_t sampling, cfg, rate;
	u_int idx, div;
	int i;

	if (geni_uart_clk_match(baud, gsc->qup_version, &idx, &div) != 0)
		return (EINVAL);
	f = &geni_clk_table[idx];
	if (p != NULL && geni_set_rate) {
		geni_rcg_regs(f, &r);
		if (r.mnd) {
			REG(p, GCC_S3_RCG + RCG_M) = (REG(p, GCC_S3_RCG +
			    RCG_M) & ~RCG_MND_MASK) | r.m;
			REG(p, GCC_S3_RCG + RCG_N) = (REG(p, GCC_S3_RCG +
			    RCG_N) & ~RCG_MND_MASK) | r.n;
			REG(p, GCC_S3_RCG + RCG_D) = (REG(p, GCC_S3_RCG +
			    RCG_D) & ~RCG_MND_MASK) | r.d;
		}
		cfg = REG(p, GCC_S3_RCG + RCG_CFG);
		REG(p, GCC_S3_RCG + RCG_CFG) = (cfg & ~r.cfg_mask) | r.cfg;
		wmb();
		REG(p, GCC_S3_RCG + RCG_CMD) |= RCG_CMD_UPDATE;
		for (i = 0; i < 500 && (REG(p, GCC_S3_RCG + RCG_CMD) &
		    RCG_CMD_UPDATE); i++)
			DELAY(1);
		if (REG(p, GCC_S3_RCG + RCG_CMD) & RCG_CMD_UPDATE) {
			device_printf(gsc->base.sc_dev, "the RCG did not "
			    "update to %u Hz\n", f->rate);
			return (EIO);
		}
		rate = f->rate;
	} else {
		/* the clock as it runs, only the divider ours */
		rate = p != NULL ? (uint32_t)geni_rcg_rate(REG(p, GCC_S3_RCG +
		    RCG_CFG), REG(p, GCC_S3_RCG + RCG_M), REG(p, GCC_S3_RCG +
		    RCG_N)) : bas->rclk;
		sampling = GENI_UART_OVERSAMPLING;
		if (gsc->qup_version >= QUP_SE_VERSION_2_5)
			sampling /= 2;
		if (rate == 0)
			return (EINVAL);
		div = howmany(rate, baud * sampling);
		if (div == 0 || div > GENI_CLK_DIV_MAX)
			return (EINVAL);
	}
	uart_setreg(bas, GENI_SER_M_CLK_CFG, GENI_SER_CLK_EN |
	    div << GENI_CLK_DIV_SHIFT);
	uart_setreg(bas, GENI_SER_S_CLK_CFG, GENI_SER_CLK_EN |
	    div << GENI_CLK_DIV_SHIFT);
	uart_setreg(bas, SE_GENI_CLK_SEL, idx & GENI_CLK_SEL_MASK);
	gsc->rate = rate;
	gsc->clk_index = idx;
	gsc->clk_div = div;
	gsc->baud = baud;
	bas->rclk = rate;
	return (0);
}

/* --- the low-level console interface: never a console ---------------------- */

static int geni_probe(struct uart_bas *bas) { return (ENXIO); }
static void geni_init(struct uart_bas *bas, int baud, int databits,
    int stopbits, int parity) { }
static void geni_term(struct uart_bas *bas) { }
static void geni_putc(struct uart_bas *bas, int c) { }
static int geni_rxready(struct uart_bas *bas) { return (0); }
static int geni_getc(struct uart_bas *bas, struct mtx *mtx) { return (-1); }

static struct uart_ops uart_geni_ops = {
	.probe = geni_probe,
	.init = geni_init,
	.term = geni_term,
	.putc = geni_putc,
	.rxready = geni_rxready,
	.getc = geni_getc,
};

/* --- the uart(4) class ------------------------------------------------------ */

static int
geni_bus_probe(struct uart_softc *sc)
{
	struct uart_bas *bas = &sc->sc_bas;
	phandle_t node = ofw_bus_get_node(sc->sc_dev);
	uint32_t fw, ver;
	u_int tx, rx;

	/* the softc is uart(4)'s own here: nothing of ours yet */
	if (geni_clock_check(sc->sc_dev, node, false) != 0)
		return (ENXIO);
	fw = uart_getreg(bas, GENI_FW_REVISION_RO);
	if (FW_REV_PROTOCOL(fw) != GENI_SE_UART) {
		device_printf(sc->sc_dev, "serial engine runs protocol %u "
		    "(firmware %#x), not UART\n", FW_REV_PROTOCOL(fw), fw);
		return (ENXIO);
	}
	if (uart_getreg(bas, GENI_IF_DISABLE_RO) & FIFO_IF_DISABLE) {
		device_printf(sc->sc_dev, "FIFOs disabled (GPI DMA only)\n");
		return (ENXIO);
	}
	ver = qup_version(node);
	fifo_depths(bas, ver, &tx, &rx);
	if (tx == 0 || rx == 0) {
		device_printf(sc->sc_dev, "no FIFO depths (%u, %u)\n", tx, rx);
		return (ENXIO);
	}
	sc->sc_txfifosz = tx * 4;
	sc->sc_rxfifosz = rx * 4;
	device_set_desc(sc->sc_dev, "Qualcomm GENI UART");
	return (0);
}

/* dev.uart.N.loopback: TX into RX and RTS into CTS inside the engine */
static int
geni_loopback_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct geni_softc *gsc = arg1;
	struct uart_softc *sc = &gsc->base;
	int error, v = gsc->loopback;

	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	uart_lock(sc->sc_hwmtx);
	gsc->loopback = v != 0;
	uart_setreg(&sc->sc_bas, SE_UART_LOOPBACK_CFG, gsc->loopback ?
	    RX_TX_CTS_RTS_SORTED : 0);
	uart_unlock(sc->sc_hwmtx);
	return (0);
}

static int
geni_bus_attach(struct uart_softc *sc)
{
	struct geni_softc *gsc = (struct geni_softc *)sc;
	struct uart_bas *bas = &sc->sc_bas;
	phandle_t node = ofw_bus_get_node(sc->sc_dev);
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid_list *kids;
	uint32_t cfg0, cfg1;
	vm_paddr_t gcc;
	int error;

	if (geni_gcc(node, &gcc) == 0) {
		gsc->gcc_rcg_page = map_reg(gcc + GCC_S3_RCG);
		gsc->gcc_vote_page = map_reg(gcc + GCC_VOTE);
	}
	geni_pinmux_apply(gsc, sc->sc_dev, node);
	gsc->qup_version = qup_version(node);
	fifo_depths(bas, gsc->qup_version, &gsc->tx_depth, &gsc->rx_depth);
	device_printf(sc->sc_dev, "QUP v%u.%u.%u, protocol %u, FIFOs %u/%u "
	    "words\n", gsc->qup_version >> 28, (gsc->qup_version >> 16) &
	    0xfff, gsc->qup_version & 0xffff,
	    FW_REV_PROTOCOL(uart_getreg(bas, GENI_FW_REVISION_RO)),
	    gsc->tx_depth, gsc->rx_depth);

	uart_lock(sc->sc_hwmtx);
	/* qcom_geni_serial_port_setup */
	stop_rx(bas);
	cancel_tx(bas);
	uart_setreg(bas, SE_UART_RX_STALE_CNT,
	    DEFAULT_BITS_PER_CHAR * STALE_TIMEOUT);
	geni_packing(8, 4, false, &cfg0, &cfg1);
	uart_setreg(bas, SE_GENI_TX_PACKING_CFG0, cfg0);
	uart_setreg(bas, SE_GENI_TX_PACKING_CFG1, cfg1);
	uart_setreg(bas, SE_GENI_RX_PACKING_CFG0, cfg0);
	uart_setreg(bas, SE_GENI_RX_PACKING_CFG1, cfg1);
	uart_setreg(bas, SE_GENI_BYTE_GRAN, 0);		/* 4 x 8 */
	/* geni_se_init: interrupts cleared, clocks gating, FIFO mode */
	uart_setreg(bas, SE_GSI_EVENT_EN, 0);
	uart_setreg(bas, SE_GENI_M_IRQ_CLEAR, 0xffffffff);
	uart_setreg(bas, SE_GENI_S_IRQ_CLEAR, 0xffffffff);
	uart_setreg(bas, SE_DMA_TX_IRQ_CLR, 0xffffffff);
	uart_setreg(bas, SE_DMA_RX_IRQ_CLR, 0xffffffff);
	uart_setreg(bas, SE_GENI_CGC_CTRL,
	    uart_getreg(bas, SE_GENI_CGC_CTRL) | DEFAULT_CGC_EN);
	uart_setreg(bas, SE_DMA_GENERAL_CFG,
	    uart_getreg(bas, SE_DMA_GENERAL_CFG) | DMA_GENERAL_CGC_ON);
	uart_setreg(bas, GENI_OUTPUT_CTRL, DEFAULT_IO_OUTPUT_CTRL_MSK);
	uart_setreg(bas, GENI_FORCE_DEFAULT_REG, FORCE_DEFAULT);
	uart_setreg(bas, SE_IRQ_EN, uart_getreg(bas, SE_IRQ_EN) | SE_IRQ_ALL);
	uart_setreg(bas, SE_GENI_DMA_MODE_EN,
	    uart_getreg(bas, SE_GENI_DMA_MODE_EN) & ~GENI_DMA_MODE_EN);
	uart_setreg(bas, SE_GENI_RX_WATERMARK_REG, UART_RX_WM);
	uart_setreg(bas, SE_GENI_RX_RFR_WATERMARK_REG, gsc->rx_depth - 2);
	uart_setreg(bas, SE_GENI_TX_WATERMARK_REG, 0);
	gsc->m_irq_en = M_COMMON_GENI_M_IRQ_EN | M_CMD_DONE_EN;
	uart_setreg(bas, SE_GENI_M_IRQ_EN, gsc->m_irq_en);
	uart_setreg(bas, SE_GENI_S_IRQ_EN, S_COMMON_GENI_S_IRQ_EN);
	uart_setreg(bas, SE_UART_LOOPBACK_CFG, 0);
	/* RTS deasserted until opened; CTS honoured */
	gsc->iflow = false;
	gsc->oflow = true;
	gsc->rts = false;
	set_rfr(gsc);
	set_cts(gsc);
	error = set_rate(gsc, 115200);
	uart_setreg(bas, SE_UART_TX_WORD_LEN, 8);
	uart_setreg(bas, SE_UART_RX_WORD_LEN, 8);
	uart_setreg(bas, SE_UART_TX_STOP_BIT_LEN, 0);
	start_rx(bas);
	uart_unlock(sc->sc_hwmtx);
	if (error != 0)
		device_printf(sc->sc_dev, "115200 baud: %d\n", error);

	sc->sc_hwiflow = 1;
	sc->sc_hwoflow = 1;

	ctx = device_get_sysctl_ctx(sc->sc_dev);
	kids = SYSCTL_CHILDREN(device_get_sysctl_tree(sc->sc_dev));
	SYSCTL_ADD_U32(ctx, kids, OID_AUTO, "qup_version", CTLFLAG_RD,
	    &gsc->qup_version, 0, "QUP v3 hardware version");
	SYSCTL_ADD_U32(ctx, kids, OID_AUTO, "clock", CTLFLAG_RD, &gsc->rate,
	    0, "Serial engine clock (Hz)");
	SYSCTL_ADD_UINT(ctx, kids, OID_AUTO, "clock_div", CTLFLAG_RD,
	    &gsc->clk_div, 0, "Serial engine clock divider");
	SYSCTL_ADD_UINT(ctx, kids, OID_AUTO, "baud", CTLFLAG_RD, &gsc->baud,
	    0, "Baud rate");
	SYSCTL_ADD_ULONG(ctx, kids, OID_AUTO, "rx_bytes", CTLFLAG_RD,
	    &gsc->rx_bytes, "Bytes received");
	SYSCTL_ADD_ULONG(ctx, kids, OID_AUTO, "tx_bytes", CTLFLAG_RD,
	    &gsc->tx_bytes, "Bytes sent");
	SYSCTL_ADD_ULONG(ctx, kids, OID_AUTO, "overruns", CTLFLAG_RD,
	    &gsc->overruns, "Receive overruns");
	SYSCTL_ADD_ULONG(ctx, kids, OID_AUTO, "parity_errors", CTLFLAG_RD,
	    &gsc->parity, "Parity or framing errors");
	SYSCTL_ADD_ULONG(ctx, kids, OID_AUTO, "breaks", CTLFLAG_RD,
	    &gsc->breaks, "Breaks");
	SYSCTL_ADD_ULONG(ctx, kids, OID_AUTO, "illegal_cmds", CTLFLAG_RD,
	    &gsc->illegal, "Illegal command interrupts");
	SYSCTL_ADD_STRING(ctx, kids, OID_AUTO, "pins", CTLFLAG_RD, gsc->pins,
	    0, "The pins' TLMM CTL before>after");
	SYSCTL_ADD_PROC(ctx, kids, OID_AUTO, "loopback",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, gsc, 0,
	    geni_loopback_sysctl, "I", "Internal loopback (TX to RX, RTS to "
	    "CTS), for a test without the chip");
	return (0);
}

static int
geni_bus_detach(struct uart_softc *sc)
{
	struct geni_softc *gsc = (struct geni_softc *)sc;
	struct uart_bas *bas = &sc->sc_bas;

	uart_lock(sc->sc_hwmtx);
	uart_setreg(bas, SE_GENI_M_IRQ_EN, 0);
	uart_setreg(bas, SE_GENI_S_IRQ_EN, 0);
	stop_rx(bas);
	cancel_tx(bas);
	uart_unlock(sc->sc_hwmtx);
	unmap_reg(gsc->gcc_rcg_page);
	unmap_reg(gsc->gcc_vote_page);
	gsc->gcc_rcg_page = gsc->gcc_vote_page = NULL;
	return (0);
}

static int
geni_bus_flush(struct uart_softc *sc, int what)
{
	struct uart_bas *bas = &sc->sc_bas;
	u_int wc;

	uart_lock(sc->sc_hwmtx);
	if (what & UART_FLUSH_TRANSMITTER) {
		cancel_tx(bas);
		sc->sc_txbusy = 0;
	}
	if (what & UART_FLUSH_RECEIVER) {
		wc = uart_getreg(bas, SE_GENI_RX_FIFO_STATUS) & RX_FIFO_WC_MSK;
		while (wc-- > 0)
			(void)uart_getreg(bas, SE_GENI_RX_FIFOn);
	}
	uart_unlock(sc->sc_hwmtx);
	return (0);
}

#define	SIGCHG(c, i, s, d)				\
	if (c) {					\
		i |= (i & s) ? s : s | d;		\
	} else {					\
		i = (i & s) ? (i & ~s) | d : i;		\
	}

static int
geni_bus_getsig(struct uart_softc *sc)
{
	uint32_t new, old, sig, ios;

	do {
		old = sc->sc_hwsig;
		sig = old;
		uart_lock(sc->sc_hwmtx);
		ios = uart_getreg(&sc->sc_bas, SE_GENI_IOS);
		uart_unlock(sc->sc_hwmtx);
		/* CTS: IO2 low (qcom_geni_serial_get_mctrl); no DSR, DCD */
		SIGCHG(!(ios & IO2_DATA_IN), sig, SER_CTS, SER_DCTS);
		SIGCHG(1, sig, SER_DSR, SER_DDSR);
		SIGCHG(1, sig, SER_DCD, SER_DDCD);
		new = sig & ~SER_MASK_DELTA;
	} while (!atomic_cmpset_32(&sc->sc_hwsig, old, new));
	return (sig);
}

static int
geni_bus_setsig(struct uart_softc *sc, int sig)
{
	struct geni_softc *gsc = (struct geni_softc *)sc;
	uint32_t new, old;

	do {
		old = sc->sc_hwsig;
		new = old;
		if (sig & SER_DDTR)
			new = (new & ~SER_DTR) | (sig & (SER_DTR | SER_DDTR));
		if (sig & SER_DRTS)
			new = (new & ~SER_RTS) | (sig & (SER_RTS | SER_DRTS));
	} while (!atomic_cmpset_32(&sc->sc_hwsig, old, new));
	if (sig & SER_DRTS) {
		uart_lock(sc->sc_hwmtx);
		gsc->rts = (new & SER_RTS) != 0;
		set_rfr(gsc);
		uart_unlock(sc->sc_hwmtx);
	}
	return (0);
}

static int
geni_bus_ioctl(struct uart_softc *sc, int request, intptr_t data)
{
	struct geni_softc *gsc = (struct geni_softc *)sc;
	int error = 0;

	uart_lock(sc->sc_hwmtx);
	switch (request) {
	case UART_IOCTL_IFLOW:
		gsc->iflow = data != 0;
		set_rfr(gsc);
		break;
	case UART_IOCTL_OFLOW:
		gsc->oflow = data != 0;
		set_cts(gsc);
		break;
	case UART_IOCTL_BAUD:
		*(int *)data = (int)gsc->baud;
		break;
	default:
		error = EINVAL;
		break;
	}
	uart_unlock(sc->sc_hwmtx);
	return (error);
}

static int
geni_bus_ipend(struct uart_softc *sc)
{
	struct geni_softc *gsc = (struct geni_softc *)sc;
	struct uart_bas *bas = &sc->sc_bas;
	uint32_t m, s;
	int ipend = 0;

	uart_lock(sc->sc_hwmtx);
	m = uart_getreg(bas, SE_GENI_M_IRQ_STATUS);
	s = uart_getreg(bas, SE_GENI_S_IRQ_STATUS);
	uart_setreg(bas, SE_GENI_M_IRQ_CLEAR, m);
	uart_setreg(bas, SE_GENI_S_IRQ_CLEAR, s);
	uart_setreg(bas, SE_DMA_TX_IRQ_CLR,
	    uart_getreg(bas, SE_DMA_TX_IRQ_STAT));
	uart_setreg(bas, SE_DMA_RX_IRQ_CLR,
	    uart_getreg(bas, SE_DMA_RX_IRQ_STAT));
	uart_unlock(sc->sc_hwmtx);

	if (m & M_ILLEGAL_CMD_EN)
		gsc->illegal++;
	if ((m & M_CMD_DONE_EN) && sc->sc_txbusy)
		ipend |= SER_INT_TXIDLE;
	if (m & (M_IO_DATA_ASSERT_EN | M_IO_DATA_DEASSERT_EN))
		ipend |= SER_INT_SIGCHG;
	if (s & S_RX_FIFO_WR_ERR_EN) {
		gsc->overruns++;
		ipend |= SER_INT_OVERRUN;
	}
	if (s & (S_GP_IRQ_0_EN | S_GP_IRQ_1_EN))
		gsc->parity++;
	if (s & (S_GP_IRQ_2_EN | S_GP_IRQ_3_EN)) {
		gsc->breaks++;
		ipend |= SER_INT_BREAK;
	}
	if (s & (S_RX_FIFO_WATERMARK_EN | S_RX_FIFO_LAST_EN))
		ipend |= SER_INT_RXREADY;
	return (ipend);
}

static int
geni_bus_param(struct uart_softc *sc, int baudrate, int databits,
    int stopbits, int parity)
{
	struct geni_softc *gsc = (struct geni_softc *)sc;
	struct uart_bas *bas = &sc->sc_bas;
	uint32_t txc, rxc, txp, rxp;
	int error;

	if (baudrate <= 0 || databits < 5 || databits > 8)
		return (EINVAL);
	uart_lock(sc->sc_hwmtx);
	error = set_rate(gsc, (u_int)baudrate);
	if (error != 0) {
		uart_unlock(sc->sc_hwmtx);
		return (error);
	}
	txc = uart_getreg(bas, SE_UART_TX_TRANS_CFG) & ~UART_TX_PAR_EN;
	rxc = uart_getreg(bas, SE_UART_RX_TRANS_CFG) & ~UART_RX_PAR_EN;
	txp = uart_getreg(bas, SE_UART_TX_PARITY_CFG) & ~(PAR_CALC_EN | 0x11u);
	rxp = uart_getreg(bas, SE_UART_RX_PARITY_CFG) & ~(PAR_CALC_EN | 0x11u);
	if (parity == UART_PARITY_ODD || parity == UART_PARITY_EVEN) {
		txc |= UART_TX_PAR_EN;
		rxc |= UART_RX_PAR_EN;
		txp |= PAR_CALC_EN | (parity == UART_PARITY_ODD ? PAR_ODD : 0);
		rxp |= PAR_CALC_EN | (parity == UART_PARITY_ODD ? PAR_ODD : 0);
	}
	uart_setreg(bas, SE_UART_TX_TRANS_CFG, txc);
	uart_setreg(bas, SE_UART_RX_TRANS_CFG, rxc);
	uart_setreg(bas, SE_UART_TX_PARITY_CFG, txp);
	uart_setreg(bas, SE_UART_RX_PARITY_CFG, rxp);
	uart_setreg(bas, SE_UART_TX_WORD_LEN, (uint32_t)databits);
	uart_setreg(bas, SE_UART_RX_WORD_LEN, (uint32_t)databits);
	uart_setreg(bas, SE_UART_TX_STOP_BIT_LEN, stopbits == 2 ? 2 : 0);
	set_cts(gsc);
	uart_unlock(sc->sc_hwmtx);
	return (0);
}

static int
geni_bus_receive(struct uart_softc *sc)
{
	struct geni_softc *gsc = (struct geni_softc *)sc;
	struct uart_bas *bas = &sc->sc_bas;
	uint32_t st, v;
	u_int wc, w, nb, b;

	uart_lock(sc->sc_hwmtx);
	for (;;) {
		st = uart_getreg(bas, SE_GENI_RX_FIFO_STATUS);
		wc = st & RX_FIFO_WC_MSK;
		if (wc == 0)
			break;
		for (w = 0; w < wc; w++) {
			v = uart_getreg(bas, SE_GENI_RX_FIFOn);
			nb = 4;
			if (w == wc - 1 && (st & RX_LAST) &&
			    RX_LAST_BYTE_VALID(st) != 0)
				nb = RX_LAST_BYTE_VALID(st);
			for (b = 0; b < nb; b++) {
				if (uart_rx_put(sc, (v >> (8 * b)) & 0xff) !=
				    0) {
					/* the buffer is full: dropped */
					sc->sc_rxbuf[sc->sc_rxput] =
					    UART_STAT_OVERRUN;
					gsc->overruns++;
					break;
				}
				gsc->rx_bytes++;
			}
		}
	}
	uart_unlock(sc->sc_hwmtx);
	return (0);
}

static int
geni_bus_transmit(struct uart_softc *sc)
{
	struct geni_softc *gsc = (struct geni_softc *)sc;
	struct uart_bas *bas = &sc->sc_bas;
	uint32_t w;
	int i, j, n = sc->sc_txdatasz;

	if (n <= 0)
		return (0);
	uart_lock(sc->sc_hwmtx);
	/* one command for the lot; it fits the FIFO (sc_txfifosz) */
	uart_setreg(bas, SE_GENI_M_IRQ_CLEAR, M_CMD_DONE_EN);
	uart_setreg(bas, SE_UART_TX_TRANS_LEN, (uint32_t)n);
	uart_setreg(bas, SE_GENI_M_CMD0, (uint32_t)UART_START_TX <<
	    M_OPCODE_SHFT);
	for (i = 0; i < n; i += 4) {
		w = 0;
		for (j = 0; j < 4 && i + j < n; j++)
			w |= (uint32_t)sc->sc_txbuf[i + j] << (8 * j);
		uart_setreg(bas, SE_GENI_TX_FIFOn, w);
	}
	gsc->tx_bytes += (u_long)n;
	sc->sc_txbusy = 1;
	uart_unlock(sc->sc_hwmtx);
	return (0);
}

static bool
geni_bus_txbusy(struct uart_softc *sc)
{
	struct uart_bas *bas = &sc->sc_bas;
	bool busy;

	uart_lock(sc->sc_hwmtx);
	busy = (uart_getreg(bas, SE_GENI_STATUS) & M_GENI_CMD_ACTIVE) != 0 ||
	    uart_getreg(bas, SE_GENI_TX_FIFO_STATUS) != 0;
	uart_unlock(sc->sc_hwmtx);
	return (busy);
}

static void
geni_bus_grab(struct uart_softc *sc)
{
}

static void
geni_bus_ungrab(struct uart_softc *sc)
{
}

static kobj_method_t geni_methods[] = {
	KOBJMETHOD(uart_probe,		geni_bus_probe),
	KOBJMETHOD(uart_attach,		geni_bus_attach),
	KOBJMETHOD(uart_detach,		geni_bus_detach),
	KOBJMETHOD(uart_flush,		geni_bus_flush),
	KOBJMETHOD(uart_getsig,		geni_bus_getsig),
	KOBJMETHOD(uart_ioctl,		geni_bus_ioctl),
	KOBJMETHOD(uart_ipend,		geni_bus_ipend),
	KOBJMETHOD(uart_param,		geni_bus_param),
	KOBJMETHOD(uart_receive,	geni_bus_receive),
	KOBJMETHOD(uart_setsig,		geni_bus_setsig),
	KOBJMETHOD(uart_transmit,	geni_bus_transmit),
	KOBJMETHOD(uart_txbusy,		geni_bus_txbusy),
	KOBJMETHOD(uart_grab,		geni_bus_grab),
	KOBJMETHOD(uart_ungrab,		geni_bus_ungrab),
	KOBJMETHOD_END
};

static struct uart_class uart_geni_class = {
	"qcom_geni",
	geni_methods,
	sizeof(struct geni_softc),
	.uc_ops = &uart_geni_ops,
	.uc_range = 0x4000,
	.uc_rclk = 7372800,
	.uc_rshift = 0,
	.uc_riowidth = 4,
};

static struct ofw_compat_data compat_data[] = {
	{ "qcom,geni-uart",	(uintptr_t)&uart_geni_class },
	{ NULL,			(uintptr_t)NULL },
};
UART_FDT_CLASS_AND_DEVICE(compat_data);
