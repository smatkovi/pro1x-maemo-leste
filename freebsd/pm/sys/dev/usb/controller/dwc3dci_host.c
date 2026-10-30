/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The SM6115's DWC3 in host mode (freebsd/pm/README.md, stage 4c): after
 * the glue's bring-up (dwc3dci_pro1x.c) the core's port capability goes to
 * host, as Linux's dwc3 core does for dr_mode host, and FreeBSD's xHCI
 * driver takes the core through a child device "xhci" (generic_xhci_attach,
 * as FreeBSD's own dwc3.c does it) whose register window and interrupt
 * are the glue's. With hw.dwc3dci.usb3=1 the QMP USB3 PHY is brought up
 * first, Linux's qmp_usbc_com_init and qmp_usbc_usb_power_on: its supplies,
 * its two resets (GCC 0x1b000, 0x1b008), the com_aux clock (0x1a054), the
 * PCS powered down, the lane select by the plug's orientation
 * (hw.dwc3dci.cc2, from pro1x_typec), the serdes table, the pipe clock
 * (0x1a058), the tables of both lanes and the PCS, out of reset, started,
 * PHYSTATUS clear. Without it the port is USB 2 only (UTMI as the PIPE
 * clock, as in device mode).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/condvar.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/sx.h>

#include <vm/vm.h>
#include <vm/pmap.h>

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
#include <dev/usb/controller/xhci.h>
#include <dev/usb/controller/generic_xhci.h>

#include <dev/usb/controller/dwc3dci.h>
#include <dev/usb/controller/dwc3dci_qmp.h>

int	qcom_rpm_regulator(phandle_t xref, bool on, const char *who);
int	dwc3dci_host_start(struct dwc3dci_softc *sc, int usb3, int cc2);
struct resource *dwc3dci_alloc_resource(device_t, device_t, int, int *,
	    rman_res_t, rman_res_t, rman_res_t, u_int);
int	dwc3dci_release_resource(device_t, device_t, struct resource *);
int	dwc3dci_activate_resource(device_t, device_t, struct resource *);

#define	GCC_USB3_PHY_BCR	0x1b000
#define	GCC_USB3PHY_PHY_BCR	0x1b008
#define	GCC_USB3_COM_AUX_CBCR	0x1a054
#define	GCC_USB3_PIPE_CBCR	0x1a058

static inline uint32_t
rd(uint8_t *b, uint32_t o)
{
	return (*(volatile uint32_t *)(b + o));
}

static inline void
wr(uint8_t *b, uint32_t o, uint32_t v)
{
	*(volatile uint32_t *)(b + o) = v;
	__asm __volatile("dsb sy" ::: "memory");
}

static void
qmp_tbl(uint8_t *base, const struct qmp_init *t, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		wr(base, t[i].off, t[i].val);
}

static int
qmp_start(struct dwc3dci_softc *sc, phandle_t qmp, int cc2)
{
	static const char *const supplies[] = { "vdda-phy-supply",
	    "vdda-pll-supply" };
	uint8_t *g = sc->sc_gcc, *p;
	bus_addr_t pa;
	bus_size_t size;
	pcell_t sup;
	u_int i;

	if (ofw_reg_to_paddr(qmp, 0, &pa, &size, NULL) != 0)
		return (ENXIO);
	p = pmap_mapdev(pa, size);
	for (i = 0; i < nitems(supplies); i++)
		if (OF_getencprop(qmp, supplies[i], &sup, sizeof(sup)) > 0)
			qcom_rpm_regulator(sup, true, "dwc3dci");
	/* the resets, then the clocks (cfg_ahb and ref are on already) */
	wr(g, GCC_USB3_PHY_BCR, rd(g, GCC_USB3_PHY_BCR) | 1);
	wr(g, GCC_USB3PHY_PHY_BCR, rd(g, GCC_USB3PHY_PHY_BCR) | 1);
	DELAY(100);
	wr(g, GCC_USB3PHY_PHY_BCR, rd(g, GCC_USB3PHY_PHY_BCR) & ~1u);
	wr(g, GCC_USB3_PHY_BCR, rd(g, GCC_USB3_PHY_BCR) & ~1u);
	wr(g, GCC_USB3_COM_AUX_CBCR, rd(g, GCC_USB3_COM_AUX_CBCR) | 1);
	/* qmp_usbc_usb_power_on */
	wr(p, QMP_PCS + QMP_PCS_POWER_DOWN, rd(p, QMP_PCS + QMP_PCS_POWER_DOWN) |
	    QMP_SW_PWRDN);
	wr(p, QMP_PCS_MISC, QMP_SW_PORTSELECT_MUX |
	    (cc2 ? QMP_SW_PORTSELECT_VAL : 0));
	qmp_tbl(p + QMP_SERDES, qmp_serdes_tbl, nitems(qmp_serdes_tbl));
	wr(g, GCC_USB3_PIPE_CBCR, rd(g, GCC_USB3_PIPE_CBCR) | 1);
	qmp_tbl(p + QMP_TX, qmp_tx_tbl, nitems(qmp_tx_tbl));
	qmp_tbl(p + QMP_RX, qmp_rx_tbl, nitems(qmp_rx_tbl));
	qmp_tbl(p + QMP_TX2, qmp_tx_tbl, nitems(qmp_tx_tbl));
	qmp_tbl(p + QMP_RX2, qmp_rx_tbl, nitems(qmp_rx_tbl));
	qmp_tbl(p + QMP_PCS, qmp_pcs_tbl, nitems(qmp_pcs_tbl));
	wr(p, QMP_PCS + QMP_PCS_SW_RESET, rd(p, QMP_PCS + QMP_PCS_SW_RESET) &
	    ~QMP_SW_RESET);
	wr(p, QMP_PCS + QMP_PCS_START, rd(p, QMP_PCS + QMP_PCS_START) |
	    QMP_SERDES_START | QMP_PCS_START_BIT);
	for (i = 0; i < 1000; i++) {
		if ((rd(p, QMP_PCS + QMP_PCS_STATUS) & QMP_PHYSTATUS) == 0)
			return (0);
		DELAY(10);
	}
	return (ETIMEDOUT);
}

int
dwc3dci_host_start(struct dwc3dci_softc *sc, int usb3, int cc2)
{
	phandle_t core, qmp;
	pcell_t phys[2];
	uint32_t v;
	int i, err;
	device_t child;

	core = ofw_bus_find_compatible(ofw_bus_get_node(sc->sc_dev),
	    "snps,dwc3");
	if (usb3) {
		qmp = OF_getencprop(core, "phys", phys, sizeof(phys)) ==
		    sizeof(phys) ? OF_node_from_xref(phys[1]) : -1;
		err = qmp > 0 ? qmp_start(sc, qmp, cc2) : ENXIO;
		device_printf(sc->sc_dev, "QMP USB3 PHY %s (lanes %s)\n",
		    err == 0 ? "started" : "failed", cc2 ? "CC2" : "CC1");
		if (err != 0)
			return (err);
	}
	/* the core: soft reset, then the port capability to host */
	bus_write_4(sc->sc_io_res, DWC3_DCTL, DWC3_DCTL_CSFTRST);
	for (i = 0; i < 5000 &&
	    (bus_read_4(sc->sc_io_res, DWC3_DCTL) & DWC3_DCTL_CSFTRST); i++)
		DELAY(10);
	v = bus_read_4(sc->sc_io_res, DWC3_GCTL) & ~(DWC3_GCTL_PRTCAP_MASK |
	    DWC3_GCTL_SCALEDOWN_MASK | DWC3_GCTL_DISSCRAMBLE);
	bus_write_4(sc->sc_io_res, DWC3_GCTL, v | DWC3_GCTL_PRTCAPDIR(1));
	v = bus_read_4(sc->sc_io_res, DWC3_GUSB2PHYCFG(0));
	bus_write_4(sc->sc_io_res, DWC3_GUSB2PHYCFG(0),
	    v & ~(DWC3_GUSB2PHYCFG_SUSPHY | DWC3_GUSB2PHYCFG_ENBLSLPM));
	child = device_add_child(sc->sc_dev, "xhci", DEVICE_UNIT_ANY);
	if (child == NULL)
		return (ENXIO);
	bus_attach_children(sc->sc_dev);
	device_printf(sc->sc_dev, "host mode (xHCI), USB %s\n",
	    usb3 ? "3 and 2" : "2 only");
	return (0);
}

/* --- the child's resources are the glue's ------------------------------- */

struct resource *
dwc3dci_alloc_resource(device_t dev, device_t child, int type, int *rid,
    rman_res_t start, rman_res_t end, rman_res_t count, u_int flags)
{
	struct dwc3dci_softc *sc = device_get_softc(dev);

	if (device_get_parent(child) != dev)
		return (bus_generic_alloc_resource(dev, child, type, rid, start,
		    end, count, flags));
	if (type == SYS_RES_MEMORY && *rid == 0)
		return (sc->sc_io_res);
	if (type == SYS_RES_IRQ && *rid == 0)
		return (sc->sc_irq_res);
	return (NULL);
}

int
dwc3dci_release_resource(device_t dev, device_t child, struct resource *r)
{
	return (0);
}

int
dwc3dci_activate_resource(device_t dev, device_t child, struct resource *r)
{
	return (0);
}

/* the xHCI on the DWC3 */
static int
pro1x_xhci_probe(device_t dev)
{
	device_set_desc(dev, "DWC_usb3 xHCI (SM6115)");
	return (BUS_PROBE_DEFAULT);
}

static device_method_t pro1x_xhci_methods[] = {
	DEVMETHOD(device_probe,		pro1x_xhci_probe),
	DEVMETHOD(device_attach,	generic_xhci_attach),
	DEVMETHOD(device_detach,	generic_xhci_detach),
	DEVMETHOD(device_shutdown,	bus_generic_shutdown),
	DEVMETHOD_END
};

static driver_t pro1x_xhci_driver = {
	"xhci",
	pro1x_xhci_methods,
	sizeof(struct xhci_softc),
};

DRIVER_MODULE(xhci, dwc3dci, pro1x_xhci_driver, 0, 0);
