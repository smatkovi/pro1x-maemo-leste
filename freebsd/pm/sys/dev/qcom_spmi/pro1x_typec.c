/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * pro1x_typec: the Pro1X's USB-C port as the PMI632's Type-C block sees it
 * (freebsd/pm/README.md, stage 4b). The device tree (mainline 7.1,
 * pmi632.dtsi and sm6115-fxtec-pro1x.dts): "qcom,pmi632-typec" at 0x1500
 * of the PMI632 (SPMI slave 2), a dual-role usb-c-connector (power and
 * data), PD disabled, its high-speed port to the DWC3, its SuperSpeed
 * port to the QMP USB3 PHY; the VBUS output "qcom,pmi632-vbus-reg" at
 * 0x1100 (500 mA to 1 A). The registers are Linux's
 * drivers/usb/typec/tcpm/qcom/qcom_pmic_typec_port.c and
 * drivers/regulator/qcom_usb_vbus-regulator.c:
 *
 *   MISC_STATUS 0x0b   CC_ATTACHED (0), CC_ORIENTATION (1: CC2),
 *                      TYPEC_VBUS_DETECT (5), SNK_SRC_MODE (6: we source)
 *   SNK_STATUS 0x06    the host's or charger's Rp: default, 1.5 A, 3 A
 *   SRC_STATUS 0x08    what a sink shows us: Rd (a device), Ra, audio
 *   MODE_CFG 0x44      the power role: 0 dual, EN_SNK_ONLY, EN_SRC_ONLY,
 *                      TYPEC_DISABLE_CMD
 *   VBUS: CMD_OTG 0x40 OTG_EN (0), OTG_CFG 0x53 OTG_EN_SRC_CFG (1),
 *         OTG_CURRENT_LIMIT_CFG 0x52 (500 mA steps from 500 mA)
 *
 * Woken by the block's interrupts (attach-detach, cc-state-change,
 * vbus-change through the SPMI arbiter; hw.pro1x_typec.poll_ms=N polls as
 * well, the fallback, and without interrupts every 250 ms), read only by
 * default: what it
 * sees goes to dev.pro1x_typec.0.state and, on every change, to devd as
 * system PRO1X, subsystem TYPEC, type ATTACH or DETACH with
 * role=device|host|audio|debug, orientation=cc1|cc2, vbus=0|1, rp=...
 * Writes only when asked: hw.pro1x_typec.drp=1 puts the port into dual
 * role if Linux left it otherwise, hw.pro1x_typec.vbus=1 switches the
 * VBUS output on while a device is attached (host role) and off on its
 * detach. Off unless hw.pro1x_typec.enable=1 (pro1x_typec.ko).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/sysctl.h>

#include <machine/bus.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/qcom_spmi/qcom_spmi.h>
#include <dev/qcom_spmi/pro1x_typec.h>

static SYSCTL_NODE(_hw, OID_AUTO, pro1x_typec, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "The Pro1X's USB-C port");
static int pro1x_typec_enable;
SYSCTL_INT(_hw_pro1x_typec, OID_AUTO, enable, CTLFLAG_RDTUN,
    &pro1x_typec_enable, 0, "Attach to the PMI632's Type-C block");
static int pro1x_typec_drp;
SYSCTL_INT(_hw_pro1x_typec, OID_AUTO, drp, CTLFLAG_RDTUN, &pro1x_typec_drp,
    0, "Set dual role at attach");
static int pro1x_typec_vbus;
SYSCTL_INT(_hw_pro1x_typec, OID_AUTO, vbus, CTLFLAG_RWTUN, &pro1x_typec_vbus,
    0, "VBUS out while a device is attached");
/* 0: the block's interrupts only; N: also a look every N ms (fallback) */
static int pro1x_typec_poll_ms;
SYSCTL_INT(_hw_pro1x_typec, OID_AUTO, poll_ms, CTLFLAG_RWTUN,
    &pro1x_typec_poll_ms, 0, "Polling interval (0: interrupts only)");

struct typec_softc {
	device_t	dev;
	struct mtx	mtx;
	struct callout	poll;
	uint16_t	base;		/* the Type-C block */
	uint16_t	vbus_base;	/* the VBUS output, 0: none */
	uint32_t	vbus_max_ua;
	struct typec_state st;
	int		vbus_on;
	int		nirq;
	struct resource	*irq[3];
	void		*ih[3];
	uint64_t	changes, irqs;
	char		state[160];
};

static int
typec_rd(struct typec_softc *sc, uint16_t off, uint8_t *v)
{
	return (qcom_spmi_read(sc->dev, sc->base + off, v, 1));
}

static int
typec_wr(struct typec_softc *sc, uint16_t addr, uint8_t v)
{
	return (qcom_spmi_write(sc->dev, addr, &v, 1));
}

/* VBUS out as Linux's regulator: by the command register, a current limit */
static void
typec_vbus(struct typec_softc *sc, int on)
{
	uint8_t cfg, lim;

	if (sc->vbus_base == 0 || sc->vbus_on == on)
		return;
	if (on) {
		if (qcom_spmi_read(sc->dev, sc->vbus_base + TYPEC_OTG_CFG, &cfg,
		    1) == 0)
			typec_wr(sc, sc->vbus_base + TYPEC_OTG_CFG,
			    cfg & ~TYPEC_OTG_EN_SRC_CFG);
		lim = typec_vbus_limit(sc->vbus_max_ua);
		typec_wr(sc, sc->vbus_base + TYPEC_OTG_CURRENT_LIMIT_CFG, lim);
	}
	typec_wr(sc, sc->vbus_base + TYPEC_CMD_OTG, on ? TYPEC_OTG_EN : 0);
	sc->vbus_on = on;
	device_printf(sc->dev, "VBUS out %s\n", on ? "on" : "off");
}

static void
typec_poll(void *arg)
{
	struct typec_softc *sc = arg;
	struct typec_state st;
	uint8_t misc, snk, src;
	char buf[128];

	mtx_assert(&sc->mtx, MA_OWNED);
	if (typec_rd(sc, TYPEC_MISC_STATUS, &misc) != 0 ||
	    typec_rd(sc, TYPEC_SNK_STATUS, &snk) != 0 ||
	    typec_rd(sc, TYPEC_SRC_STATUS, &src) != 0)
		goto again;
	typec_decode(misc, snk, src, &st);
	if (st.attached != sc->st.attached || st.role != sc->st.role ||
	    st.cc2 != sc->st.cc2 || st.vbus != sc->st.vbus) {
		sc->changes++;
		typec_format(&st, buf, sizeof(buf));
		snprintf(sc->state, sizeof(sc->state), "%s (misc %02x snk %02x "
		    "src %02x)", buf, misc, snk, src);
		device_printf(sc->dev, "%s\n", sc->state);
		if (st.attached != sc->st.attached || st.role != sc->st.role)
			devctl_notify("PRO1X", "TYPEC", st.attached ? "ATTACH" :
			    "DETACH", buf);
		if (pro1x_typec_vbus)
			typec_vbus(sc, st.attached && st.role == TYPEC_ROLE_HOST);
		sc->st = st;
	}
again:
	/* the interrupts call again; polling only as the fallback */
	if (pro1x_typec_poll_ms > 0 || sc->nirq == 0)
		callout_reset(&sc->poll, MAX(pro1x_typec_poll_ms, 250) * hz /
		    1000, typec_poll, sc);
}

/* attach-detach, cc-state-change, vbus-change: a look 20 ms later */
static void
typec_intr(void *arg)
{
	struct typec_softc *sc = arg;

	mtx_lock(&sc->mtx);
	sc->irqs++;
	callout_reset(&sc->poll, MAX(hz / 50, 1), typec_poll, sc);
	mtx_unlock(&sc->mtx);
}

/* The block's interrupts through the SPMI arbiter, as qcom_pon's */
static void
typec_irqs(struct typec_softc *sc)
{
	static const char *const names[] = { "attach-detach",
	    "cc-state-change", "vbus-change" };
	phandle_t node = ofw_bus_get_node(sc->dev);
	pcell_t *cells;
	ssize_t n;
	int i, idx, irq, rid;

	n = OF_getencprop_alloc_multi(node, "interrupts", sizeof(pcell_t),
	    (void **)&cells);
	if (n <= 0)
		return;
	for (i = 0; i < (int)nitems(names); i++) {
		if (ofw_bus_find_string_index(node, "interrupt-names", names[i],
		    &idx) != 0 || (idx + 1) * 4 > n)
			continue;
		irq = ofw_bus_map_intr(sc->dev, qcom_spmi_intr_xref(sc->dev), 4,
		    &cells[idx * 4]);
		rid = sc->nirq;
		bus_set_resource(sc->dev, SYS_RES_IRQ, rid, irq, 1);
		sc->irq[rid] = bus_alloc_resource_any(sc->dev, SYS_RES_IRQ, &rid,
		    RF_ACTIVE);
		if (sc->irq[rid] == NULL || bus_setup_intr(sc->dev, sc->irq[rid],
		    INTR_TYPE_MISC | INTR_MPSAFE, NULL, typec_intr, sc,
		    &sc->ih[rid]) != 0) {
			device_printf(sc->dev, "interrupt %s not set up\n",
			    names[i]);
			continue;
		}
		sc->nirq++;
	}
	OF_prop_free(cells);
}

static int
typec_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "qcom,pmi632-typec") ||
	    !pro1x_typec_enable)
		return (ENXIO);
	device_set_desc(dev, "PMI632 Type-C port");
	return (BUS_PROBE_DEFAULT);
}

static int
typec_attach(device_t dev)
{
	struct typec_softc *sc = device_get_softc(dev);
	phandle_t node, vb;
	pcell_t ref, reg, ua;
	uint8_t mode;

	sc->dev = dev;
	sc->base = qcom_spmi_base(dev, 0);
	node = ofw_bus_get_node(dev);
	/* vdd-vbus-supply: the VBUS output's node, its reg in the PMIC */
	if (OF_getencprop(node, "vdd-vbus-supply", &ref, sizeof(ref)) > 0 &&
	    (vb = OF_node_from_xref(ref)) > 0 && ofw_bus_node_status_okay(vb) &&
	    OF_getencprop(vb, "reg", &reg, sizeof(reg)) > 0) {
		sc->vbus_base = reg;
		ua = 500000;
		OF_getencprop(vb, "regulator-max-microamp", &ua, sizeof(ua));
		sc->vbus_max_ua = ua;
	}
	mtx_init(&sc->mtx, "pro1x_typec", NULL, MTX_DEF);
	callout_init_mtx(&sc->poll, &sc->mtx, 0);
	sc->st.attached = -1;		/* the first poll reports */
	if (typec_rd(sc, TYPEC_MODE_CFG, &mode) == 0) {
		device_printf(dev, "at %#x, mode %02x (%s), VBUS out at %#x up "
		    "to %u mA%s\n", sc->base, mode, typec_mode_name(mode),
		    sc->vbus_base, sc->vbus_max_ua / 1000,
		    pro1x_typec_vbus ? " (switched by the role)" : " (left alone)");
		if (pro1x_typec_drp && (mode & TYPEC_POWER_ROLE_MASK) != 0) {
			typec_wr(sc, sc->base + TYPEC_MODE_CFG,
			    mode & ~TYPEC_POWER_ROLE_MASK);
			device_printf(dev, "dual role set (was %02x)\n", mode);
		}
	}
	SYSCTL_ADD_STRING(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "state",
	    CTLFLAG_RD, sc->state, 0, "What the port sees");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "changes",
	    CTLFLAG_RD, &sc->changes, 0, "Changes seen");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "irqs",
	    CTLFLAG_RD, &sc->irqs, 0, "Interrupts taken");
	typec_irqs(sc);
	device_printf(dev, "%d of 3 interrupts%s\n", sc->nirq,
	    sc->nirq == 0 || pro1x_typec_poll_ms > 0 ? ", polling" : "");
	mtx_lock(&sc->mtx);
	typec_poll(sc);
	mtx_unlock(&sc->mtx);
	return (0);
}

static int
typec_detach(device_t dev)
{
	struct typec_softc *sc = device_get_softc(dev);

	int i;

	for (i = 0; i < sc->nirq; i++)
		bus_teardown_intr(dev, sc->irq[i], sc->ih[i]);
	callout_drain(&sc->poll);
	mtx_lock(&sc->mtx);
	if (sc->vbus_on)
		typec_vbus(sc, 0);
	mtx_unlock(&sc->mtx);
	mtx_destroy(&sc->mtx);
	return (0);
}

static device_method_t typec_methods[] = {
	DEVMETHOD(device_probe,		typec_probe),
	DEVMETHOD(device_attach,	typec_attach),
	DEVMETHOD(device_detach,	typec_detach),
	DEVMETHOD_END
};

static driver_t typec_driver = {
	"pro1x_typec",
	typec_methods,
	sizeof(struct typec_softc),
};

DRIVER_MODULE(pro1x_typec, qcom_pmic, typec_driver, 0, 0);
MODULE_VERSION(pro1x_typec, 1);
