/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * dwc3dci on the SM6115 (F(x)tec Pro1X): the Qualcomm wrapper around the
 * DWC_usb3 ("qcom,sm6115-dwc3" with its "snps,dwc3" child), brought up the
 * way Linux 7.1 does it (dwc3-qcom.c, phy-qcom-qusb2.c, gcc-sm6115.c), as
 * far as Linux has not left it running at the kexec:
 *
 *  - the PHY's supplies (L4A, L12A, L15A) on through the RPM, the stream
 *    0x120 of the apps SMMU without translation (qcom_dsp.ko's
 *    qcom_rpm_regulator and qcom_smmu_bypass: FreeBSD hands the core
 *    physical addresses);
 *  - the GCC: the GDSC at 0x1a004, the branch clocks (CBCR bit 0 on, bit
 *    31 off-status) at 0x1a084 (config NoC), 0x1a080 (system NoC),
 *    0x1a010 (master), 0x1a014 (sleep), 0x1a018 (mock UTMI), 0x9f000
 *    (reference), 0x1d008 (the PHY's AHB), the master clock's RCG at
 *    0x1a01c to 66.67 MHz (GPLL0_OUT_AUX2, source 2, divider 4.5), the
 *    block reset 0x1a000 pulsed;
 *  - the QUSB2 PHY (0x1613000): its reset 0x1c000 pulsed, powered down,
 *    sm6115_init_tbl, powered up, the single-ended reference clock
 *    (PLL_TEST CLK_REF_SEL), PLL lock in PLL_STATUS;
 *  - the wrapper (QSCRATCH): UTMI as the PIPE clock (no USB3 PHY here:
 *    high speed only) and VBUS forced valid for device mode
 *    (dwc3_qcom_select_utmi_clk, dwc3_qcom_vbus_override_enable).
 *
 * Off unless hw.dwc3dci.enable=1 (and only when the module is loaded:
 * pro1x_usb.ko). dev.dwc3dci.0.bringup says what was found and done.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/condvar.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/sx.h>
#include <sys/sysctl.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>

#include <machine/bus.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_subr.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usb_core.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_util.h>
#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>

#include <dev/usb/controller/dwc3dci.h>

/* freebsd/dsp's qcom_dsp.ko */
int	qcom_smmu_bypass(phandle_t smmu, uint32_t sid, uint32_t mask,
	    const char *who);
int	qcom_rpm_regulator(phandle_t xref, bool on, const char *who);

#define	GCC_USB30_BCR		0x1a000
#define	GCC_USB30_GDSCR		0x1a004
#define	GDSC_SW_COLLAPSE	(1u << 0)
#define	GDSC_PWR_ON		(1u << 31)
#define	GCC_USB30_MASTER_CMD	0x1a01c
#define	GCC_USB30_MASTER_CFG	0x1a020
#define	RCG_CMD_UPDATE		(1u << 0)
#define	RCG_CFG_SRC_DIV_MASK	0x71fu
#define	RCG_CFG_66MHZ		((2u << 8) | 8u)	/* AUX2, (4.5 * 2) - 1 */
#define	CBCR_CLK_ENABLE		(1u << 0)
#define	CBCR_CLK_OFF		(1u << 31)
#define	GCC_QUSB2PHY_BCR	0x1c000
#define	BCR_BLK_ARES		(1u << 0)

static const struct {
	uint32_t	off;
	const char	*name;
} gcc_clocks[] = {
	{ 0x1a084, "cfg_noc" },
	{ 0x1a080, "sys_noc" },
	{ 0x1a010, "master" },
	{ 0x1a014, "sleep" },
	{ 0x1a018, "mock_utmi" },
	{ 0x9f000, "clkref" },
	{ 0x1d008, "ahb2phy" },
};

/* QUSB2 (msm8996 layout, sm6115_init_tbl) */
#define	QUSB2_PLL_TEST		0x04
#define	QUSB2_PLL_TEST_CLK_REF_SEL (1u << 7)
#define	QUSB2_PLL_STATUS	0x38
#define	QUSB2_PLL_LOCKED	(1u << 5)
#define	QUSB2_PORT_POWERDOWN	0xb4
#define	QUSB2_POWER_DOWN	(1u << 0)
#define	QUSB2_DISABLE_CTRL	((1u << 5) | (1u << 1) | (1u << 0))
static const struct { uint16_t off; uint8_t val; } qusb2_tbl[] = {
	{ 0x80, 0xf8 }, { 0x84, 0x53 }, { 0x88, 0x81 }, { 0x8c, 0x17 },
	{ 0x08, 0x30 }, { 0x0c, 0x79 }, { 0x10, 0x21 },
	{ 0x9c, 0x14 },
	{ 0x1c, 0x9f }, { 0x18, 0x00 },
};

/* QSCRATCH (dwc3-qcom.c) */
#define	QSCRATCH_GENERAL_CFG	0x08
#define	PIPE_UTMI_CLK_SEL	(1u << 0)
#define	PIPE3_PHYSTATUS_SW	(1u << 3)
#define	PIPE_UTMI_CLK_DIS	(1u << 8)
#define	QSCRATCH_HS_PHY_CTRL	0x10
#define	UTMI_OTG_VBUS_VALID	(1u << 20)
#define	SW_SESSVLD_SEL		(1u << 28)
#define	QSCRATCH_SS_PHY_CTRL	0x30
#define	LANE0_PWR_PRESENT	(1u << 24)

/* hw.dwc3dci.enable=1: attach (the kernel environment, freebsd/cmdline) */
static int dwc3dci_enable;
TUNABLE_INT("hw.dwc3dci.enable", &dwc3dci_enable);

static inline uint32_t
rd(uint8_t *base, uint32_t off)
{
	return (*(volatile uint32_t *)(base + off));
}

static inline void
wr(uint8_t *base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
	__asm __volatile("dsb sy" ::: "memory");
}

static int
poll(uint8_t *base, uint32_t off, uint32_t mask, uint32_t want, int us)
{
	for (; us > 0; us -= 10) {
		if ((rd(base, off) & mask) == want)
			return (0);
		DELAY(10);
	}
	return (ETIMEDOUT);
}

static void *
map_node(phandle_t node, int idx, bus_size_t *size)
{
	bus_addr_t pa;

	if (ofw_reg_to_paddr(node, idx, &pa, size, NULL) != 0)
		return (NULL);
	return (pmap_mapdev(pa, *size));
}

#define	NOTE(sc, ...) do {						\
	size_t _l = strlen((sc)->sc_bringup);				\
	snprintf((sc)->sc_bringup + _l, sizeof((sc)->sc_bringup) - _l,	\
	    __VA_ARGS__);						\
} while (0)

/* The GCC's part: power, clocks, the block reset */
static int
pro1x_gcc(struct dwc3dci_softc *sc)
{
	uint8_t *g = sc->sc_gcc;
	uint32_t v;
	u_int i, on = 0;

	v = rd(g, GCC_USB30_GDSCR);
	if ((v & GDSC_PWR_ON) == 0) {
		wr(g, GCC_USB30_GDSCR, v & ~GDSC_SW_COLLAPSE);
		if (poll(g, GCC_USB30_GDSCR, GDSC_PWR_ON, GDSC_PWR_ON,
		    100000) != 0) {
			NOTE(sc, "GDSC does not come on (%08x)", rd(g,
			    GCC_USB30_GDSCR));
			return (ENXIO);
		}
		NOTE(sc, "GDSC on, ");
	} else
		NOTE(sc, "GDSC was on, ");
	for (i = 0; i < nitems(gcc_clocks); i++) {
		v = rd(g, gcc_clocks[i].off);
		if ((v & CBCR_CLK_OFF) == 0)
			continue;
		wr(g, gcc_clocks[i].off, v | CBCR_CLK_ENABLE);
		if (poll(g, gcc_clocks[i].off, CBCR_CLK_OFF, 0, 10000) != 0) {
			NOTE(sc, "clock %s does not come on (%08x)",
			    gcc_clocks[i].name, rd(g, gcc_clocks[i].off));
			return (ENXIO);
		}
		on++;
	}
	NOTE(sc, "%u of %zu clocks switched on, ", on, nitems(gcc_clocks));
	v = rd(g, GCC_USB30_MASTER_CFG);
	if ((v & RCG_CFG_SRC_DIV_MASK) != RCG_CFG_66MHZ) {
		wr(g, GCC_USB30_MASTER_CFG, (v & ~RCG_CFG_SRC_DIV_MASK) |
		    RCG_CFG_66MHZ);
		wr(g, GCC_USB30_MASTER_CMD, rd(g, GCC_USB30_MASTER_CMD) |
		    RCG_CMD_UPDATE);
		if (poll(g, GCC_USB30_MASTER_CMD, RCG_CMD_UPDATE, 0,
		    10000) != 0)
			NOTE(sc, "master RCG update pending, ");
		NOTE(sc, "master %08x -> 66.67 MHz, ", v);
	}
	/* a fresh controller: Linux's dwc3-qcom pulses the reset at probe */
	wr(g, GCC_USB30_BCR, rd(g, GCC_USB30_BCR) | BCR_BLK_ARES);
	DELAY(100);
	wr(g, GCC_USB30_BCR, rd(g, GCC_USB30_BCR) & ~BCR_BLK_ARES);
	DELAY(100);
	return (0);
}

/* Linux's qusb2_phy_init for the SM6115 */
static int
pro1x_qusb2(struct dwc3dci_softc *sc)
{
	uint8_t *p = sc->sc_phy, *g = sc->sc_gcc;
	uint32_t test;
	u_int i;

	wr(g, GCC_QUSB2PHY_BCR, rd(g, GCC_QUSB2PHY_BCR) | BCR_BLK_ARES);
	DELAY(150);
	wr(g, GCC_QUSB2PHY_BCR, rd(g, GCC_QUSB2PHY_BCR) & ~BCR_BLK_ARES);
	wr(p, QUSB2_PORT_POWERDOWN, rd(p, QUSB2_PORT_POWERDOWN) |
	    QUSB2_DISABLE_CTRL);
	test = rd(p, QUSB2_PLL_TEST);
	for (i = 0; i < nitems(qusb2_tbl); i++)
		wr(p, qusb2_tbl[i].off, qusb2_tbl[i].val);
	wr(p, QUSB2_PORT_POWERDOWN, rd(p, QUSB2_PORT_POWERDOWN) &
	    ~QUSB2_POWER_DOWN);
	DELAY(160);
	/* no TCSR clock scheme in the device tree: single-ended (Linux) */
	wr(p, QUSB2_PLL_TEST, test | QUSB2_PLL_TEST_CLK_REF_SEL);
	(void)rd(p, QUSB2_PLL_TEST);
	DELAY(110);
	if ((rd(p, QUSB2_PLL_STATUS) & QUSB2_PLL_LOCKED) == 0) {
		NOTE(sc, "QUSB2 PLL not locked (%02x)", rd(p,
		    QUSB2_PLL_STATUS) & 0xff);
		return (ENXIO);
	}
	NOTE(sc, "QUSB2 PLL locked, ");
	return (0);
}

static void
pro1x_qscratch(struct dwc3dci_softc *sc)
{
	uint8_t *q = sc->sc_qscratch;

	wr(q, QSCRATCH_GENERAL_CFG, rd(q, QSCRATCH_GENERAL_CFG) |
	    PIPE_UTMI_CLK_DIS);
	DELAY(100);
	wr(q, QSCRATCH_GENERAL_CFG, rd(q, QSCRATCH_GENERAL_CFG) |
	    PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW);
	DELAY(100);
	wr(q, QSCRATCH_GENERAL_CFG, rd(q, QSCRATCH_GENERAL_CFG) &
	    ~PIPE_UTMI_CLK_DIS);
	wr(q, QSCRATCH_HS_PHY_CTRL, rd(q, QSCRATCH_HS_PHY_CTRL) |
	    UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL);
	wr(q, QSCRATCH_SS_PHY_CTRL, rd(q, QSCRATCH_SS_PHY_CTRL) |
	    LANE0_PWR_PRESENT);
}

static void *
dma_alloc(size_t size, uint64_t *pa)
{
	void *va;

	va = kmem_alloc_contig(round_page(size), M_WAITOK | M_ZERO, 0,
	    0xffffffffUL, PAGE_SIZE, 0, VM_MEMATTR_UNCACHEABLE);
	*pa = vtophys(va);
	return (va);
}

static int
dwc3dci_fdt_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "qcom,sm6115-dwc3"))
		return (ENXIO);
	if (!dwc3dci_enable)
		return (ENXIO);
	device_set_desc(dev, "DWC_usb3 device controller (SM6115)");
	return (BUS_PROBE_DEFAULT);
}

static int
dwc3dci_sysctl_stats(SYSCTL_HANDLER_ARGS)
{
	struct dwc3dci_softc *sc = arg1;
	char buf[256];

	snprintf(buf, sizeof(buf), "events %ju, resets %ju, connects %ju (%s "
	    "speed), setups %ju, TRBs %ju, command errors %ju; address %u, "
	    "DSTS %08x", (uintmax_t)sc->sc_n_events, (uintmax_t)sc->sc_n_resets,
	    (uintmax_t)sc->sc_n_connects, sc->sc_flags.status_high_speed ?
	    "high" : "full", (uintmax_t)sc->sc_n_setups,
	    (uintmax_t)sc->sc_n_trbs, (uintmax_t)sc->sc_n_cmderr,
	    sc->sc_dv_addr, bus_read_4(sc->sc_io_res, DWC3_DSTS));
	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}

static int
dwc3dci_fdt_attach(device_t dev)
{
	struct dwc3dci_softc *sc = device_get_softc(dev);
	phandle_t node, core, phy, gcc, sup;
	pcell_t cells[3];
	bus_size_t size;
	bus_addr_t pa;
	struct resource *r;
	int rid, n, irq, err, i;
	static const char *const supplies[] = { "vdd-supply",
	    "vdda-pll-supply", "vdda-phy-dpdm-supply" };

	sc->sc_dev = dev;
	node = ofw_bus_get_node(dev);
	core = ofw_bus_find_compatible(node, "snps,dwc3");
	if (core <= 0) {
		device_printf(dev, "no snps,dwc3 below it\n");
		return (ENXIO);
	}
	rid = 0;
	r = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (r == NULL)
		return (ENXIO);
	sc->sc_qscratch = rman_get_virtual(r);
	if (sc->sc_qscratch == NULL)
		sc->sc_qscratch = pmap_mapdev(rman_get_start(r), rman_get_size(r));

	/* the core's registers and interrupt (the child node's) */
	if (ofw_reg_to_paddr(core, 0, &pa, &size, NULL) != 0)
		return (ENXIO);
	bus_set_resource(dev, SYS_RES_MEMORY, 1, pa, size);
	rid = 1;
	sc->sc_io_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	n = OF_getencprop(core, "interrupts", cells, sizeof(cells));
	irq = n >= 12 ? ofw_bus_map_intr(dev, ofw_bus_find_iparent(core), 3,
	    cells) : -1;
	if (sc->sc_io_res == NULL || irq < 0) {
		device_printf(dev, "no core registers or interrupt\n");
		return (ENXIO);
	}
	bus_set_resource(dev, SYS_RES_IRQ, 0, irq, 1);
	rid = 0;
	sc->sc_irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
	    RF_ACTIVE);
	gcc = ofw_bus_find_compatible(OF_finddevice("/"), "qcom,gcc-sm6115");
	phy = OF_getencprop(core, "phys", cells, sizeof(cells[0])) > 0 ?
	    OF_node_from_xref(cells[0]) : -1;
	if (gcc <= 0 || phy <= 0 || sc->sc_irq_res == NULL ||
	    (sc->sc_gcc = map_node(gcc, 0, &size)) == NULL ||
	    (sc->sc_phy = map_node(phy, 0, &size)) == NULL) {
		device_printf(dev, "no GCC, PHY or interrupt\n");
		return (ENXIO);
	}

	/* the PHY's supplies and the SMMU stream (qcom_dsp.ko) */
	for (i = 0; i < (int)nitems(supplies); i++)
		if (OF_getencprop(phy, supplies[i], &sup, sizeof(sup)) > 0 &&
		    (err = qcom_rpm_regulator(sup, true, "dwc3dci")) != 0)
			NOTE(sc, "%s refused (%d), ", supplies[i], err);
	cells[2] = 0;
	if (OF_getencprop(core, "iommus", cells, sizeof(cells)) >= 8) {
		err = qcom_smmu_bypass(cells[0], cells[1], cells[2], "dwc3dci");
		NOTE(sc, "SMMU stream %#x %s, ", cells[1], err == 0 ?
		    "untranslated" : "refused");
	}
	if (pro1x_gcc(sc) != 0 || pro1x_qusb2(sc) != 0) {
		device_printf(dev, "bring-up: %s\n", sc->sc_bringup);
		return (ENXIO);
	}
	pro1x_qscratch(sc);
	NOTE(sc, "UTMI as PIPE clock, VBUS valid");
	device_printf(dev, "bring-up: %s\n", sc->sc_bringup);

	/* the event buffer, a TRB and a bounce buffer per endpoint */
	sc->sc_ev = dma_alloc(DWC3DCI_EVBUF_SIZE, &sc->sc_ev_pa);
	{
		uint64_t tpa;
		volatile struct dwc3_trb *t = dma_alloc(PAGE_SIZE, &tpa);

		for (i = 0; i < DWC3DCI_PHYS_MAX; i++) {
			sc->sc_ep[i].trb = &t[i];
			sc->sc_ep[i].trb_pa = tpa + i * sizeof(*t);
			sc->sc_ep[i].buf_size = i < 2 ? DWC3DCI_EP0_BUF :
			    DWC3DCI_EP_BUF;
			sc->sc_ep[i].buf = dma_alloc(sc->sc_ep[i].buf_size,
			    &sc->sc_ep[i].buf_pa);
		}
	}

	/* the USB bus, as the other device controllers do it */
	sc->sc_bus.parent = dev;
	sc->sc_bus.devices = sc->sc_devices;
	sc->sc_bus.devices_max = DWC3DCI_MAX_DEVICES;
	sc->sc_bus.dma_bits = 32;
	if (usb_bus_mem_alloc_all(&sc->sc_bus, USB_GET_DMA_TAG(dev), NULL))
		return (ENOMEM);
	sc->sc_bus.bdev = device_add_child(dev, "usbus", DEVICE_UNIT_ANY);
	if (sc->sc_bus.bdev == NULL)
		return (ENXIO);
	device_set_ivars(sc->sc_bus.bdev, &sc->sc_bus);
	SYSCTL_ADD_STRING(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "bringup",
	    CTLFLAG_RD, sc->sc_bringup, 0, "What the bring-up found and did");
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "stats",
	    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, sc, 0,
	    dwc3dci_sysctl_stats, "A", "Events, resets, connections, setups");
	err = bus_setup_intr(dev, sc->sc_irq_res, INTR_TYPE_TTY | INTR_MPSAFE,
	    NULL, dwc3dci_interrupt, sc, &sc->sc_intr_hdl);
	if (err != 0)
		return (err);
	if (dwc3dci_init(sc) != 0)
		return (ENXIO);
	return (device_probe_and_attach(sc->sc_bus.bdev));
}

static int
dwc3dci_fdt_detach(device_t dev)
{
	struct dwc3dci_softc *sc = device_get_softc(dev);
	int err;

	if ((err = bus_generic_detach(dev)) != 0)
		return (err);
	if (sc->sc_io_res != NULL)
		dwc3dci_uninit(sc);
	if (sc->sc_intr_hdl != NULL)
		bus_teardown_intr(dev, sc->sc_irq_res, sc->sc_intr_hdl);
	usb_bus_mem_free_all(&sc->sc_bus, NULL);
	return (0);
}

static device_method_t dwc3dci_fdt_methods[] = {
	DEVMETHOD(device_probe,		dwc3dci_fdt_probe),
	DEVMETHOD(device_attach,	dwc3dci_fdt_attach),
	DEVMETHOD(device_detach,	dwc3dci_fdt_detach),
	DEVMETHOD(device_suspend,	bus_generic_suspend),
	DEVMETHOD(device_resume,	bus_generic_resume),
	DEVMETHOD(device_shutdown,	bus_generic_shutdown),
	DEVMETHOD_END
};

static driver_t dwc3dci_fdt_driver = {
	.name = "dwc3dci",
	.methods = dwc3dci_fdt_methods,
	.size = sizeof(struct dwc3dci_softc),
};

DRIVER_MODULE(dwc3dci, simplebus, dwc3dci_fdt_driver, 0, 0);
MODULE_DEPEND(dwc3dci, usb, 1, 1, 1);
MODULE_DEPEND(dwc3dci, qcom_dsp, 1, 1, 1);
MODULE_VERSION(dwc3dci, 1);
