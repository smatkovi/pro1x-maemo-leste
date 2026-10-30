/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Stage 2 of freebsd/camera/README.md, raw frames: CSIPHY -> CSID (RDI 0)
 * -> the TFE's RDI 0 write master -> memory, the "ISP-lite" path Linux's
 * CAMSS uses for this SoC (CAMSS_2290: camss-csiphy-3ph-1-0.c with
 * lane_regs_qcm2290, camss-csid-340.c, camss-vfe-340.c; camss.c's
 * csiphy/csid/vfe_res_2290 for the clocks). One stream at a time, through
 * CSID 0 and TFE 0 (any CSIPHY can feed any CSID: RX_CFG0's PHY_NUM_SEL).
 *
 * The order is Linux's: power (clocks at the rates camss.c's formulas
 * pick, reset; CSIPHY hardware version, CSID and TFE reset with their
 * RST_DONE), then from the memory end towards the sensor: the write
 * master with its first two buffers (vfe_enable_output_v2), the CSID's
 * receiver and RDI (csid_configure_stream), the CSIPHY's lanes
 * (csiphy_lanes_enable with the settle count); user space then starts the
 * sensor. The interrupts are polled (TFE_IRQ_STATUS_0, the bus status):
 * a frame is a BUF_DONE of subgroup RDI0, then the next buffer is given
 * to the write master (vfe_buf_done).
 *
 * Memory: the write master's IMAGE_ADDR is 32 bits, and FreeBSD hands the
 * TFE physical addresses, so the buffers are physically contiguous below
 * 4 GiB (contigmalloc) and the camera's SMMU streams (the node's iommus)
 * have translation off (camss_smmu.c); the multimedia TBUs' power domains
 * get our vote. The buffers are cached: written back and invalidated
 * before the TFE gets them, invalidated before user space does (DQBUF).
 */

#include <sys/param.h>
#ifdef _KERNEL
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/rman.h>
#include <sys/sx.h>
#include <sys/time.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/bus.h>
#include <machine/cpufunc.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#else
#include <stdint.h>
#ifndef roundup2
#define	roundup2(x, y)	(((x) + ((y) - 1)) & ~((y) - 1))
#endif
#endif

#include <dev/ofw/openfirm.h>

#include "camss_var.h"
#include "camss_stream.h"

/* csiphy_settle_cnt_calc() of camss-csiphy-3ph-1-0.c */
uint8_t
camss_settle_cnt(uint64_t link_freq, uint32_t timer_hz)
{
	uint32_t ui, timer_period, t_hs_settle;

	if (link_freq == 0 || timer_hz == 0)
		return (0);
	ui = (uint32_t)(1000000000000ULL / link_freq);
	ui /= 2;
	t_hs_settle = 85000 + 6 * ui;		/* t_hs_prepare_max */
	timer_period = (uint32_t)(1000000000000ULL / timer_hz);
	return ((uint8_t)(t_hs_settle / timer_period - 6));
}

/* packed RAW (bpp bits a pixel), lines CAMSS_BPL_ALIGN aligned */
uint32_t
camss_stride(uint32_t width, uint32_t bpp)
{
	return (roundup2(width * bpp / 8, CAMSS_BPL_ALIGN));
}

/* csid_get_lane_assign() */
uint32_t
camss_lane_assign(const uint8_t *lanes, u_int n)
{
	uint32_t a = 0;
	u_int i;

	for (i = 0; i < n; i++)
		a |= (uint32_t)lanes[i] << (i * 4);
	return (a);
}

#include "camss_gcc_sm6115.h"

/* camss.c's rate tables for CAMSS_2290 (csiphy/csid/vfe_res_2290) */
const uint32_t camss_rate_cphy[3] = { 240000000, 341330000, 384000000 };
const uint32_t camss_rate_timer[3] = { 100000000, 200000000, 268800000 };
const uint32_t camss_rate_csid[4] = { 192000000, 240000000, 384000000,
	426400000 };
const uint32_t camss_rate_tfe[6] = { 19200000, 153600000, 192000000,
	256000000, 384000000, 460800000 };

uint64_t
camss_margin(uint64_t hz)
{
	return (hz * CAMSS_CLOCK_MARGIN_NUM / CAMSS_CLOCK_MARGIN_DEN);
}

/*
 * camss.c's choice (csiphy/csid/vfe_set_clock_rates): the first rate of
 * the table above min, as the GCC table has it (qcom_find_freq: the first
 * entry at or above), here only if its source can run (usable). 0: none.
 */
uint32_t
camss_pick_rate(int rcg, const uint32_t *tbl, u_int n, uint64_t min,
    int (*usable)(int, uint32_t))
{
	const struct camss_rcg *r;
	u_int i, j;

	if (rcg < 0 || rcg >= CAMSS_NRCG)
		return (0);
	r = &camss_rcgs[rcg];
	for (i = 0; i < n; i++) {
		if (min >= tbl[i])
			continue;
		for (j = 0; j < r->nfreq; j++)
			if (r->freq[j].freq >= tbl[i])
				break;
		if (j < r->nfreq && usable(rcg, r->freq[j].freq))
			return (r->freq[j].freq);
	}
	return (0);
}

#ifdef _KERNEL

static MALLOC_DEFINE(M_CAMSS, "camss", "camera frame buffers");

static u_int camss_axi_hz;
TUNABLE_INT("hw.qcom_camss.axi_hz", &camss_axi_hz);

/* --- CSIPHY (camss-csiphy-3ph-1-0.c, regs->offset 0x800, status +0xb0) ----- */
#define	CSIPHY_CMN		0x800
#define	CSIPHY_CTRL(n)		(CSIPHY_CMN + 0x4 * (n))
#define	CSIPHY_STATUS(n)	(CSIPHY_CMN + 0xb0 + 0x4 * (n))
#define	 CTRL5_CLK_ENABLE	(1u << 7)
#define	 CTRL6_COMMON_PWRDN_B	(1u << 0)
#define	 CTRL6_SHOW_REV_ID	(1u << 1)

enum { CSIPHY_DEFAULT_PARAMS, CSIPHY_LANE_ENABLE, CSIPHY_SETTLE_CNT_LOWER_BYTE,
	CSIPHY_SETTLE_CNT_HIGHER_BYTE, CSIPHY_DNP_PARAMS, CSIPHY_2PH_REGS,
	CSIPHY_3PH_REGS, CSIPHY_SKEW_CAL };
struct csiphy_lane_reg {
	uint32_t	reg_addr, reg_data, delay_us, csiphy_param_type;
};
#include "camss_csiphy_qcm2290.h"

/* --- CSID 340 (camss-csid-340.c) ------------------------------------------- */
#define	CSID_HW_VERSION		0x000
#define	CSID_RST_STROBES	0x010
#define	 CSID_RST_IRQ		(1u << 1)
#define	 CSID_RST_IFE_CLK	(1u << 2)
#define	 CSID_RST_PHY_CLK	(1u << 3)
#define	 CSID_RST_CSID_CLK	(1u << 4)
#define	CSID_IRQ_STATUS		0x070
#define	CSID_IRQ_MASK		0x074
#define	 CSID_IRQ_RST_DONE	(1u << 0)
#define	CSID_IRQ_CLEAR		0x078
#define	CSID_IRQ_CMD		0x080
#define	 CSID_IRQ_CMD_CLEAR	(1u << 0)
#define	CSID_CSI2_RX_CFG0	0x100
#define	 RX_CFG0_PHY_NUM_SEL_SHIFT	20
#define	 RX_CFG0_DLX_INPUT_SEL_SHIFT	4
#define	CSID_CSI2_RX_CFG1	0x104
#define	 RX_CFG1_ECC_CORRECTION_EN	(1u << 0)
#define	CSID_RDI_CFG0(r)	(0x300 + 0x100 * (r))
#define	 RDI_CFG0_DECODE_FORMAT_NOP	(0xfu << 12)
#define	 RDI_CFG0_DT_SHIFT	16
#define	 RDI_CFG0_VC_SHIFT	22
#define	 RDI_CFG0_DTID_SHIFT	27
#define	 RDI_CFG0_ENABLE	(1u << 31)
#define	CSID_RDI_CTRL(r)	(0x308 + 0x100 * (r))
#define	MIPI_CSI2_DT_RAW10	0x2b
#define	CSID_RESET_TIMEOUT_MS	500

/* --- TFE 340 (camss-vfe-340.c) ----------------------------------------------- */
#define	TFE_HW_VERSION		0x000
#define	TFE_GLOBAL_RESET_CMD	0x014
#define	 TFE_GLOBAL_RESET_CORE	(1u << 0)
#define	TFE_REG_UPDATE_CMD	0x02c
#define	TFE_IRQ_CMD		0x030
#define	 TFE_IRQ_CMD_CLEAR	(1u << 0)
#define	TFE_IRQ_MASK_0		0x034
#define	 TFE_IRQ_RST_DONE	(1u << 0)
#define	 TFE_IRQ_BUS_WR		(1u << 1)
#define	TFE_IRQ_CLEAR_0		0x040
#define	TFE_IRQ_STATUS_0	0x04c
#define	BUS_REG(a)		(0xa00 + (a))
#define	TFE_BUS_IRQ_MASK_0	BUS_REG(0x18)
#define	 BUS_RUP_DONE(i)	(1u << (i))		/* iface */
#define	 BUS_RUP_DONE_MASK	0xfu
#define	 BUS_BUF_DONE(g)	(1u << (8 + (g)))	/* subgroup */
#define	 BUS_BUF_DONE_MASK	(0xffu << 8)
#define	 BUS_CONS_VIOL		(1u << 28)
#define	 BUS_VIOL		(1u << 30)
#define	 BUS_IMG_VIOL		(1u << 31)
#define	TFE_BUS_IRQ_CLEAR_0	BUS_REG(0x20)
#define	TFE_BUS_IRQ_STATUS_0	BUS_REG(0x28)
#define	TFE_BUS_IRQ_CMD		BUS_REG(0x30)
#define	TFE_BUS_STATUS_CLEAR	BUS_REG(0x60)
#define	TFE_BUS_VIOLATION_STATUS BUS_REG(0x64)
#define	TFE_BUS_OVERFLOW_STATUS	BUS_REG(0x68)
#define	TFE_BUS_IMAGE_SZ_VIOLATION_STATUS BUS_REG(0x70)
#define	TFE_BUS_CLIENT_CFG(c)	BUS_REG(0x200 + (c) * 0x100)
#define	 CLIENT_CFG_EN		(1u << 0)
#define	 CLIENT_CFG_MODE_FRAME	(1u << 16)
#define	TFE_BUS_IMAGE_ADDR(c)	BUS_REG(0x204 + (c) * 0x100)
#define	TFE_BUS_FRAME_INCR(c)	BUS_REG(0x208 + (c) * 0x100)
#define	TFE_BUS_IMAGE_CFG_0(c)	BUS_REG(0x20c + (c) * 0x100)
#define	TFE_BUS_IMAGE_CFG_1(c)	BUS_REG(0x210 + (c) * 0x100)
#define	TFE_BUS_IMAGE_CFG_2(c)	BUS_REG(0x214 + (c) * 0x100)
#define	TFE_BUS_PACKER_CFG(c)	BUS_REG(0x218 + (c) * 0x100)
#define	 PACKER_CFG_FMT_PLAIN64	0xa
#define	TFE_BUS_IRQ_SUBSAMPLE_CFG_0(c) BUS_REG(0x230 + (c) * 0x100)
#define	TFE_BUS_IRQ_SUBSAMPLE_CFG_1(c) BUS_REG(0x234 + (c) * 0x100)
#define	TFE_BUS_FRAMEDROP_CFG_0(c) BUS_REG(0x238 + (c) * 0x100)
#define	TFE_BUS_FRAMEDROP_CFG_1(c) BUS_REG(0x23c + (c) * 0x100)
#define	RDI_WM(n)		(7 + (n))	/* write masters 7-9 */
#define	TFE_IFACE_RDI0		1		/* REG_UPDATE bit, RUP_DONE */
#define	TFE_SUBGROUP_RDI0	5		/* BUF_DONE */
#define	VFE_RESET_TIMEOUT_MS	50

#define	CR(r, o)	bus_read_4((r), (o))
#define	CW(r, o, v)	bus_write_4((r), (o), (v))

/* the device tree's clock-names of the camss node -> GCC branch */
static int
dt_clock(struct camss_softc *sc, const char *name)
{
	pcell_t *clk = NULL;
	char *names = NULL;
	ssize_t nclk, nlen, off;
	int i, b = -1;

	nclk = OF_getencprop_alloc_multi(sc->node, "clocks", sizeof(pcell_t),
	    (void **)&clk);
	nlen = OF_getprop_alloc(sc->node, "clock-names", (void **)&names);
	for (i = 0, off = 0; names != NULL && off < nlen && 2 * i + 1 < nclk;
	    off += strlen(names + off) + 1, i++)
		if (strcmp(names + off, name) == 0) {
			b = camss_clk_by_dt(clk[2 * i], clk[2 * i + 1]);
			break;
		}
	OF_prop_free(clk);
	OF_prop_free(names);
	return (b);
}

static int
reg_index(struct camss_softc *sc, const char *name)
{
	char *names = NULL;
	ssize_t len, off;
	int i, idx = -1;

	len = OF_getprop_alloc(sc->node, "reg-names", (void **)&names);
	for (i = 0, off = 0; names != NULL && off < len;
	    off += strlen(names + off) + 1, i++)
		if (strcmp(names + off, name) == 0) {
			idx = i;
			break;
		}
	OF_prop_free(names);
	return (idx);
}

static uint32_t
pick(int rcg, const uint32_t *tbl, u_int n, uint64_t min)
{
	return (camss_pick_rate(rcg, tbl, n, min, camss_clk_reparent_ok));
}

int
camss_stream_attach(struct camss_softc *sc)
{
	sc->gdsc = -1;
	sc->hw[0] = sc->hw[1] = -1;
	return (0);
}

static void
free_bufs(struct camss_softc *sc)
{
	u_int i;

	for (i = 0; i < CAMSS_MAX_BUFS; i++) {
		if (sc->buf[i].va != NULL)
			contigfree(sc->buf[i].va, sc->bufsize, M_CAMSS);
		sc->buf[i].va = NULL;
	}
	sc->nbufs = 0;
	sc->bufsize = 0;
}

void
camss_stream_detach(struct camss_softc *sc)
{
	camss_stream_stop(sc);
	free_bufs(sc);
}

static struct resource *
map(struct camss_softc *sc, const char *name, int *rid)
{
	struct resource *r;

	if ((*rid = reg_index(sc, name)) < 0) {
		device_printf(sc->dev, "no register window %s\n", name);
		return (NULL);
	}
	r = bus_alloc_resource_any(sc->dev, SYS_RES_MEMORY, rid, RF_ACTIVE);
	if (r == NULL)
		device_printf(sc->dev, "%s: cannot map\n", name);
	return (r);
}

static void
unmap(struct camss_softc *sc)
{
	if (sc->csiphy != NULL)
		bus_release_resource(sc->dev, SYS_RES_MEMORY, sc->csiphy_rid,
		    sc->csiphy);
	if (sc->csid != NULL)
		bus_release_resource(sc->dev, SYS_RES_MEMORY, sc->csid_rid,
		    sc->csid);
	if (sc->vfe != NULL)
		bus_release_resource(sc->dev, SYS_RES_MEMORY, sc->vfe_rid,
		    sc->vfe);
	sc->csiphy = sc->csid = sc->vfe = NULL;
}

static int
clk(struct camss_softc *sc, const char *name, uint32_t hz)
{
	int b, error;

	if ((b = dt_clock(sc, name)) < 0) {
		device_printf(sc->dev, "no clock %s in the device tree\n", name);
		return (ENXIO);
	}
	if ((error = camss_clk_on(b, hz, "camss")) != 0)
		return (error);
	sc->clk[sc->nclk++] = b;
	return (0);
}

static void
clocks_off(struct camss_softc *sc)
{
	while (sc->nclk > 0)
		camss_clk_off(sc->clk[--sc->nclk], "camss");
}

/* one of the TFE's or CSID's status registers until bit, or timeout */
static int
wait_bit(struct resource *r, uint32_t status, uint32_t bit, u_int ms)
{
	u_int us;

	for (us = 0; us < ms * 1000; us += 10) {
		if (CR(r, status) & bit)
			return (0);
		DELAY(10);
	}
	return (ETIMEDOUT);
}

/* --- power: CSIPHY, CSID, TFE ----------------------------------------------- */

static void
csiphy_power(struct camss_softc *sc)
{
	uint32_t v;

	CW(sc->csiphy, CSIPHY_CTRL(6), CTRL6_SHOW_REV_ID);
	v = (CR(sc->csiphy, CSIPHY_STATUS(12)) & 0xff) |
	    (CR(sc->csiphy, CSIPHY_STATUS(13)) & 0xff) << 8 |
	    (CR(sc->csiphy, CSIPHY_STATUS(14)) & 0xff) << 16 |
	    (CR(sc->csiphy, CSIPHY_STATUS(15)) & 0xff) << 24;
	device_printf(sc->dev, "CSIPHY%u HW version %#x\n", sc->port, v);
	/* csiphy_reset() */
	CW(sc->csiphy, CSIPHY_CTRL(0), 0x1);
	pause_sbt("csiphy", SBT_1MS * 6, 0, 0);
	CW(sc->csiphy, CSIPHY_CTRL(0), 0x0);
}

static int
csid_power(struct camss_softc *sc)
{
	uint32_t st;
	int error;

	/* csid_reset(): registers preserved, RST_DONE in the status */
	CW(sc->csid, CSID_IRQ_MASK, CSID_IRQ_RST_DONE);
	CW(sc->csid, CSID_IRQ_CLEAR, CSID_IRQ_RST_DONE);
	CW(sc->csid, CSID_IRQ_CMD, CSID_IRQ_CMD_CLEAR);
	CW(sc->csid, CSID_RST_STROBES, CSID_RST_IRQ | CSID_RST_IFE_CLK |
	    CSID_RST_PHY_CLK | CSID_RST_CSID_CLK);
	error = wait_bit(sc->csid, CSID_IRQ_STATUS, CSID_IRQ_RST_DONE,
	    CSID_RESET_TIMEOUT_MS);
	st = CR(sc->csid, CSID_IRQ_STATUS);
	CW(sc->csid, CSID_IRQ_CLEAR, st);
	CW(sc->csid, CSID_IRQ_CMD, CSID_IRQ_CMD_CLEAR);
	device_printf(sc->dev, "CSID0 HW version %#x, reset %s (status %#x)\n",
	    CR(sc->csid, CSID_HW_VERSION), error ? "timeout" : "done", st);
	return (error);
}

static int
tfe_reset(struct camss_softc *sc)
{
	uint32_t st;
	int error;

	CW(sc->vfe, TFE_IRQ_MASK_0, TFE_IRQ_RST_DONE);
	CW(sc->vfe, TFE_GLOBAL_RESET_CMD, TFE_GLOBAL_RESET_CORE);
	error = wait_bit(sc->vfe, TFE_IRQ_STATUS_0, TFE_IRQ_RST_DONE,
	    VFE_RESET_TIMEOUT_MS);
	st = CR(sc->vfe, TFE_IRQ_STATUS_0);
	CW(sc->vfe, TFE_IRQ_CLEAR_0, st);
	CW(sc->vfe, TFE_IRQ_CMD, TFE_IRQ_CMD_CLEAR);
	return (error);
}

/* --- the write master ----------------------------------------------------- */

static void
reg_update(struct camss_softc *sc)
{
	sc->reg_update |= 1u << TFE_IFACE_RDI0;
	CW(sc->vfe, TFE_REG_UPDATE_CMD, sc->reg_update);
}

static void
give(struct camss_softc *sc, int slot, int i)
{
	sc->hw[slot] = i;
	sc->buf[i].state = BUF_HW;
	CW(sc->vfe, TFE_BUS_IMAGE_ADDR(RDI_WM(0)), (uint32_t)sc->buf[i].pa);
	reg_update(sc);
}

static int
next_queued(struct camss_softc *sc)
{
	u_int i;

	for (i = 0; i < sc->nbufs; i++)
		if (sc->buf[i].state == BUF_QUEUED)
			return (i);
	return (-1);
}

/* vfe_wm_start() for RDI 0, then vfe_enable_output_v2()'s two buffers */
static void
wm_start(struct camss_softc *sc)
{
	u_int wm = RDI_WM(0);
	int i;

	CW(sc->vfe, TFE_BUS_IMAGE_CFG_0(wm), 0xffff);
	CW(sc->vfe, TFE_BUS_IMAGE_CFG_1(wm), 0);
	CW(sc->vfe, TFE_BUS_IMAGE_CFG_2(wm), 0xffff);
	CW(sc->vfe, TFE_BUS_FRAME_INCR(wm), sc->cfg.stride * sc->cfg.height);
	CW(sc->vfe, TFE_BUS_PACKER_CFG(wm), PACKER_CFG_FMT_PLAIN64);
	CW(sc->vfe, TFE_BUS_FRAMEDROP_CFG_0(wm), 0);
	CW(sc->vfe, TFE_BUS_FRAMEDROP_CFG_1(wm), 1);
	CW(sc->vfe, TFE_BUS_IRQ_SUBSAMPLE_CFG_0(wm), 0);
	CW(sc->vfe, TFE_BUS_IRQ_SUBSAMPLE_CFG_1(wm), 1);
	/* vfe_enable_irq() */
	CW(sc->vfe, TFE_IRQ_MASK_0, TFE_IRQ_RST_DONE | TFE_IRQ_BUS_WR);
	CW(sc->vfe, TFE_BUS_IRQ_MASK_0, BUS_RUP_DONE_MASK | BUS_BUF_DONE_MASK |
	    BUS_CONS_VIOL | BUS_VIOL | BUS_IMG_VIOL);
	CW(sc->vfe, TFE_BUS_CLIENT_CFG(wm), CLIENT_CFG_EN | CLIENT_CFG_MODE_FRAME);
	sc->reg_update = 0;
	for (i = 0; i < 2; i++) {
		int b = next_queued(sc);

		if (b < 0)
			break;
		give(sc, i, b);
	}
}

/* --- start, stop -------------------------------------------------------------- */

int
camss_stream_start(struct camss_softc *sc, struct camss_stream *s)
{
	const struct camss_sensor *cs;
	pcell_t *iommus = NULL;
	ssize_t n;
	char name[16];
	uint32_t cphy, timer, csid, tfe, v;
	uint64_t min;
	u_int i;
	int error, b;

	if (sc->streaming)
		return (EBUSY);
	if (s->sensor >= sc->info.nsensors || s->bpp != 10 ||
	    s->width == 0 || s->height == 0 || s->link_freq == 0 ||
	    s->pixel_rate == 0 || s->nbufs < 2 || s->nbufs > CAMSS_MAX_BUFS)
		return (EINVAL);
	cs = &sc->info.sensor[s->sensor];
	if (cs->port > 2 || cs->nlanes == 0 || cs->nlanes > 4)
		return (ENXIO);
	sc->cfg = *s;
	sc->port = cs->port;
	sc->cfg.stride = camss_stride(s->width, s->bpp);
	min = camss_margin(s->link_freq / 4);
	snprintf(name, sizeof(name), "csiphy%u", sc->port);
	b = dt_clock(sc, name);
	cphy = b >= 0 ? pick(camss_branches[b].rcg, camss_rate_cphy,
	    nitems(camss_rate_cphy), min) : 0;
	snprintf(name, sizeof(name), "csiphy%u_timer", sc->port);
	b = dt_clock(sc, name);
	timer = b >= 0 ? pick(camss_branches[b].rcg, camss_rate_timer,
	    nitems(camss_rate_timer), min) : 0;
	b = dt_clock(sc, "csi0");
	csid = b >= 0 ? pick(camss_branches[b].rcg, camss_rate_csid,
	    nitems(camss_rate_csid), min) : 0;
	b = dt_clock(sc, "vfe0");
	tfe = b >= 0 ? pick(camss_branches[b].rcg, camss_rate_tfe,
	    nitems(camss_rate_tfe), camss_margin(s->pixel_rate * s->bpp / 64)) : 0;
	device_printf(sc->dev, "stream: sensor %u CSIPHY%u %u lanes, %ux%u "
	    "RAW%u, link %ju Hz, pixel rate %ju Hz: CPHY %u, timer %u, CSID "
	    "%u, TFE %u Hz\n", s->sensor, sc->port, cs->nlanes, s->width,
	    s->height, s->bpp, (uintmax_t)s->link_freq,
	    (uintmax_t)s->pixel_rate, cphy, timer, csid, tfe);
	if (cphy == 0 || timer == 0 || csid == 0 || tfe == 0)
		return (ERANGE);
	sc->cfg.clk_cphy = cphy;
	sc->cfg.clk_timer = timer;
	sc->cfg.clk_csid = csid;
	sc->cfg.clk_tfe = tfe;
	sc->cfg.settle_cnt = camss_settle_cnt(s->link_freq, timer);

	/* the buffers: contiguous, below 4 GiB, kept until the size changes */
	v = round_page(sc->cfg.stride * s->height);
	if (v != sc->bufsize || s->nbufs > sc->nbufs) {
		free_bufs(sc);
		for (i = 0; i < s->nbufs; i++) {
			sc->buf[i].va = contigmalloc(v, M_CAMSS, M_WAITOK | M_ZERO,
			    0, 0xffffffffUL, PAGE_SIZE, 0);
			if (sc->buf[i].va == NULL) {
				sc->bufsize = v;
				free_bufs(sc);
				return (ENOMEM);
			}
			sc->buf[i].pa = vtophys(sc->buf[i].va);
		}
		sc->bufsize = v;
	}
	sc->nbufs = s->nbufs;
	sc->cfg.bufsize = sc->bufsize;
	for (i = 0; i < sc->nbufs; i++) {
		cpu_dcache_wbinv_range(sc->buf[i].va, sc->bufsize);
		sc->buf[i].state = BUF_QUEUED;
		sc->buf[i].sequence = 0;
		sc->buf[i].flags = 0;
	}
	sc->hw[0] = sc->hw[1] = -1;
	sc->sequence = 0;
	sc->overflow = sc->violation = 0;
	sc->frames = sc->errors = 0;

	/* power domain, the multimedia TBUs, the SMMU streams */
	if ((error = camss_gdsc_get(CAMSS_GDSC_CAMSS_TOP, "camss")) != 0)
		return (error);
	sc->gdsc = 0;
	(void)camss_gdsc_vote(CAMSS_GDSC_TBU_RT, "camss");
	(void)camss_gdsc_vote(CAMSS_GDSC_TBU_NRT, "camss");
	n = OF_getencprop_alloc_multi(sc->node, "iommus", sizeof(pcell_t),
	    (void **)&iommus);
	/* <&apps_smmu SID MASK> each (#iommu-cells = 2) */
	for (i = 0, error = 0; n > 0 && i + 3 <= (u_int)n; i += 3)
		if ((error = camss_smmu_bypass(iommus[i], iommus[i + 1],
		    iommus[i + 2], "camss")) != 0)
			break;
	OF_prop_free(iommus);
	if (error != 0)
		goto fail;

	/* clocks: the buses as found (axi: hw.qcom_camss.axi_hz), then the
	 * path's at the rates above; vfe0_cphy_rx shares the CPHY's RCG */
	if ((error = clk(sc, "top_ahb", 0)) || (error = clk(sc, "ahb", 0)) ||
	    (error = clk(sc, "axi", camss_axi_hz)) ||
	    (error = clk(sc, "camnoc_rt_axi", 0)) ||
	    (error = clk(sc, "camnoc_nrt_axi", 0)))
		goto fail;
	snprintf(name, sizeof(name), "csiphy%u", sc->port);
	if ((error = clk(sc, name, cphy)))
		goto fail;
	snprintf(name, sizeof(name), "csiphy%u_timer", sc->port);
	if ((error = clk(sc, name, timer)) || (error = clk(sc, "csi0", csid)) ||
	    (error = clk(sc, "vfe0_cphy_rx", 0)) ||
	    (error = clk(sc, "vfe0", tfe)))
		goto fail;

	snprintf(name, sizeof(name), "csiphy%u", sc->port);
	if ((sc->csiphy = map(sc, name, &sc->csiphy_rid)) == NULL ||
	    (sc->csid = map(sc, "csid0", &sc->csid_rid)) == NULL ||
	    (sc->vfe = map(sc, "vfe0", &sc->vfe_rid)) == NULL) {
		error = ENXIO;
		goto fail;
	}

	/* power: csiphy_set_power, csid_set_power, vfe_get */
	csiphy_power(sc);
	if ((error = csid_power(sc)) != 0)
		goto fail;
	if ((error = tfe_reset(sc)) != 0) {
		device_printf(sc->dev, "TFE0 reset: timeout\n");
		goto fail;
	}
	device_printf(sc->dev, "TFE0 HW version %#x\n",
	    CR(sc->vfe, TFE_HW_VERSION));

	/* stream on, from the memory end: the write master */
	wm_start(sc);
	/* the CSID: receiver from CSIPHY port, RDI 0 for RAW10 on VC 0 */
	v = (cs->nlanes - 1) | camss_lane_assign(cs->lanes, cs->nlanes) <<
	    RX_CFG0_DLX_INPUT_SEL_SHIFT |
	    (sc->port + 1) << RX_CFG0_PHY_NUM_SEL_SHIFT;
	CW(sc->csid, CSID_CSI2_RX_CFG0, v);
	CW(sc->csid, CSID_CSI2_RX_CFG1, RX_CFG1_ECC_CORRECTION_EN);
	CW(sc->csid, CSID_RDI_CFG0(0), RDI_CFG0_DECODE_FORMAT_NOP |
	    MIPI_CSI2_DT_RAW10 << RDI_CFG0_DT_SHIFT | 0 << RDI_CFG0_VC_SHIFT |
	    0 << RDI_CFG0_DTID_SHIFT | RDI_CFG0_ENABLE);
	CW(sc->csid, CSID_RDI_CTRL(0), 1);	/* resume at frame boundary */
	/* the CSIPHY's lanes: csiphy_lanes_enable() */
	v = CTRL5_CLK_ENABLE;
	for (i = 0; i < cs->nlanes; i++)
		v |= 1u << (cs->lanes[i] * 2);
	CW(sc->csiphy, CSIPHY_CTRL(5), v);
	CW(sc->csiphy, CSIPHY_CTRL(6), CTRL6_COMMON_PWRDN_B);
	CW(sc->csiphy, CSIPHY_CTRL(7), 0x02);
	CW(sc->csiphy, CSIPHY_CTRL(0), 0x00);
	for (i = 0; i < nitems(csiphy_lane_regs_qcm2290); i++) {
		const struct csiphy_lane_reg *r = &csiphy_lane_regs_qcm2290[i];

		if (r->csiphy_param_type == CSIPHY_SKEW_CAL ||
		    r->csiphy_param_type == CSIPHY_DNP_PARAMS)
			continue;
		CW(sc->csiphy, r->reg_addr, r->csiphy_param_type ==
		    CSIPHY_SETTLE_CNT_LOWER_BYTE ? (sc->cfg.settle_cnt & 0xff) :
		    r->reg_data);
		if (r->delay_us)
			DELAY(r->delay_us);
	}
	for (i = 11; i < 22; i++)
		CW(sc->csiphy, CSIPHY_CTRL(i), 0);
	sc->streaming = 1;
	device_printf(sc->dev, "stream on: RX_CFG0 %#x, lanes %#x, settle %u, "
	    "%u buffers of %u bytes (stride %u)\n", CR(sc->csid,
	    CSID_CSI2_RX_CFG0), CR(sc->csiphy, CSIPHY_CTRL(5)),
	    sc->cfg.settle_cnt, sc->nbufs, sc->bufsize, sc->cfg.stride);
	*s = sc->cfg;
	return (0);
fail:
	device_printf(sc->dev, "stream start failed: %d\n", error);
	sc->streaming = 1;		/* for the way back */
	camss_stream_stop(sc);
	return (error);
}

int
camss_stream_stop(struct camss_softc *sc)
{
	u_int i;

	if (!sc->streaming)
		return (0);
	if (sc->csid != NULL) {
		CW(sc->csid, CSID_RDI_CTRL(0), 0);	/* halt at frame boundary */
		CW(sc->csid, CSID_RDI_CFG0(0), CR(sc->csid, CSID_RDI_CFG0(0)) &
		    ~RDI_CFG0_ENABLE);
	}
	if (sc->vfe != NULL) {
		CW(sc->vfe, TFE_BUS_CLIENT_CFG(RDI_WM(0)), 0);
		(void)tfe_reset(sc);
	}
	if (sc->csiphy != NULL) {
		CW(sc->csiphy, CSIPHY_CTRL(5), 0);
		CW(sc->csiphy, CSIPHY_CTRL(6), 0);
	}
	unmap(sc);
	clocks_off(sc);
	if (sc->gdsc == 0)
		camss_gdsc_put(CAMSS_GDSC_CAMSS_TOP, "camss");
	sc->gdsc = -1;
	for (i = 0; i < sc->nbufs; i++)
		sc->buf[i].state = BUF_USER;
	sc->hw[0] = sc->hw[1] = -1;
	sc->streaming = 0;
	device_printf(sc->dev, "stream off: %u frames, %u with errors, "
	    "overflow %#x, violation %#x\n", sc->frames, sc->errors,
	    sc->overflow, sc->violation);
	return (0);
}

/* --- frames ----------------------------------------------------------------- */

/* the interrupt status as vfe_isr() handles it; 1 if a frame completed */
static int
poll_tfe(struct camss_softc *sc)
{
	uint32_t st, bs = 0, ov, vio, img;
	int done, i, ready;

	st = CR(sc->vfe, TFE_IRQ_STATUS_0);
	if (st == 0)
		goto overflow;
	CW(sc->vfe, TFE_IRQ_CLEAR_0, st);
	CW(sc->vfe, TFE_IRQ_CMD, TFE_IRQ_CMD_CLEAR);
	if ((st & TFE_IRQ_BUS_WR) == 0)
		goto overflow;
	bs = CR(sc->vfe, TFE_BUS_IRQ_STATUS_0);
	CW(sc->vfe, TFE_BUS_IRQ_CLEAR_0, bs);
	CW(sc->vfe, TFE_BUS_IRQ_CMD, 1);
	if (bs & BUS_RUP_DONE(TFE_IFACE_RDI0))
		sc->reg_update &= ~(1u << TFE_IFACE_RDI0);
	if (bs & (BUS_CONS_VIOL | BUS_VIOL | BUS_IMG_VIOL)) {
		vio = CR(sc->vfe, TFE_BUS_VIOLATION_STATUS);
		img = CR(sc->vfe, TFE_BUS_IMAGE_SZ_VIOLATION_STATUS);
		sc->violation |= bs & (BUS_CONS_VIOL | BUS_VIOL | BUS_IMG_VIOL);
		if (sc->errors++ < 8)
			device_printf(sc->dev, "TFE0 bus status %#x: violation "
			    "%#x, image size violation %#x\n", bs, vio, img);
		if (sc->hw[0] >= 0)
			sc->buf[sc->hw[0]].flags |= CAMSS_BUF_ERROR;
	}
overflow:
	ov = CR(sc->vfe, TFE_BUS_OVERFLOW_STATUS);
	if (ov != 0) {
		CW(sc->vfe, TFE_BUS_STATUS_CLEAR, ov);
		sc->overflow |= ov;
		if (sc->errors++ < 8)
			device_printf(sc->dev, "TFE0 bus overflow %#x\n", ov);
		if (sc->hw[0] >= 0)
			sc->buf[sc->hw[0]].flags |= CAMSS_BUF_ERROR;
	}
	done = 0;
	if (bs & BUS_BUF_DONE(TFE_SUBGROUP_RDI0)) {
		/* vfe_buf_done() */
		if ((ready = sc->hw[0]) < 0) {
			if (sc->errors++ < 8)
				device_printf(sc->dev, "frame done without a "
				    "buffer\n");
			return (0);
		}
		sc->buf[ready].state = BUF_DONE;
		sc->buf[ready].sequence = sc->sequence++;
		sc->buf[ready].ts = sbttons(sbinuptime());
		sc->buf[ready].tfe_status = st;
		sc->buf[ready].bus_status = bs;
		sc->hw[0] = sc->hw[1];
		sc->hw[1] = -1;
		i = sc->hw[0] >= 0 ? 1 : 0;
		if ((ready = next_queued(sc)) >= 0)
			give(sc, i, ready);
		sc->frames++;
		done = 1;
	}
	return (done);
}

int
camss_stream_dqbuf(struct camss_softc *sc, struct camss_buf *b)
{
	sbintime_t end;
	u_int i;
	int best;

	if (!sc->streaming)
		return (EINVAL);
	end = sbinuptime() + SBT_1MS * MAX(b->timeout_ms, 1);
	for (;;) {
		(void)poll_tfe(sc);
		best = -1;
		for (i = 0; i < sc->nbufs; i++)
			if (sc->buf[i].state == BUF_DONE && (best < 0 ||
			    sc->buf[i].sequence < sc->buf[best].sequence))
				best = i;
		if (best >= 0)
			break;
		if (sbinuptime() >= end) {
			b->tfe_status = CR(sc->vfe, TFE_IRQ_STATUS_0);
			b->bus_status = CR(sc->vfe, TFE_BUS_IRQ_STATUS_0);
			b->overflow = sc->overflow;
			b->violation = sc->violation;
			return (EAGAIN);
		}
		pause_sbt("camssdq", SBT_1MS, 0, 0);
	}
	cpu_dcache_inv_range(sc->buf[best].va, sc->bufsize);
	sc->buf[best].state = BUF_USER;
	b->index = best;
	b->sequence = sc->buf[best].sequence;
	b->flags = sc->buf[best].flags;
	b->timestamp_ns = sc->buf[best].ts;
	b->tfe_status = sc->buf[best].tfe_status;
	b->bus_status = sc->buf[best].bus_status;
	b->overflow = sc->overflow;
	b->violation = sc->violation;
	sc->buf[best].flags = 0;
	return (0);
}

int
camss_stream_qbuf(struct camss_softc *sc, struct camss_buf *b)
{
	int slot;

	if (!sc->streaming || b->index >= sc->nbufs ||
	    sc->buf[b->index].state != BUF_USER)
		return (EINVAL);
	/* nothing of the CPU's may be written back over the frame later */
	cpu_dcache_wbinv_range(sc->buf[b->index].va, sc->bufsize);
	sc->buf[b->index].state = BUF_QUEUED;
	/* vfe_queue_buffer_v2(): straight to the write master if it has
	 * fewer than two */
	slot = sc->hw[0] < 0 ? 0 : (sc->hw[1] < 0 ? 1 : -1);
	if (slot >= 0)
		give(sc, slot, b->index);
	return (0);
}

int
camss_stream_regdump(struct camss_softc *sc, struct camss_regdump *r)
{
	struct resource *res;
	u_int i;

	if (!sc->streaming)
		return (EINVAL);	/* the windows are clocked only then */
	if (strncmp(r->name, "csiphy", 6) == 0)
		res = sc->csiphy;
	else if (strcmp(r->name, "csid0") == 0)
		res = sc->csid;
	else if (strcmp(r->name, "vfe0") == 0)
		res = sc->vfe;
	else
		return (ENOENT);
	if (r->count > nitems(r->val) || (r->offset & 3) != 0 ||
	    r->offset + 4 * r->count > rman_get_size(res))
		return (EINVAL);
	for (i = 0; i < r->count; i++)
		r->val[i] = CR(res, r->offset + 4 * i);
	return (0);
}

int
camss_stream_mmap(struct camss_softc *sc, vm_ooffset_t off, vm_paddr_t *pa,
    vm_memattr_t *memattr)
{
	u_int i;

	if (sc->bufsize == 0 || off < 0)
		return (EINVAL);
	i = off / sc->bufsize;
	if (i >= sc->nbufs || sc->buf[i].va == NULL)
		return (EINVAL);
	*pa = sc->buf[i].pa + (off % sc->bufsize);
	*memattr = VM_MEMATTR_WRITE_BACK;
	return (0);
}
#endif /* _KERNEL */
