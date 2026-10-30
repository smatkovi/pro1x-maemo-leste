/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The Qualcomm Camera Control Interface (CCI, cci@5c1b000 on the SM6115):
 * the camera's own I2C controller with two masters, i2c-bus@0 (the rear
 * IMX582 at 0x10 and its DW9800 lens at 0x0c) and i2c-bus@1 (the front
 * S5K4H7 at 0x2d). Each master is an iicbus(4) bus, so /dev/iicN and
 * i2c(8) work on it. A port of Linux's i2c-qcom-cci.c (cci_v2_data, the
 * "qcom,msm8996-cci" the node is compatible with):
 *
 * - before the first register access: the camera power domain
 *   (power-domains, GCC_CAMSS_TOP_GDSC), the node's clocks (top AHB, CCI
 *   at assigned-clock-rates' 37.5 MHz), the CCI pins (pinctrl-0: gpio22/23
 *   and gpio29/30 in function cci_i2c) -- camss_gcc.c, camss_tlmm.c
 * - reset, interrupt masks and each master's timing (fast mode for the
 *   children's clock-frequency 400000) as cci_reset()/cci_init()
 * - a message is one queue run as cci_i2c_write()/cci_i2c_read(): SET_PARAM
 *   with the address, WRITE with up to 11 bytes (queue 0, then REPORT) or
 *   READ of up to 12 bytes (queue 1); a register read is two runs, WRITE
 *   then READ, with a STOP between them, as Linux does it (IIC_M_NOSTOP
 *   cannot be honoured); the first byte of the read buffer is skipped as
 *   Linux skips it
 * - completion is polled in IRQ_STATUS_0 (the interrupt, GIC SPI 206, is
 *   not used); an error halts the master and resets it (cci_isr), a
 *   timeout resets and reinitialises the controller (cci_run_queue)
 *
 *   sysctl dev.qcom_cci.0.{hw_version,xfers,nacks,errors,timeouts}
 */

#ifndef CCI_HOST_TEST
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/sysctl.h>

#include <machine/bus.h>

#include <dev/iicbus/iicbus.h>
#include <dev/iicbus/iiconf.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "iicbus_if.h"

#include "camss_var.h"
#include "camss_gcc_sm6115.h"
#else
#include "cci_host_shim.h"	/* freebsd/camera/test */
#endif

#define	CCI_HW_VERSION			0x000
#define	CCI_RESET_CMD			0x004
#define	 CCI_RESET_CMD_MASK		0x0f73f3f7
#define	 CCI_RESET_CMD_M0_MASK		0x000003f1
#define	 CCI_RESET_CMD_M1_MASK		0x0003f001
#define	CCI_QUEUE_START			0x008
#define	CCI_HALT_REQ			0x034
#define	CCI_I2C_SCL_CTL(m)		(0x100 + 0x100 * (m))
#define	CCI_I2C_SDA_CTL_0(m)		(0x104 + 0x100 * (m))
#define	CCI_I2C_SDA_CTL_1(m)		(0x108 + 0x100 * (m))
#define	CCI_I2C_SDA_CTL_2(m)		(0x10c + 0x100 * (m))
#define	CCI_I2C_MISC_CTL(m)		(0x110 + 0x100 * (m))
#define	CCI_I2C_READ_DATA(m)		(0x118 + 0x100 * (m))
#define	CCI_I2C_READ_BUF_LEVEL(m)	(0x11c + 0x100 * (m))
#define	CCI_I2C_Q_EXEC_WORD_CNT(m, n)	(0x300 + 0x200 * (m) + 0x100 * (n))
#define	CCI_I2C_Q_CUR_WORD_CNT(m, n)	(0x304 + 0x200 * (m) + 0x100 * (n))
#define	CCI_I2C_Q_LOAD_DATA(m, n)	(0x310 + 0x200 * (m) + 0x100 * (n))
#define	CCI_IRQ_GLOBAL_CLEAR_CMD	0xc00
#define	CCI_IRQ_MASK_0			0xc04
#define	CCI_IRQ_CLEAR_0			0xc08
#define	CCI_IRQ_STATUS_0		0xc0c
#define	 IRQ_RD_DONE(m)			(1u << (0 + 12 * (m)))
#define	 IRQ_Q0_REPORT(m)		(1u << (4 + 12 * (m)))
#define	 IRQ_Q1_REPORT(m)		(1u << (8 + 12 * (m)))
#define	 IRQ_RST_DONE_ACK		(1u << 24)
#define	 IRQ_HALT_ACK(m)		(1u << (25 + (m)))
#define	 IRQ_NACK(m)			(3u << (27 + 2 * (m)))
#define	 IRQ_M0_ERROR			0x18000ee6u
#define	 IRQ_M1_ERROR			0x60ee6000u
#define	 IRQ_ERROR(m)			((m) ? IRQ_M1_ERROR : IRQ_M0_ERROR)
/* cci_init()'s mask */
#define	CCI_IRQ_MASK_ALL	(IRQ_RD_DONE(0) | IRQ_Q0_REPORT(0) | \
	IRQ_Q1_REPORT(0) | IRQ_RD_DONE(1) | IRQ_Q0_REPORT(1) | \
	IRQ_Q1_REPORT(1) | IRQ_RST_DONE_ACK | IRQ_HALT_ACK(0) | \
	IRQ_HALT_ACK(1) | IRQ_M0_ERROR | IRQ_M1_ERROR)

#define	CCI_I2C_SET_PARAM	1
#define	CCI_I2C_REPORT		8
#define	CCI_I2C_WRITE		9
#define	CCI_I2C_READ		10
#define	CCI_I2C_REPORT_IRQ_EN	(1u << 8)

#define	CCI_TIMEOUT_US		100000		/* CCI_TIMEOUT: 100 ms */
#define	CCI_NMASTERS		2
#define	CCI_MAX_WRITE		11		/* cci_v2_data quirks */
#define	CCI_MAX_READ		12

static const u_int cci_queue_size[2] = { 64, 16 };

/* cci_v2_data: thigh, tlow, tsu_sto, tsu_sta, thd_dat, thd_sta, tbuf,
 * scl_stretch_en, trdhld, tsp */
static const struct cci_hw_params {
	uint16_t	thigh, tlow, tsu_sto, tsu_sta, thd_dat, thd_sta, tbuf;
	uint8_t		scl_stretch_en;
	uint16_t	trdhld, tsp;
} cci_params[3] = {
	{ 201, 174, 204, 231, 22, 162, 227, 0, 6, 3 },	/* standard */
	{ 38, 56, 40, 40, 22, 35, 62, 0, 6, 3 },	/* fast */
	{ 16, 22, 17, 18, 16, 15, 24, 0, 3, 3 },	/* fast plus */
};

struct cci_softc {
	device_t		dev;
	struct resource		*mem;
	struct mtx		mtx;
	phandle_t		node;
	int			clk[4];
	int			nclk;
	int			gdsc;
	uint32_t		hw_version;
	u_int			mode[CCI_NMASTERS];
	int			present[CCI_NMASTERS];
	u_int			xfers, nacks, errors, timeouts;
};

struct ccim_softc {
	device_t		dev;
	device_t		iicbus;
	u_int			master;
};

#define	RD4(sc, r)	bus_read_4((sc)->mem, (r))
#define	WR4(sc, r, v)	bus_write_4((sc)->mem, (r), (v))

static void
cci_init(struct cci_softc *sc)
{
	const struct cci_hw_params *hw;
	u_int m;

	WR4(sc, CCI_IRQ_MASK_0, CCI_IRQ_MASK_ALL);
	for (m = 0; m < CCI_NMASTERS; m++) {
		if (!sc->present[m])
			continue;
		hw = &cci_params[sc->mode[m]];
		WR4(sc, CCI_I2C_SCL_CTL(m), hw->thigh << 16 | hw->tlow);
		WR4(sc, CCI_I2C_SDA_CTL_0(m), hw->tsu_sto << 16 | hw->tsu_sta);
		WR4(sc, CCI_I2C_SDA_CTL_1(m), hw->thd_dat << 16 | hw->thd_sta);
		WR4(sc, CCI_I2C_SDA_CTL_2(m), hw->tbuf);
		WR4(sc, CCI_I2C_MISC_CTL(m), hw->scl_stretch_en << 8 |
		    hw->trdhld << 4 | hw->tsp);
	}
}

/* the status bits as cci_isr() reads and clears them */
static uint32_t
cci_status(struct cci_softc *sc)
{
	uint32_t st;

	st = RD4(sc, CCI_IRQ_STATUS_0);
	if (st != 0) {
		WR4(sc, CCI_IRQ_CLEAR_0, st);
		WR4(sc, CCI_IRQ_GLOBAL_CLEAR_CMD, 1);
	}
	return (st);
}

/* wait for any of bits; 0 if seen, else ETIMEDOUT; the bits seen in *seen */
static int
cci_poll(struct cci_softc *sc, uint32_t bits, uint32_t *seen)
{
	uint32_t st, all = 0;
	int us;

	for (us = 0; us < CCI_TIMEOUT_US; us += 5) {
		st = cci_status(sc);
		all |= st;
		if (st & bits)
			break;
		DELAY(5);
	}
	if (seen != NULL)
		*seen = all;
	return ((all & bits) ? 0 : ETIMEDOUT);
}

static int
cci_reset(struct cci_softc *sc)
{
	WR4(sc, CCI_IRQ_MASK_0, CCI_IRQ_MASK_ALL);
	WR4(sc, CCI_RESET_CMD, CCI_RESET_CMD_MASK);
	return (cci_poll(sc, IRQ_RST_DONE_ACK, NULL));
}

/*
 * One queue run: cci_run_queue(), with cci_isr()'s handling of the result.
 * 0, IIC_ENOACK, IIC_EBUSERR or IIC_ETIMEOUT.
 */
static int
cci_run_queue(struct cci_softc *sc, u_int m, u_int q)
{
	uint32_t seen, more;

	WR4(sc, CCI_I2C_Q_EXEC_WORD_CNT(m, q), RD4(sc,
	    CCI_I2C_Q_CUR_WORD_CNT(m, q)));
	WR4(sc, CCI_QUEUE_START, 1u << (m * 2 + q));
	if (cci_poll(sc, IRQ_RD_DONE(m) | IRQ_Q0_REPORT(m) | IRQ_Q1_REPORT(m) |
	    IRQ_ERROR(m), &seen) != 0) {
		sc->timeouts++;
		device_printf(sc->dev, "master %u queue %u timeout\n", m, q);
		cci_reset(sc);
		cci_init(sc);
		return (IIC_ETIMEOUT);
	}
	if ((seen & IRQ_ERROR(m)) == 0)
		return (0);
	/* halt the master; its halt acknowledge resets it (cci_isr) */
	WR4(sc, CCI_HALT_REQ, 1u << m);
	if (cci_poll(sc, IRQ_HALT_ACK(m), &more) == 0) {
		WR4(sc, CCI_RESET_CMD, m ? CCI_RESET_CMD_M1_MASK :
		    CCI_RESET_CMD_M0_MASK);
		(void)cci_poll(sc, IRQ_RST_DONE_ACK, NULL);
	}
	if (seen & IRQ_NACK(m)) {
		sc->nacks++;
		return (IIC_ENOACK);
	}
	sc->errors++;
	device_printf(sc->dev, "master %u queue %u error: status %#x\n", m, q,
	    seen);
	return (IIC_EBUSERR);
}

/* cci_validate_queue(): an unfinished queue is run first */
static int
cci_validate_queue(struct cci_softc *sc, u_int m, u_int q)
{
	uint32_t n;

	n = RD4(sc, CCI_I2C_Q_CUR_WORD_CNT(m, q));
	if (n == cci_queue_size[q])
		return (IIC_EOVERFLOW);
	if (n == 0)
		return (0);
	WR4(sc, CCI_I2C_Q_LOAD_DATA(m, q), CCI_I2C_REPORT |
	    CCI_I2C_REPORT_IRQ_EN);
	return (cci_run_queue(sc, m, q));
}

static int
cci_write(struct cci_softc *sc, u_int m, uint8_t addr, const uint8_t *buf,
    u_int len)
{
	uint8_t load[CCI_MAX_WRITE + 1 + 3];
	u_int i, j;
	int error;

	if (len > CCI_MAX_WRITE)
		return (IIC_ENOTSUPP);
	if ((error = cci_validate_queue(sc, m, 0)) != 0)
		return (error);
	WR4(sc, CCI_I2C_Q_LOAD_DATA(m, 0), CCI_I2C_SET_PARAM |
	    (addr & 0x7f) << 4);
	memset(load, 0, sizeof(load));
	i = 0;
	load[i++] = CCI_I2C_WRITE | len << 4;
	for (j = 0; j < len; j++)
		load[i++] = buf[j];
	for (j = 0; j < i; j += 4)
		WR4(sc, CCI_I2C_Q_LOAD_DATA(m, 0), load[j] | load[j + 1] << 8 |
		    load[j + 2] << 16 | (uint32_t)load[j + 3] << 24);
	WR4(sc, CCI_I2C_Q_LOAD_DATA(m, 0), CCI_I2C_REPORT |
	    CCI_I2C_REPORT_IRQ_EN);
	return (cci_run_queue(sc, m, 0));
}

static int
cci_read(struct cci_softc *sc, u_int m, uint8_t addr, uint8_t *buf, u_int len)
{
	uint32_t val, words, expect;
	u_int i, idx = 0;
	int error, first = 1;

	if (len == 0 || len > CCI_MAX_READ)
		return (IIC_ENOTSUPP);
	if ((error = cci_validate_queue(sc, m, 1)) != 0)
		return (error);
	WR4(sc, CCI_I2C_Q_LOAD_DATA(m, 1), CCI_I2C_SET_PARAM |
	    (addr & 0x7f) << 4);
	WR4(sc, CCI_I2C_Q_LOAD_DATA(m, 1), CCI_I2C_READ | len << 4);
	if ((error = cci_run_queue(sc, m, 1)) != 0)
		return (error);
	words = RD4(sc, CCI_I2C_READ_BUF_LEVEL(m));
	expect = len / 4 + 1;
	if (words != expect) {
		device_printf(sc->dev, "words read %u, expected %u\n", words,
		    expect);
		sc->errors++;
		return (IIC_EBUSERR);
	}
	do {
		val = RD4(sc, CCI_I2C_READ_DATA(m));
		for (i = 0; i < 4 && idx < len; i++) {
			if (first) {
				/* as Linux: the first byte is not data */
				first = 0;
				continue;
			}
			buf[idx++] = (val >> (i * 8)) & 0xff;
		}
	} while (--words);
	return (0);
}

static int
cci_transfer(struct cci_softc *sc, u_int m, struct iic_msg *msgs,
    uint32_t nmsgs)
{
	uint32_t i;
	int error = 0;

	mtx_lock(&sc->mtx);
	for (i = 0; i < nmsgs && error == 0; i++) {
		/* FreeBSD's slave addresses are 8 bit */
		if (msgs[i].flags & IIC_M_RD)
			error = cci_read(sc, m, msgs[i].slave >> 1, msgs[i].buf,
			    msgs[i].len);
		else
			error = cci_write(sc, m, msgs[i].slave >> 1,
			    msgs[i].buf, msgs[i].len);
	}
	sc->xfers++;
	mtx_unlock(&sc->mtx);
	return (error);
}

#ifndef CCI_HOST_TEST
/* --- the controller ----------------------------------------------------- */

static int
cci_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (!ofw_bus_is_compatible(dev, "qcom,msm8996-cci") &&
	    !ofw_bus_is_compatible(dev, "qcom,qcm2290-cci"))
		return (ENXIO);
	device_set_desc(dev, "Qualcomm Camera Control Interface");
	return (BUS_PROBE_DEFAULT);
}

static int cci_detach(device_t dev);

static int
cci_attach(device_t dev)
{
	struct cci_softc *sc = device_get_softc(dev);
	struct sysctl_oid_list *tree;
	struct sysctl_ctx_list *ctx;
	pcell_t pd[2];
	phandle_t ch;
	device_t child;
	uint32_t idx, freq;
	int error, rid, n;

	sc->dev = dev;
	sc->node = ofw_bus_get_node(dev);
	sc->gdsc = -1;
	mtx_init(&sc->mtx, "qcom_cci", NULL, MTX_DEF);
	if ((error = camss_gcc_attach()) != 0)
		goto fail;
	/* the power domain: GCC_CAMSS_TOP_GDSC only */
	if (OF_getencprop(sc->node, "power-domains", pd, sizeof(pd)) !=
	    sizeof(pd) || pd[1] != CAMSS_DT_GCC_CAMSS_TOP_GDSC) {
		device_printf(dev, "power domain is not GCC_CAMSS_TOP_GDSC\n");
		error = ENXIO;
		goto fail;
	}
	if ((error = camss_gdsc_get(CAMSS_GDSC_CAMSS_TOP, "cci")) != 0)
		goto fail;
	sc->gdsc = 0;
	/* camera AHB: CLK_IS_CRITICAL in Linux, so normally on already */
	if ((error = camss_clk_on(CAMSS_CLK_CAMERA_AHB_CLK, 0, "cci")) != 0)
		goto fail;
	sc->clk[0] = CAMSS_CLK_CAMERA_AHB_CLK;
	n = camss_dt_clocks_on(sc->node, "cci", sc->clk + 1,
	    nitems(sc->clk) - 1);
	if (n < 0) {
		error = -n;
		goto fail;
	}
	sc->nclk = n + 1;
	if ((error = camss_pinctrl(sc->node, 0, "cci")) != 0)
		device_printf(dev, "pins: %d (continuing)\n", error);
	rid = 0;
	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->mem == NULL) {
		error = ENXIO;
		goto fail;
	}
	sc->hw_version = RD4(sc, CCI_HW_VERSION);
	device_printf(dev, "CCI HW version %#x\n", sc->hw_version);

	/* the masters: i2c-bus@N, fast mode at clock-frequency 400000 */
	for (ch = OF_child(sc->node); ch > 0; ch = OF_peer(ch)) {
		if (OF_getencprop(ch, "reg", &idx, sizeof(idx)) <= 0 ||
		    idx >= CCI_NMASTERS || !ofw_bus_node_status_okay(ch))
			continue;
		sc->present[idx] = 1;
		sc->mode[idx] = 0;
		if (OF_getencprop(ch, "clock-frequency", &freq,
		    sizeof(freq)) > 0) {
			if (freq == 400000)
				sc->mode[idx] = 1;
			else if (freq == 1000000)
				sc->mode[idx] = 2;
		}
	}
	mtx_lock(&sc->mtx);
	if (cci_reset(sc) != 0)
		device_printf(dev, "reset: no RST_DONE (continuing)\n");
	cci_init(sc);
	mtx_unlock(&sc->mtx);

	ctx = device_get_sysctl_ctx(dev);
	tree = SYSCTL_CHILDREN(device_get_sysctl_tree(dev));
	SYSCTL_ADD_U32(ctx, tree, OID_AUTO, "hw_version", CTLFLAG_RD,
	    &sc->hw_version, 0, "CCI_HW_VERSION");
	SYSCTL_ADD_UINT(ctx, tree, OID_AUTO, "xfers", CTLFLAG_RD, &sc->xfers,
	    0, "transfers");
	SYSCTL_ADD_UINT(ctx, tree, OID_AUTO, "nacks", CTLFLAG_RD, &sc->nacks,
	    0, "messages not acknowledged");
	SYSCTL_ADD_UINT(ctx, tree, OID_AUTO, "errors", CTLFLAG_RD,
	    &sc->errors, 0, "other errors");
	SYSCTL_ADD_UINT(ctx, tree, OID_AUTO, "timeouts", CTLFLAG_RD,
	    &sc->timeouts, 0, "queue runs that never finished");

	for (idx = 0; idx < CCI_NMASTERS; idx++) {
		if (!sc->present[idx])
			continue;
		child = device_add_child(dev, "qcom_ccim", DEVICE_UNIT_ANY);
		if (child == NULL)
			continue;
		device_set_ivars(child, (void *)(uintptr_t)idx);
	}
	bus_attach_children(dev);
	return (0);
fail:
	cci_detach(dev);
	return (error);
}

static int
cci_detach(device_t dev)
{
	struct cci_softc *sc = device_get_softc(dev);
	int error;

	if ((error = bus_generic_detach(dev)) != 0)
		return (error);
	if (sc->mem != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mem);
	sc->mem = NULL;
	camss_dt_clocks_off(sc->clk, sc->nclk, "cci");
	sc->nclk = 0;
	if (sc->gdsc == 0)
		camss_gdsc_put(CAMSS_GDSC_CAMSS_TOP, "cci");
	sc->gdsc = -1;
	if (mtx_initialized(&sc->mtx))
		mtx_destroy(&sc->mtx);
	return (0);
}

/* the iic unit of a master's bus, for /dev/camss's information */
int	qcom_cci_iic_unit(phandle_t busnode);

int
qcom_cci_iic_unit(phandle_t busnode)
{
	devclass_t dc;
	device_t d, cci, iicbus, iic;
	struct ccim_softc *msc;
	uint32_t idx;
	int i;

	if ((dc = devclass_find("qcom_ccim")) == NULL ||
	    OF_getencprop(busnode, "reg", &idx, sizeof(idx)) <= 0)
		return (-1);
	for (i = 0; i <= devclass_get_maxunit(dc); i++) {
		if ((d = devclass_get_device(dc, i)) == NULL ||
		    !device_is_attached(d))
			continue;
		msc = device_get_softc(d);
		cci = device_get_parent(d);
		if (msc->master != idx ||
		    ofw_bus_get_node(cci) != OF_parent(busnode))
			continue;
		if ((iicbus = msc->iicbus) == NULL ||
		    (iic = device_find_child(iicbus, "iic", DEVICE_UNIT_ANY)) ==
		    NULL)
			return (-1);
		return (device_get_unit(iic));
	}
	return (-1);
}

/* --- a master: the parent of its iicbus ------------------------------------------ */

static int
ccim_probe(device_t dev)
{
	device_set_desc(dev, "Qualcomm CCI master");
	return (BUS_PROBE_NOWILDCARD);
}

static int
ccim_attach(device_t dev)
{
	struct ccim_softc *sc = device_get_softc(dev);

	sc->dev = dev;
	sc->master = (u_int)(uintptr_t)device_get_ivars(dev);
	sc->iicbus = device_add_child(dev, "iicbus", DEVICE_UNIT_ANY);
	if (sc->iicbus == NULL)
		return (ENXIO);
	bus_attach_children(dev);
	device_printf(dev, "master %u\n", sc->master);
	return (0);
}

static int
ccim_detach(device_t dev)
{
	return (bus_generic_detach(dev));
}

static int
ccim_transfer(device_t dev, struct iic_msg *msgs, uint32_t nmsgs)
{
	struct ccim_softc *sc = device_get_softc(dev);

	return (cci_transfer(device_get_softc(device_get_parent(dev)),
	    sc->master, msgs, nmsgs));
}

static int
ccim_reset(device_t dev, u_char speed, u_char addr, u_char *oldaddr)
{
	/* the timing is the device tree's; nothing to change */
	return (0);
}

static device_method_t cci_methods[] = {
	DEVMETHOD(device_probe,		cci_probe),
	DEVMETHOD(device_attach,	cci_attach),
	DEVMETHOD(device_detach,	cci_detach),
	DEVMETHOD_END
};

static driver_t cci_driver = {
	"qcom_cci",
	cci_methods,
	sizeof(struct cci_softc),
};

static device_method_t ccim_methods[] = {
	DEVMETHOD(device_probe,		ccim_probe),
	DEVMETHOD(device_attach,	ccim_attach),
	DEVMETHOD(device_detach,	ccim_detach),
	DEVMETHOD(iicbus_transfer,	ccim_transfer),
	DEVMETHOD(iicbus_reset,		ccim_reset),
	DEVMETHOD(iicbus_callback,	iicbus_null_callback),
	DEVMETHOD_END
};

static driver_t ccim_driver = {
	"qcom_ccim",
	ccim_methods,
	sizeof(struct ccim_softc),
};

DRIVER_MODULE(qcom_cci, simplebus, cci_driver, NULL, NULL);
DRIVER_MODULE(qcom_ccim, qcom_cci, ccim_driver, NULL, NULL);
DRIVER_MODULE(iicbus, qcom_ccim, iicbus_driver, NULL, NULL);
MODULE_DEPEND(qcom_camss, iicbus, IICBUS_MINVER, IICBUS_PREFVER,
    IICBUS_MAXVER);
#endif /* !CCI_HOST_TEST */
