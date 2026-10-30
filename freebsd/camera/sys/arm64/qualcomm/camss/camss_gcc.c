/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The SM6115 global clock controller's camera part, for the CCI and CAMSS
 * drivers: FreeBSD has no clock driver for this SoC, and Linux's boot menu
 * leaves the camera island off (nothing in its initramfs uses it; the
 * clock framework gates and parks what is unused). Every offset, bit and
 * frequency is Linux's (gcc-sm6115.c via camss_gcc_sm6115.h, made by
 * freebsd/camera/gen/gen-tables.py); the sequences are those of Linux's
 * clk-branch.c, clk-rcg2.c (clk_rcg2_shared_ops), clk-alpha-pll.c and
 * gdsc.c:
 *
 * - a branch clock: its enable bit, then wait for the halt register's
 *   CLK_OFF to clear (BRANCH_HALT, _VOTED) or 10 us (_DELAY)
 * - an RCG: the frequency table entry's source select and divider
 *   (2 * h - 1) in CFG, M/N/D when the entry has N; written with the root
 *   forced on (CMD ROOT_EN), then UPDATE, as clk_rcg2_shared_enable does
 * - a PLL the source needs: voted on (FSM mode) or the manual sequence,
 *   only if Linux configured it (its L value set)
 * - the camera's power domain GCC_CAMSS_TOP_GDSC: the wait times
 *   gdsc_init writes, SW_COLLAPSE cleared, PWR_ON waited for
 *
 * Without a rate (0) an RCG is left as found and its rate reported.
 * Linux's CLK_IS_CRITICAL clocks (camera AHB, camera XO) are never turned
 * off. Nothing else in GCC is written.
 */

#include <sys/param.h>
#ifdef _KERNEL
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/sx.h>
#include <sys/sbuf.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/bus.h>
#include <machine/cpufunc.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_subr.h>
#else
#include <errno.h>
#include <stdint.h>
#include <string.h>
#endif

#include "camss_var.h"
#include "camss_gcc_sm6115.h"

#define	XO_HZ		19200000ULL

/* --- the RCG words (also for the host tests) ------------------------------------ */

static const struct camss_freq *
rcg_entry(u_int rcg, uint32_t hz)
{
	const struct camss_rcg *r = &camss_rcgs[rcg];
	u_int i;

	for (i = 0; i < r->nfreq; i++)
		if (r->freq[i].freq == hz)
			return (&r->freq[i]);
	return (NULL);
}

/*
 * What __clk_rcg2_configure() writes for the table entry of hz: CFG's
 * source select, divider and mode under its mask, and M, N (~(n - m)),
 * D (~clamp(n, m, 2 * (n - m))) under the M/N/D width.
 */
int
camss_rcg_encode(u_int rcg, uint32_t hz, struct camss_rcg_words *w)
{
	const struct camss_rcg *r;
	const struct camss_freq *f;
	uint32_t d, nm2;

	if (rcg >= CAMSS_NRCG)
		return (EINVAL);
	r = &camss_rcgs[rcg];
	if ((f = rcg_entry(rcg, hz)) == NULL || r->sel[f->src] < 0)
		return (EINVAL);
	memset(w, 0, sizeof(*w));
	w->cfg_mask = ((1u << r->hid) - 1) | RCG_CFG_SRC_SEL_MASK |
	    RCG_CFG_MODE_MASK | RCG_CFG_HW_CLK_CTRL;
	w->cfg = (uint32_t)f->pre_div |
	    (uint32_t)r->sel[f->src] << RCG_CFG_SRC_SEL_SHIFT;
	if (r->mnd != 0 && f->n != 0) {
		w->mnd = 1;
		w->mnd_mask = (1u << r->mnd) - 1;
		w->m = f->m & w->mnd_mask;
		w->n = ~(uint32_t)(f->n - f->m) & w->mnd_mask;
		nm2 = (f->n - f->m) * 2;
		d = f->n;
		if (d < f->m)
			d = f->m;
		if (d > nm2)
			d = nm2;
		w->d = ~d & w->mnd_mask;
		if (f->m != f->n)
			w->cfg |= RCG_CFG_MODE_DUAL_EDGE;
	}
	return (0);
}

/* the rate of an RCG's CFG (and M, N), given its sources' rates */
uint64_t
camss_rcg_decode(u_int rcg, uint32_t cfg, uint32_t m, uint32_t n,
    const uint64_t *src_hz)
{
	const struct camss_rcg *r = &camss_rcgs[rcg];
	uint32_t sel, div, mask;
	uint64_t hz;
	u_int s;

	sel = (cfg & RCG_CFG_SRC_SEL_MASK) >> RCG_CFG_SRC_SEL_SHIFT;
	for (s = 0; s < CAMSS_NSRC; s++)
		if (r->sel[s] == (int)sel)
			break;
	if (s == CAMSS_NSRC || src_hz[s] == 0)
		return (0);
	hz = src_hz[s];
	div = cfg & ((1u << r->hid) - 1);
	if (div != 0)
		hz = hz * 2 / (div + 1);
	if (r->mnd != 0 && (cfg & RCG_CFG_MODE_MASK) != 0) {
		mask = (1u << r->mnd) - 1;
		m &= mask;
		/* N register = ~(n - m) */
		n = (~n & mask) + m;
		if (n != 0)
			hz = hz * m / n;
	}
	return (hz);
}

#ifdef _KERNEL
/* --- the controller ------------------------------------------------------------- */

static struct {
	struct sx	lock;
	int		state;		/* 0 not yet, 1 mapped, errno */
	phandle_t	node;
	vm_paddr_t	pa;
	vm_size_t	size;
	volatile uint32_t *va;
	u_int		ref[CAMSS_NBRANCH];
	uint32_t	rcg_hz[CAMSS_NRCG];	/* set by us, 0 not */
	u_int		rcg_users[CAMSS_NRCG];
	u_int		gdsc_ref;
} gcc;
SX_SYSINIT(camss_gcc_lock, &gcc.lock, "camss_gcc");

static uint32_t
R(uint32_t off)
{
	return (gcc.va[off / 4]);
}

static void
W(uint32_t off, uint32_t v)
{
	gcc.va[off / 4] = v;
	dsb(sy);
}

int
camss_gcc_attach(void)
{
	bus_addr_t pa;
	bus_size_t size;
	phandle_t node;

	sx_xlock(&gcc.lock);
	if (gcc.state != 0) {
		sx_xunlock(&gcc.lock);
		return (gcc.state == 1 ? 0 : gcc.state);
	}
	node = ofw_bus_find_compatible(OF_finddevice("/"), "qcom,gcc-sm6115");
	if (node <= 0 || ofw_reg_to_paddr(node, 0, &pa, &size, NULL) != 0) {
		printf("camss_gcc: no qcom,gcc-sm6115 node\n");
		gcc.state = ENXIO;
		sx_xunlock(&gcc.lock);
		return (ENXIO);
	}
	gcc.node = node;
	gcc.pa = pa;
	gcc.size = size;
	gcc.va = pmap_mapdev(pa, size);
	gcc.state = 1;
	sx_xunlock(&gcc.lock);
	return (0);
}

/* the rate of a source as the hardware is set: L * 19.2 MHz / post divider */
static uint64_t
src_hz(u_int src)
{
	const struct camss_pll *p;
	uint32_t mode, l, user, field;
	uint64_t hz;
	u_int i, j;
	int pll;

	if ((pll = camss_src_pll[src]) < 0)
		return (XO_HZ);
	p = &camss_plls[pll];
	mode = R(p->off + PLL_MODE);
	if ((mode & (PLL_ACTIVE_FLAG | PLL_LOCK_DET)) == 0)
		return (0);
	l = R(p->off + PLL_L_VAL) & 0xffff;
	hz = l * XO_HZ;
	/* a postdiv output: the divider its field selects */
	for (i = 0; i < nitems(camss_postdivs); i++) {
		if (camss_postdivs[i].src != (int)src)
			continue;
		user = R(camss_postdivs[i].off + PLL_USER_CTL);
		field = (user & PLL_POST_DIV_MASK) >> PLL_POST_DIV_SHIFT;
		for (j = 0; j < camss_postdivs[i].nent; j++)
			if (camss_postdivs[i].val[j] == field)
				return (hz / camss_postdivs[i].div[j]);
		return (0);	/* a divider Linux does not know */
	}
	return (hz);
}

static int
pll_running(const struct camss_pll *p)
{
	uint32_t mode = R(p->off + PLL_MODE);

	if (mode & PLL_ACTIVE_FLAG)
		return (1);
	return ((mode & (PLL_OUTCTRL | PLL_RESET_N | PLL_BYPASSNL)) ==
	    (PLL_OUTCTRL | PLL_RESET_N | PLL_BYPASSNL) &&
	    (mode & PLL_LOCK_DET) != 0);
}

/*
 * The PLL a source needs, on: clk_alpha_pll_enable(). Only a PLL Linux
 * configured (L value set, gcc_sm6115_probe does that for GPLL8-11); the
 * configuration itself is not redone here.
 */
static int
pll_on(int pll, const char *who)
{
	const struct camss_pll *p = &camss_plls[pll];
	uint32_t mode, l;
	int i;

	if (pll_running(p))
		return (0);
	mode = R(p->off + PLL_MODE);
	l = R(p->off + PLL_L_VAL) & 0xffff;
	printf("camss_gcc: %s: %s off (mode %#x, L %#x)", who, p->name, mode, l);
	if (l == 0) {
		printf(", not configured: not started\n");
		return (ENXIO);
	}
	if (mode & PLL_VOTE_FSM_ENA) {
		W(p->vote_reg, R(p->vote_reg) | p->vote_mask);
		for (i = 0; i < 1000 && (R(p->off + PLL_MODE) &
		    PLL_ACTIVE_FLAG) == 0; i++)
			DELAY(1);
		printf(", voted on: mode %#x\n", R(p->off + PLL_MODE));
	} else {
		W(p->off + PLL_MODE, R(p->off + PLL_MODE) | PLL_BYPASSNL);
		DELAY(5);
		W(p->off + PLL_MODE, R(p->off + PLL_MODE) | PLL_RESET_N);
		for (i = 0; i < 1000 && (R(p->off + PLL_MODE) &
		    PLL_LOCK_DET) == 0; i++)
			DELAY(1);
		W(p->off + PLL_MODE, R(p->off + PLL_MODE) | PLL_OUTCTRL);
		printf(", started: mode %#x\n", R(p->off + PLL_MODE));
	}
	return (pll_running(p) ? 0 : ETIMEDOUT);
}

static uint32_t
rcg_cur_hz(u_int rcg)
{
	const struct camss_rcg *r = &camss_rcgs[rcg];
	uint64_t s[CAMSS_NSRC];
	u_int i;

	for (i = 0; i < CAMSS_NSRC; i++)
		s[i] = src_hz(i);
	return ((uint32_t)camss_rcg_decode(rcg, R(r->cmd + RCG_CFG),
	    R(r->cmd + RCG_M), R(r->cmd + RCG_N), s));
}

/* clk_rcg2_shared_enable() with the entry for hz */
static int
rcg_set(u_int rcg, uint32_t hz, const char *who)
{
	const struct camss_rcg *r = &camss_rcgs[rcg];
	const struct camss_freq *f;
	struct camss_rcg_words w;
	int error, i, pll;

	if ((error = camss_rcg_encode(rcg, hz, &w)) != 0) {
		printf("camss_gcc: %s: %s has no %u Hz\n", who, r->name, hz);
		return (error);
	}
	f = rcg_entry(rcg, hz);
	if ((pll = camss_src_pll[f->src]) >= 0 && (error = pll_on(pll, who)))
		return (error);
	W(r->cmd + RCG_CMD, R(r->cmd + RCG_CMD) | RCG_CMD_ROOT_EN);
	for (i = 0; i < 500 && (R(r->cmd + RCG_CMD) & RCG_CMD_ROOT_OFF); i++)
		DELAY(1);
	if (w.mnd) {
		W(r->cmd + RCG_M, (R(r->cmd + RCG_M) & ~w.mnd_mask) | w.m);
		W(r->cmd + RCG_N, (R(r->cmd + RCG_N) & ~w.mnd_mask) | w.n);
		W(r->cmd + RCG_D, (R(r->cmd + RCG_D) & ~w.mnd_mask) | w.d);
	}
	W(r->cmd + RCG_CFG, (R(r->cmd + RCG_CFG) & ~w.cfg_mask) | w.cfg);
	W(r->cmd + RCG_CMD, R(r->cmd + RCG_CMD) | RCG_CMD_UPDATE);
	for (i = 0; i < 500 && (R(r->cmd + RCG_CMD) & RCG_CMD_UPDATE); i++)
		DELAY(1);
	error = (R(r->cmd + RCG_CMD) & RCG_CMD_UPDATE) ? ETIMEDOUT : 0;
	W(r->cmd + RCG_CMD, R(r->cmd + RCG_CMD) & ~RCG_CMD_ROOT_EN);
	printf("camss_gcc: %s: %s %u Hz from %s (CFG %#x)%s\n", who, r->name,
	    hz, camss_src_name[f->src], R(r->cmd + RCG_CFG),
	    error ? ": the update did not complete" : "");
	return (error);
}

static int
branch_critical(int b)
{
	/* CLK_IS_CRITICAL in gcc-sm6115.c */
	return (b == CAMSS_CLK_CAMERA_AHB_CLK || b == CAMSS_CLK_CAMERA_XO_CLK);
}

int
camss_clk_on(int b, uint32_t hz, const char *who)
{
	const struct camss_branch *c;
	uint32_t v;
	int error = 0, i;

	if (b < 0 || b >= CAMSS_NBRANCH || gcc.state != 1)
		return (EINVAL);
	c = &camss_branches[b];
	sx_xlock(&gcc.lock);
	if (gcc.ref[b]++ > 0) {
		if (hz != 0 && c->rcg >= 0 && gcc.rcg_hz[c->rcg] != hz)
			printf("camss_gcc: %s: %s already at %u Hz, not %u\n",
			    who, c->name, gcc.rcg_hz[c->rcg], hz);
		goto out;
	}
	if (c->rcg >= 0 && hz != 0) {
		if (gcc.rcg_users[c->rcg] > 0 && gcc.rcg_hz[c->rcg] != hz) {
			printf("camss_gcc: %s: %s's %s runs at %u Hz for "
			    "another clock, not %u\n", who, c->name,
			    camss_rcgs[c->rcg].name, gcc.rcg_hz[c->rcg], hz);
			error = EBUSY;
		} else if (gcc.rcg_users[c->rcg] == 0 &&
		    (error = rcg_set(c->rcg, hz, who)) == 0)
			gcc.rcg_hz[c->rcg] = hz;
		if (error != 0) {
			gcc.ref[b]--;
			goto out;
		}
	}
	if (c->rcg >= 0)
		gcc.rcg_users[c->rcg]++;
	v = R(c->halt);
	W(c->en, R(c->en) | c->mask);
	if (c->check == CAMSS_HALT_DELAY)
		DELAY(10);
	else
		for (i = 0; i < 200 && (R(c->halt) & BRANCH_CLK_OFF); i++)
			DELAY(1);
	printf("camss_gcc: %s: %s on (halt %#x -> %#x)", who, c->name, v,
	    R(c->halt));
	if (c->rcg >= 0)
		printf(", %u Hz", rcg_cur_hz(c->rcg));
	printf("\n");
	if (c->check != CAMSS_HALT_DELAY && (R(c->halt) & BRANCH_CLK_OFF)) {
		/* it did not start: undone, so that the caller may retry */
		printf("camss_gcc: %s: %s does not run\n", who, c->name);
		if (!branch_critical(b))
			W(c->en, R(c->en) & ~c->mask);
		gcc.ref[b]--;
		if (c->rcg >= 0 && --gcc.rcg_users[c->rcg] == 0)
			gcc.rcg_hz[c->rcg] = 0;
		error = ETIMEDOUT;
	}
out:
	sx_xunlock(&gcc.lock);
	return (error);
}

void
camss_clk_off(int b, const char *who)
{
	const struct camss_branch *c;

	if (b < 0 || b >= CAMSS_NBRANCH || gcc.state != 1)
		return;
	c = &camss_branches[b];
	sx_xlock(&gcc.lock);
	if (gcc.ref[b] == 0 || --gcc.ref[b] > 0) {
		sx_xunlock(&gcc.lock);
		return;
	}
	if (c->rcg >= 0 && --gcc.rcg_users[c->rcg] == 0)
		gcc.rcg_hz[c->rcg] = 0;
	if (!branch_critical(b)) {
		W(c->en, R(c->en) & ~c->mask);
		printf("camss_gcc: %s: %s off\n", who, c->name);
	}
	sx_xunlock(&gcc.lock);
}

uint64_t
camss_clk_hz(int b)
{
	const struct camss_branch *c;

	if (b < 0 || b >= CAMSS_NBRANCH || gcc.state != 1)
		return (0);
	c = &camss_branches[b];
	if (R(c->halt) & BRANCH_CLK_OFF)
		return (0);
	return (c->rcg >= 0 ? rcg_cur_hz(c->rcg) : XO_HZ);
}

/* can rcg run at hz with what is running now (its source's PLL)? */
int
camss_clk_reparent_ok(int rcg, uint32_t hz)
{
	const struct camss_freq *f;
	int pll;

	if (rcg < 0 || rcg >= CAMSS_NRCG || (f = rcg_entry(rcg, hz)) == NULL)
		return (0);
	pll = camss_src_pll[f->src];
	return (pll < 0 || pll_running(&camss_plls[pll]) ||
	    (R(camss_plls[pll].off + PLL_L_VAL) & 0xffff) != 0);
}

int
camss_clk_by_dt(phandle_t xref, uint32_t dt)
{
	u_int i;

	if (gcc.state != 1 || OF_node_from_xref(xref) != gcc.node)
		return (-1);
	for (i = 0; i < CAMSS_NBRANCH; i++)
		if (camss_branches[i].dt == (int)dt)
			return (i);
	return (-1);
}

/*
 * A node's "clocks" on, at its "assigned-clock-rates" where it has one
 * (the RCG behind an assigned clock): the branch ids into ids.
 */
int
camss_dt_clocks_on(phandle_t node, const char *who, int *ids, int max)
{
	pcell_t *clk = NULL, *ac = NULL, *ar = NULL;
	ssize_t nclk, nac, nar;
	int b, error = 0, i, j, n = 0;
	uint32_t hz;

	nclk = OF_getencprop_alloc_multi(node, "clocks", sizeof(pcell_t),
	    (void **)&clk);
	nac = OF_getencprop_alloc_multi(node, "assigned-clocks",
	    sizeof(pcell_t), (void **)&ac);
	nar = OF_getencprop_alloc_multi(node, "assigned-clock-rates",
	    sizeof(pcell_t), (void **)&ar);
	for (i = 0; i + 1 < nclk && n < max; i += 2) {
		if ((b = camss_clk_by_dt(clk[i], clk[i + 1])) < 0) {
			printf("camss_gcc: %s: clock %u is not a GCC camera "
			    "clock\n", who, clk[i + 1]);
			error = ENXIO;
			break;
		}
		hz = 0;
		for (j = 0; j + 1 < nac && nar > 0; j += 2)
			if (ac[j + 1] == clk[i + 1] && j / 2 < nar)
				hz = ar[j / 2];
		error = camss_clk_on(b, hz, who);
		/* a source that does not come up: the crystal, if the table
		 * has it (the clock then runs slower, the caller is told) */
		if (error != 0 && hz != 0 && hz != XO_HZ &&
		    camss_branches[b].rcg >= 0 &&
		    rcg_entry(camss_branches[b].rcg, XO_HZ) != NULL) {
			printf("camss_gcc: %s: %s at %u Hz failed (%d), at "
			    "%ju Hz instead\n", who, camss_branches[b].name, hz,
			    error, (uintmax_t)XO_HZ);
			error = camss_clk_on(b, XO_HZ, who);
		}
		if (error != 0)
			break;
		ids[n++] = b;
	}
	OF_prop_free(clk);
	OF_prop_free(ac);
	OF_prop_free(ar);
	if (error != 0) {
		camss_dt_clocks_off(ids, n, who);
		return (-error);
	}
	return (n);
}

void
camss_dt_clocks_off(const int *ids, int n, const char *who)
{
	while (n-- > 0)
		camss_clk_off(ids[n], who);
}

/* --- power domains -------------------------------------------------------------- */

static int
gdsc_wait_on(uint32_t reg)
{
	int i;

	for (i = 0; i < 2000 && (R(reg) & GDSC_PWR_ON) == 0; i++)
		DELAY(1);
	return ((R(reg) & GDSC_PWR_ON) ? 0 : ETIMEDOUT);
}

/* gdsc_init()'s wait times, then gdsc_enable() */
int
camss_gdsc_get(uint32_t reg, const char *who)
{
	uint32_t v;
	int error = 0;

	if (gcc.state != 1)
		return (ENXIO);
	sx_xlock(&gcc.lock);
	if (gcc.gdsc_ref++ > 0)
		goto out;
	v = R(reg);
	if ((v & GDSC_PWR_ON) && (v & GDSC_SW_COLLAPSE) == 0) {
		printf("camss_gcc: %s: GDSC %#x already on (%#x)\n", who, reg, v);
		goto out;
	}
	W(reg, (v & ~(GDSC_HW_CONTROL | GDSC_SW_OVERRIDE |
	    GDSC_EN_REST_WAIT_MASK | GDSC_EN_FEW_WAIT_MASK |
	    GDSC_CLK_DIS_WAIT_MASK)) | GDSC_WAITS);
	W(reg, R(reg) & ~GDSC_SW_COLLAPSE);
	error = gdsc_wait_on(reg);
	DELAY(1);
	printf("camss_gcc: %s: GDSC %#x: %#x -> %#x%s\n", who, reg, v, R(reg),
	    error ? " (PWR_ON did not come)" : "");
	if (error)
		gcc.gdsc_ref--;
out:
	sx_xunlock(&gcc.lock);
	return (error);
}

void
camss_gdsc_put(uint32_t reg, const char *who)
{
	if (gcc.state != 1)
		return;
	sx_xlock(&gcc.lock);
	if (gcc.gdsc_ref > 0 && --gcc.gdsc_ref == 0) {
		W(reg, R(reg) | GDSC_SW_COLLAPSE);
		printf("camss_gcc: %s: GDSC %#x off (%#x)\n", who, reg, R(reg));
	}
	sx_xunlock(&gcc.lock);
}

/* a VOTABLE GDSC (the SMMU TBUs'): our vote on, never taken back */
int
camss_gdsc_vote(uint32_t reg, const char *who)
{
	uint32_t v;
	int error = 0;

	if (gcc.state != 1)
		return (ENXIO);
	sx_xlock(&gcc.lock);
	v = R(reg);
	if (v & GDSC_SW_COLLAPSE) {
		W(reg, v & ~GDSC_SW_COLLAPSE);
		error = gdsc_wait_on(reg);
	}
	printf("camss_gcc: %s: GDSC %#x (vote): %#x -> %#x\n", who, reg, v,
	    R(reg));
	sx_xunlock(&gcc.lock);
	return (error);
}

void
camss_gcc_report(struct sbuf *sb)
{
	const struct camss_branch *c;
	u_int i;

	if (gcc.state != 1) {
		sbuf_printf(sb, "GCC not mapped (%d)", gcc.state);
		return;
	}
	sbuf_printf(sb, "GDSC camss_top %#x, tbu_rt %#x, tbu_nrt %#x\n",
	    R(CAMSS_GDSC_CAMSS_TOP), R(CAMSS_GDSC_TBU_RT),
	    R(CAMSS_GDSC_TBU_NRT));
	for (i = 0; i < CAMSS_NPLL; i++)
		sbuf_printf(sb, "%s mode %#x L %#x user %#x\n",
		    camss_plls[i].name, R(camss_plls[i].off + PLL_MODE),
		    R(camss_plls[i].off + PLL_L_VAL),
		    R(camss_plls[i].off + PLL_USER_CTL));
	for (i = 0; i < CAMSS_NBRANCH; i++) {
		c = &camss_branches[i];
		sbuf_printf(sb, "%s halt %#x ref %u", c->name, R(c->halt),
		    gcc.ref[i]);
		if (c->rcg >= 0)
			sbuf_printf(sb, " %s cmd %#x cfg %#x %u Hz",
			    camss_rcgs[c->rcg].name,
			    R(camss_rcgs[c->rcg].cmd + RCG_CMD),
			    R(camss_rcgs[c->rcg].cmd + RCG_CFG),
			    rcg_cur_hz(c->rcg));
		sbuf_printf(sb, "\n");
	}
}
#endif /* _KERNEL */
