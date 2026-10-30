/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The SM6115's camera subsystem (camss@5c11000, "qcom,qcm2290-camss") as
 * /dev/camss0 (qcom_camss_ioctl.h, freebsd/camera/README.md).
 *
 * Stage 1, the sensors: what the device tree says about them, read once
 * at attach (CAMSS_IOC_INFO): the port graph (ports/port@N -> the
 * sensor's endpoint), each sensor's CCI bus (and its /dev/iicN), address,
 * reset line, MCLK and its rate, link frequencies, and its rails with
 * where each comes from (a PCA9534 pin through a regulator-fixed, an LDO
 * of the PM8008, both on the keyboard's I2C bus); its lens (lens-focus).
 * The rails and the sensor registers are user space's (pro1x-cam over
 * /dev/iicN); what needs SoC registers is here: the MCLK with the sensor
 * node's pins (CAMSS_IOC_MCLK) and the reset lines (CAMSS_IOC_GPIO).
 *
 * Stage 2, raw frames (camss_stream.c): CSIPHY, CSID and the TFE's RDI
 * write master, as Linux's camss-csiphy-3ph-1-0.c, camss-csid-340.c and
 * camss-vfe-340.c drive them for this SoC.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/endian.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/sbuf.h>
#include <sys/sx.h>
#include <sys/sysctl.h>

#include <machine/bus.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "camss_var.h"
#include "camss_gcc_sm6115.h"
#include "camss_stream.h"
#include "qcom_camss_ioctl.h"

int	qcom_cci_iic_unit(phandle_t busnode);

static const int mclk_branch[4] = {
	CAMSS_CLK_CAMSS_MCLK0_CLK, CAMSS_CLK_CAMSS_MCLK1_CLK,
	CAMSS_CLK_CAMSS_MCLK2_CLK, CAMSS_CLK_CAMSS_MCLK3_CLK,
};

/* --- the device tree ------------------------------------------------------------ */

static void
copy_name(char *dst, size_t len, phandle_t node)
{
	if (OF_getprop(node, "name", dst, len) <= 0)
		dst[0] = '\0';
	dst[len - 1] = '\0';
}

static void
copy_compatible(char *dst, size_t len, phandle_t node)
{
	if (OF_getprop(node, "compatible", dst, len) <= 0)
		dst[0] = '\0';
	dst[len - 1] = '\0';		/* the first string only */
}

/* /dev/iicN of the iicbus on the I2C controller node ctrl, -1 if none */
static int
iic_unit_of_controller(phandle_t ctrl)
{
	devclass_t dc;
	device_t bus, iic;
	int i;

	if ((dc = devclass_find("iicbus")) == NULL)
		return (-1);
	for (i = 0; i <= devclass_get_maxunit(dc); i++) {
		if ((bus = devclass_get_device(dc, i)) == NULL ||
		    !device_is_attached(bus))
			continue;
		if (ofw_bus_get_node(bus) != ctrl)
			continue;
		iic = device_find_child(bus, "iic", DEVICE_UNIT_ANY);
		return (iic != NULL ? device_get_unit(iic) : -1);
	}
	return (-1);
}

static phandle_t pmic_node;	/* the PM8008 found behind a supply */

static void
resolve_supply(phandle_t ph, const char *name, struct camss_supply *s)
{
	phandle_t node, ctrl, parent;
	pcell_t gpio[3];
	char pname[32];
	uint32_t v;
	u_int ldo;

	memset(s, 0, sizeof(*s));
	strlcpy(s->name, name, sizeof(s->name));
	s->iic_unit = -1;
	node = OF_node_from_xref(ph);
	copy_name(s->node, sizeof(s->node), node);
	if (OF_getencprop(node, "regulator-min-microvolt", &v, sizeof(v)) > 0)
		s->min_uv = v;
	if (OF_getencprop(node, "regulator-max-microvolt", &v, sizeof(v)) > 0)
		s->max_uv = v;
	if (ofw_bus_node_is_compatible(node, "regulator-fixed") &&
	    OF_getencprop(node, "gpio", gpio, sizeof(gpio)) == sizeof(gpio)) {
		ctrl = OF_node_from_xref(gpio[0]);
		if (ofw_bus_node_is_compatible(ctrl, "nxp,pca9534")) {
			s->type = CAMSS_SUPPLY_PCA9534;
			s->index = gpio[1];
			s->active_high = OF_hasprop(node, "enable-active-high");
			if (OF_getencprop(ctrl, "reg", &v, sizeof(v)) > 0)
				s->addr = v;
			s->iic_unit = iic_unit_of_controller(OF_parent(ctrl));
		}
		return;
	}
	parent = OF_parent(node);
	copy_name(pname, sizeof(pname), parent);
	if (strcmp(pname, "regulators") == 0 &&
	    ofw_bus_node_is_compatible(OF_parent(parent), "qcom,pm8008") &&
	    sscanf(s->node, "ldo%u", &ldo) == 1) {
		s->type = CAMSS_SUPPLY_PM8008;
		s->index = ldo;
		pmic_node = OF_parent(parent);
		if (OF_getencprop(pmic_node, "reg", &v, sizeof(v)) > 0)
			s->addr = v;
		s->iic_unit = iic_unit_of_controller(OF_parent(pmic_node));
	}
}

/* every "<name>-supply" of node, in the device tree's order */
static int
read_supplies(phandle_t node, struct camss_supply *s, int max)
{
	char prop[64], name[32];
	pcell_t ph;
	size_t l;
	int n = 0;

	prop[0] = '\0';
	while (n < max && OF_nextprop(node, prop, prop, sizeof(prop)) > 0) {
		l = strlen(prop);
		if (l <= 7 || strcmp(prop + l - 7, "-supply") != 0)
			continue;
		if (OF_getencprop(node, prop, &ph, sizeof(ph)) != sizeof(ph))
			continue;
		strlcpy(name, prop, MIN(sizeof(name), l - 7 + 1));
		resolve_supply(ph, name, &s[n++]);
	}
	return (n);
}

static void
read_sensor(phandle_t sensor, phandle_t ep, phandle_t camss_ep,
    struct camss_sensor *cs, struct camss_sensor_priv *p)
{
	uint32_t lanes[4], v, rates[2];
	pcell_t clk[2], lens;
	uint64_t lf[CAMSS_MAX_LINKFREQ];
	phandle_t bus, ln;
	ssize_t len;
	int pin, al, i, b;

	copy_name(cs->node, sizeof(cs->node), sensor);
	copy_compatible(cs->compatible, sizeof(cs->compatible), sensor);
	p->node = sensor;
	if (OF_getencprop(sensor, "reg", &v, sizeof(v)) > 0)
		cs->addr = v;
	bus = OF_parent(sensor);
	if (OF_getencprop(bus, "reg", &v, sizeof(v)) > 0)
		cs->cci_master = v;
	cs->iic_unit = qcom_cci_iic_unit(bus);
	len = OF_getencprop(camss_ep, "data-lanes", lanes, sizeof(lanes));
	cs->nlanes = len > 0 ? len / sizeof(uint32_t) : 0;
	for (i = 0; i < cs->nlanes; i++)
		cs->lanes[i] = lanes[i];
	len = OF_getprop(ep, "link-frequencies", lf, sizeof(lf));
	cs->nlink = len > 0 ? len / sizeof(uint64_t) : 0;
	for (i = 0; i < cs->nlink; i++)
		cs->link_freq[i] = be64toh(lf[i]);
	cs->reset_gpio = -1;
	if (camss_dt_gpio(sensor, "reset-gpios", &pin, &al) == 0) {
		cs->reset_gpio = pin;
		cs->reset_active_low = al;
	}
	cs->mclk = -1;
	p->mclk_branch = -1;
	if (OF_getencprop(sensor, "clocks", clk, sizeof(clk)) == sizeof(clk) &&
	    (b = camss_clk_by_dt(clk[0], clk[1])) >= 0) {
		for (i = 0; i < 4; i++)
			if (mclk_branch[i] == b)
				cs->mclk = i;
		p->mclk_branch = b;
	}
	if (OF_getencprop(sensor, "assigned-clock-rates", rates,
	    sizeof(rates[0])) > 0)
		cs->mclk_hz = rates[0];
	cs->orientation = cs->rotation = -1;
	if (OF_getencprop(sensor, "orientation", &v, sizeof(v)) > 0)
		cs->orientation = v;
	if (OF_getencprop(sensor, "rotation", &v, sizeof(v)) > 0)
		cs->rotation = v;
	cs->nsupplies = read_supplies(sensor, cs->supply, CAMSS_MAX_SUPPLIES);
	if (OF_getencprop(sensor, "lens-focus", &lens, sizeof(lens)) ==
	    sizeof(lens)) {
		ln = OF_node_from_xref(lens);
		cs->has_lens = 1;
		copy_name(cs->lens_node, sizeof(cs->lens_node), ln);
		copy_compatible(cs->lens_compatible,
		    sizeof(cs->lens_compatible), ln);
		if (OF_getencprop(ln, "reg", &v, sizeof(v)) > 0)
			cs->lens_addr = v;
		cs->nlens_supplies = read_supplies(ln, cs->lens_supply,
		    CAMSS_MAX_SUPPLIES);
	}
}

static int
read_graph(struct camss_softc *sc)
{
	phandle_t ports, port, ep, rep, rport, sensor;
	pcell_t ph;
	uint32_t reg;
	struct camss_sensor *cs;
	int pin, al;

	ports = ofw_bus_find_child(sc->node, "ports");
	if (ports <= 0)
		return (ENXIO);
	for (port = OF_child(ports); port > 0; port = OF_peer(port)) {
		if (sc->info.nsensors >= CAMSS_MAX_SENSORS)
			break;
		if (OF_getencprop(port, "reg", &reg, sizeof(reg)) <= 0)
			continue;
		for (ep = OF_child(port); ep > 0; ep = OF_peer(ep)) {
			if (OF_getencprop(ep, "remote-endpoint", &ph,
			    sizeof(ph)) != sizeof(ph))
				continue;
			rep = OF_node_from_xref(ph);
			rport = OF_parent(rep);
			sensor = OF_parent(rport);
			if (!ofw_bus_node_status_okay(sensor))
				continue;
			cs = &sc->info.sensor[sc->info.nsensors];
			cs->port = reg;
			read_sensor(sensor, rep, ep, cs,
			    &sc->sensor[sc->info.nsensors]);
			sc->info.nsensors++;
			break;
		}
	}
	sc->info.pmic_reset_gpio = -1;
	if (pmic_node > 0 && camss_dt_gpio(pmic_node, "reset-gpios", &pin,
	    &al) == 0) {
		sc->info.pmic_reset_gpio = pin;
		sc->info.pmic_reset_active_low = al;
	}
	return (0);
}

static void
print_supply(device_t dev, const char *who, const struct camss_supply *s)
{
	static const char *const type[] = { "other", "pca9534", "pm8008" };

	device_printf(dev, "  %s %s: %s %s %u (0x%02x on iic%d) %u-%u uV%s\n",
	    who, s->name, s->node, type[s->type < 3 ? s->type : 0], s->index,
	    s->addr, s->iic_unit, s->min_uv, s->max_uv,
	    s->type == CAMSS_SUPPLY_PCA9534 && !s->active_high ?
	    " active low" : "");
}

static void
print_info(struct camss_softc *sc)
{
	struct camss_sensor *cs;
	u_int i, j;

	for (i = 0; i < sc->info.nsensors; i++) {
		cs = &sc->info.sensor[i];
		device_printf(sc->dev, "sensor %u: %s %s port %u, %u lanes, "
		    "CCI master %u (iic%d) 0x%02x, reset gpio%d%s, MCLK%d %u Hz,"
		    " %u link frequencies (%ju), rotation %d\n", i,
		    cs->compatible, cs->node, cs->port, cs->nlanes,
		    cs->cci_master, cs->iic_unit, cs->addr, cs->reset_gpio,
		    cs->reset_active_low ? " (active low)" : "", cs->mclk,
		    cs->mclk_hz, cs->nlink, (uintmax_t)cs->link_freq[0],
		    cs->rotation);
		for (j = 0; j < cs->nsupplies; j++)
			print_supply(sc->dev, "rail", &cs->supply[j]);
		if (cs->has_lens) {
			device_printf(sc->dev, "  lens %s %s 0x%02x\n",
			    cs->lens_compatible, cs->lens_node, cs->lens_addr);
			for (j = 0; j < cs->nlens_supplies; j++)
				print_supply(sc->dev, "lens rail",
				    &cs->lens_supply[j]);
		}
	}
	device_printf(sc->dev, "PMIC reset gpio%d%s\n", sc->info.pmic_reset_gpio,
	    sc->info.pmic_reset_active_low ? " (active low)" : "");
}

/*
 * The /dev/iicN units, read again if one was not there yet at attach (the
 * CCI's and the keyboard bus' iic devices attach on their own): the whole
 * graph again, keeping which MCLKs are on.
 */
static void
refresh_units(struct camss_softc *sc)
{
	struct camss_sensor *cs;
	int on[CAMSS_MAX_SENSORS];
	u_int i, j, missing = 0;

	for (i = 0; i < sc->info.nsensors; i++) {
		cs = &sc->info.sensor[i];
		missing |= cs->iic_unit < 0;
		for (j = 0; j < cs->nsupplies; j++)
			missing |= cs->supply[j].type != CAMSS_SUPPLY_OTHER &&
			    cs->supply[j].iic_unit < 0;
	}
	if (!missing)
		return;
	for (i = 0; i < CAMSS_MAX_SENSORS; i++)
		on[i] = sc->sensor[i].mclk_on;
	memset(sc->info.sensor, 0, sizeof(sc->info.sensor));
	sc->info.nsensors = 0;
	(void)read_graph(sc);
	for (i = 0; i < CAMSS_MAX_SENSORS; i++)
		sc->sensor[i].mclk_on = on[i];
}

/* --- the primitives ------------------------------------------------------------- */

static int
camss_mclk(struct camss_softc *sc, struct camss_mclk *m)
{
	struct camss_sensor_priv *p;
	struct camss_sensor *cs;
	int error;

	if (m->sensor >= sc->info.nsensors)
		return (EINVAL);
	cs = &sc->info.sensor[m->sensor];
	p = &sc->sensor[m->sensor];
	if (p->mclk_branch < 0)
		return (ENXIO);
	if (m->on && !p->mclk_on) {
		/* the MCLK pin's function and the reset line as a GPIO */
		if ((error = camss_gdsc_get(CAMSS_GDSC_CAMSS_TOP, "mclk")) != 0)
			return (error);
		(void)camss_pinctrl(p->node, 0, cs->node);
		if ((error = camss_clk_on(p->mclk_branch, cs->mclk_hz,
		    cs->node)) != 0) {
			camss_gdsc_put(CAMSS_GDSC_CAMSS_TOP, "mclk");
			return (error);
		}
		p->mclk_on = 1;
	} else if (!m->on && p->mclk_on) {
		camss_clk_off(p->mclk_branch, cs->node);
		camss_gdsc_put(CAMSS_GDSC_CAMSS_TOP, "mclk");
		p->mclk_on = 0;
	}
	m->hz = p->mclk_on ? (uint32_t)camss_clk_hz(p->mclk_branch) : 0;
	return (0);
}

static int
camss_gpio(struct camss_softc *sc, struct camss_gpio *g)
{
	u_int i;
	int al = -1;

	for (i = 0; i < sc->info.nsensors; i++)
		if (sc->info.sensor[i].reset_gpio == g->gpio)
			al = sc->info.sensor[i].reset_active_low;
	if (sc->info.pmic_reset_gpio == g->gpio)
		al = sc->info.pmic_reset_active_low;
	if (al < 0 || g->gpio < 0)
		return (EPERM);		/* not a line of the camera's */
	return (camss_gpio_out(g->gpio, (g->value != 0) ^ al, "camss"));
}

/* --- the device ----------------------------------------------------------------- */

static d_open_t		camss_open;
static d_close_t	camss_close;
static d_ioctl_t	camss_ioctl;
static d_mmap_t		camss_mmap;

static struct cdevsw camss_cdevsw = {
	.d_version =	D_VERSION,
	.d_open =	camss_open,
	.d_close =	camss_close,
	.d_ioctl =	camss_ioctl,
	.d_mmap =	camss_mmap,
	.d_name =	"camss",
};

static int
camss_open(struct cdev *dev, int oflags, int devtype, struct thread *td)
{
	return (0);
}

static int
camss_close(struct cdev *dev, int fflag, int devtype, struct thread *td)
{
	struct camss_softc *sc = dev->si_drv1;

	/* a stream outlives no process: the last close stops it */
	sx_xlock(&sc->lock);
	camss_stream_stop(sc);
	sx_xunlock(&sc->lock);
	return (0);
}

static int
camss_ioctl(struct cdev *dev, u_long cmd, caddr_t data, int fflag,
    struct thread *td)
{
	struct camss_softc *sc = dev->si_drv1;
	int error;

	sx_xlock(&sc->lock);
	switch (cmd) {
	case CAMSS_IOC_INFO:
		refresh_units(sc);
		memcpy(data, &sc->info, sizeof(sc->info));
		error = 0;
		break;
	case CAMSS_IOC_MCLK:
		error = camss_mclk(sc, (struct camss_mclk *)data);
		break;
	case CAMSS_IOC_GPIO:
		error = camss_gpio(sc, (struct camss_gpio *)data);
		break;
	case CAMSS_IOC_STREAMON:
		error = camss_stream_start(sc, (struct camss_stream *)data);
		break;
	case CAMSS_IOC_STREAMOFF:
		error = camss_stream_stop(sc);
		break;
	case CAMSS_IOC_QBUF:
		error = camss_stream_qbuf(sc, (struct camss_buf *)data);
		break;
	case CAMSS_IOC_DQBUF:
		error = camss_stream_dqbuf(sc, (struct camss_buf *)data);
		break;
	case CAMSS_IOC_REGDUMP:
		error = camss_stream_regdump(sc, (struct camss_regdump *)data);
		break;
	default:
		error = ENOTTY;
	}
	sx_xunlock(&sc->lock);
	return (error);
}

static int
camss_mmap(struct cdev *dev, vm_ooffset_t offset, vm_paddr_t *paddr,
    int nprot, vm_memattr_t *memattr)
{
	return (camss_stream_mmap(dev->si_drv1, offset, paddr, memattr));
}

static int
camss_gcc_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sb;
	int error;

	sbuf_new_for_sysctl(&sb, NULL, 4096, req);
	camss_gcc_report(&sb);
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

static int
camss_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "qcom,qcm2290-camss"))
		return (ENXIO);
	device_set_desc(dev, "Qualcomm camera subsystem (QCM2290/SM6115)");
	return (BUS_PROBE_DEFAULT);
}

static int
camss_attach(device_t dev)
{
	struct camss_softc *sc = device_get_softc(dev);
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid_list *tree;
	int error;

	sc->dev = dev;
	sc->node = ofw_bus_get_node(dev);
	sx_init(&sc->lock, "qcom_camss");
	if ((error = camss_gcc_attach()) != 0)
		return (error);
	sc->info.version = CAMSS_ABI_VERSION;
	if ((error = read_graph(sc)) != 0) {
		device_printf(dev, "no ports\n");
		return (error);
	}
	print_info(sc);
	if ((error = camss_stream_attach(sc)) != 0)
		return (error);
	ctx = device_get_sysctl_ctx(dev);
	tree = SYSCTL_CHILDREN(device_get_sysctl_tree(dev));
	SYSCTL_ADD_PROC(ctx, tree, OID_AUTO, "gcc", CTLTYPE_STRING |
	    CTLFLAG_RD | CTLFLAG_MPSAFE, sc, 0, camss_gcc_sysctl, "A",
	    "the camera clocks, PLLs and power domains as they are");
	sc->cdev = make_dev(&camss_cdevsw, device_get_unit(dev), UID_ROOT,
	    GID_VIDEO, 0660, "camss%d", device_get_unit(dev));
	sc->cdev->si_drv1 = sc;
	return (0);
}

static int
camss_detach(device_t dev)
{
	struct camss_softc *sc = device_get_softc(dev);
	struct camss_mclk m;
	u_int i;

	if (sc->cdev != NULL)
		destroy_dev(sc->cdev);
	sx_xlock(&sc->lock);
	camss_stream_stop(sc);
	for (i = 0; i < sc->info.nsensors; i++) {
		m.sensor = i;
		m.on = 0;
		camss_mclk(sc, &m);
	}
	camss_stream_detach(sc);
	sx_xunlock(&sc->lock);
	sx_destroy(&sc->lock);
	return (0);
}

static device_method_t camss_methods[] = {
	DEVMETHOD(device_probe,		camss_probe),
	DEVMETHOD(device_attach,	camss_attach),
	DEVMETHOD(device_detach,	camss_detach),
	DEVMETHOD_END
};

static driver_t camss_driver = {
	"qcom_camss",
	camss_methods,
	sizeof(struct camss_softc),
};

/* after the CCI, whose buses the sensors' information names */
DRIVER_MODULE_ORDERED(qcom_camss, simplebus, camss_driver, NULL, NULL,
    SI_ORDER_ANY);
MODULE_VERSION(qcom_camss, 1);
