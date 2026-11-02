/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The FreeBSD half of msm.ko (msm_freebsd.h, freebsd/gpu/msm-port.md): the
 * device tree, and what Linux's clock, power domain, SMMU and RPM drivers
 * do for the SM6115's Adreno 610, which FreeBSD has no drivers for:
 *
 *  - the GPU clock controller (gpucc@5990000) and GCC's GPU branches, as
 *    Linux v6.12's gpucc-sm6115.c, clk-alpha-pll.c, clk-rcg2.c and
 *    clk-branch.c program them: the two alpha PLLs (configured as at
 *    gpucc's probe, their rate set for the gfx3d frequency table), the
 *    gfx3d RCG (source and divider, the new PLL on while it switches), the
 *    branches with their halt checks;
 *  - the GPU's GDSCs, CX then GX, as gdsc.c's gdsc_init/enable/disable
 *    (GX: block reset and IO clamp; both votable);
 *  - the GPU's SMMU (adreno_smmu@59a0000, an MMU-500 the application
 *    processor owns, in the GPU's CX power domain): after each CX power up,
 *    with its clocks on, every client transaction bypasses it
 *    (sCR0.CLIENTPD), or with hw.msm.smmu=2 a stream match entry with S2CR
 *    bypass for the GPU node's "iommus" stream. msm.ko hands the GPU
 *    physical addresses (its VRAM carveout); Linux's page tables from
 *    before the kexec are gone;
 *  - the VDDCX level the GPU's OPPs require, a vote at the RPM (rpmpd's
 *    "rwcx" level) through qcom_dsp, only with hw.msm.cx_vote=1;
 *  - the speed bin from the QFPROM (nvmem cell), VRAM pages, memory for
 *    TrustZone, qcom_dsp's secure monitor calls.
 *
 * Tunables and sysctls (hw.msm.*):
 *   max_freq     OPPs above it are left out (default 320000000: the lowest
 *                OPP, LOW_SVS, until the CX vote is known to work; 0: all)
 *   cx_vote      1: vote VDDCX at the RPM for the OPP (default 0: the level
 *                Linux left voted stays; the display Linux set up needs at
 *                least LOW_SVS of it)
 *   cx_floor     the least level voted with cx_vote=1 (default 416,
 *                TURBO_NO_CPR: FreeBSD's vote replaces all of Linux's for
 *                the application processor and must not lower it)
 *   smmu         1 CLIENTPD bypass (default), 2 stream bypass, 0 untouched
 *   speedbin     -1 from the QFPROM (default), else this fuse value
 *   autosuspend  1: runtime suspend of the GPU after its delay (Linux's);
 *                default 0: powered from the first open until unload
 *   unprivileged_submit  1: GPU submits from processes without PRIV_IO
 *                (default 0: root only, as Linux's msm without IOMMU asks
 *                CAP_SYS_RAWIO; with the SMMU in bypass a submitting
 *                process can make the GPU read and write all memory)
 *   selftest     1 (loader): power the GPU up at load and check that its
 *                registers answer; writing 1 runs it again
 *   vram         (msm's module parameter, a string) the VRAM carveout's
 *                size, default "16m"; 256m with KMS (a few screens)
 *   kms          1: KMS for the display Linux left running
 *                (msm_freebsd_kms.c, freebsd/gpu/kms.md); the VRAM carveout
 *                then below 4 GiB, for the display's 32-bit addresses.
 *                Default 0: the device stays headless (GPU only)
 *   kms_irq      1 (default): vblank from INTF_1's vsync interrupt through
 *                qcom_dpu's MDSS interrupt when the kernel has it
 *                (patches/0053, dev.qcom_dpu.0.irq); 0: polled
 *   kms_info, kms_vblanks, kms_flips, kms_flips_late, kms_underruns
 *                (read only) KMS's set-up, its vblanks, page flips done,
 *                flips a frame late, INTF_1 underruns
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/eventhandler.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/proc.h>
#include <sys/sx.h>
#include <sys/sysctl.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <vm/vm_page.h>

#include <machine/bus.h>
#include <machine/cpufunc.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_subr.h>

#include "qcom_dsp.h"
#include "msm_freebsd.h"

MODULE_VERSION(msm, 1);
MODULE_DEPEND(msm, drmn, 2, 2, 2);
MODULE_DEPEND(msm, dmabuf, 1, 1, 1);
MODULE_DEPEND(msm, linuxkpi, 1, 1, 1);
MODULE_DEPEND(msm, firmware, 1, 1, 1);
MODULE_DEPEND(msm, lindebugfs, 1, 1, 1);

SYSCTL_NODE(_hw, OID_AUTO, msm, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "msm: the Adreno GPU (freebsd/gpu/msm-port.md)");

static u_long msm_max_freq = 320000000;
SYSCTL_ULONG(_hw_msm, OID_AUTO, max_freq, CTLFLAG_RDTUN, &msm_max_freq, 0,
    "Highest GPU OPP used, Hz (0: all)");
static int msm_cx_vote;
SYSCTL_INT(_hw_msm, OID_AUTO, cx_vote, CTLFLAG_RWTUN, &msm_cx_vote, 0,
    "Vote VDDCX at the RPM for the GPU's OPP");
static u_int msm_cx_floor = 416;
SYSCTL_UINT(_hw_msm, OID_AUTO, cx_floor, CTLFLAG_RWTUN, &msm_cx_floor, 0,
    "Least VDDCX level voted (RPM_SMD_LEVEL_*)");
static int msm_smmu_mode = 1;
SYSCTL_INT(_hw_msm, OID_AUTO, smmu, CTLFLAG_RWTUN, &msm_smmu_mode, 0,
    "GPU SMMU after CX power-up: 1 CLIENTPD bypass, 2 stream bypass, 0 none");
static int msm_speedbin = -1;
SYSCTL_INT(_hw_msm, OID_AUTO, speedbin, CTLFLAG_RDTUN, &msm_speedbin, 0,
    "GPU speed bin fuse (-1: read the QFPROM)");
static int msm_autosuspend;
SYSCTL_INT(_hw_msm, OID_AUTO, autosuspend, CTLFLAG_RDTUN, &msm_autosuspend, 0,
    "Runtime suspend of the GPU after its delay (0: kept on)");
static int msm_unprivileged_submit;
SYSCTL_INT(_hw_msm, OID_AUTO, unprivileged_submit, CTLFLAG_RWTUN,
    &msm_unprivileged_submit, 0,
    "GPU submits from processes without PRIV_IO (the GPU's SMMU is in "
    "bypass: such a process can reach all memory through the GPU)");
static u_int msm_cx_voted;
SYSCTL_UINT(_hw_msm, OID_AUTO, cx_voted, CTLFLAG_RD, &msm_cx_voted, 0,
    "VDDCX level last voted at the RPM (0: none)");
static u_int msm_cx_wanted;
SYSCTL_UINT(_hw_msm, OID_AUTO, cx_wanted, CTLFLAG_RD, &msm_cx_wanted, 0,
    "VDDCX level the GPU's OPP requires");
static int msm_kms;
SYSCTL_INT(_hw_msm, OID_AUTO, kms, CTLFLAG_RDTUN, &msm_kms, 0,
    "KMS for the display Linux left running (freebsd/gpu/kms.md)");
static int msm_kms_irq = 1;
SYSCTL_INT(_hw_msm, OID_AUTO, kms_irq, CTLFLAG_RDTUN, &msm_kms_irq, 0,
    "KMS vblank from qcom_dpu's MDSS interrupt when there (0: polled)");

unsigned long
msm_bsd_max_freq(void)
{
	return (msm_max_freq);
}

bool
msm_bsd_autosuspend(void)
{
	return (msm_autosuspend != 0);
}

bool
msm_bsd_unprivileged_submit(void)
{
	return (msm_unprivileged_submit != 0);
}

/* --- string module parameters (lkpi/linux/moduleparam.h) ------------------------- */

static MALLOC_DEFINE(M_MSM_PARAM, "msm_param", "msm.ko string parameters");

int lkpi_msm_charp_sysctl(SYSCTL_HANDLER_ARGS);

int
lkpi_msm_charp_sysctl(SYSCTL_HANDLER_ARGS)
{
	char **var = arg1;
	char buf[64], *n;
	int error;

	strlcpy(buf, *var != NULL ? *var : "", sizeof(buf));
	error = sysctl_handle_string(oidp, buf, sizeof(buf), req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	/* the old string may be the driver's literal: not freed */
	n = malloc(strlen(buf) + 1, M_MSM_PARAM, M_WAITOK);
	strcpy(n, buf);
	*var = n;
	return (0);
}

/* --- the device tree -------------------------------------------------------------- */

uintptr_t
msm_bsd_of_parse_phandle(uintptr_t node, const char *prop, int index)
{
	pcell_t *cells;
	phandle_t xref;
	ssize_t n;

	if (node == 0)
		return (0);
	n = OF_getencprop_alloc_multi((phandle_t)node, prop, sizeof(*cells),
	    (void **)&cells);
	if (n <= 0)
		return (0);
	xref = index < n ? cells[index] : 0;
	OF_prop_free(cells);
	if (xref == 0)
		return (0);
	return ((uintptr_t)OF_node_from_xref(xref));
}

int
msm_bsd_of_is_compatible(uintptr_t node, const char *compat)
{
	return (node != 0 && ofw_bus_node_is_compatible((phandle_t)node,
	    compat));
}

int
msm_bsd_of_is_available(uintptr_t node)
{
	return (node != 0 && ofw_bus_node_status_okay((phandle_t)node));
}

uintptr_t
msm_bsd_of_find_compatible(const char *compat)
{
	phandle_t n;

	n = ofw_bus_find_compatible(OF_finddevice("/"), compat);
	return (n > 0 ? (uintptr_t)n : 0);
}

int
msm_bsd_of_machine_is_compatible(const char *compat)
{
	return (ofw_bus_node_is_compatible(OF_finddevice("/"), compat));
}

int
msm_bsd_of_reg(uintptr_t node, int index, uint64_t *pa, uint64_t *size)
{
	bus_addr_t a;
	bus_size_t s;

	if (node == 0 || ofw_reg_to_paddr((phandle_t)node, index, &a, &s,
	    NULL) != 0)
		return (-EINVAL);
	*pa = a;
	*size = s;
	return (0);
}

int
msm_bsd_of_string_index(uintptr_t node, const char *prop, const char *name)
{
	int idx;

	if (node == 0 || ofw_bus_find_string_index((phandle_t)node, prop, name,
	    &idx) != 0)
		return (-ENOENT);
	return (idx);
}

int
msm_bsd_of_cells(uintptr_t node, const char *prop, uint32_t *buf, int max)
{
	ssize_t len;

	if (node == 0)
		return (-EINVAL);
	len = OF_getencprop((phandle_t)node, prop, buf, max * sizeof(*buf));
	if (len < 0)
		return (-ENOENT);
	return ((int)(len / sizeof(*buf)));
}

/* the newbus devices of msm.ko's platform drivers (lkpi_platform*.c) */
static const char *const msm_pf_names[] = { "adreno", "gmuwrap" };

void *
msm_bsd_find_pdev(uintptr_t node)
{
	devclass_t dc;
	device_t dev;
	u_int i;
	int u;

	for (i = 0; i < nitems(msm_pf_names); i++) {
		if ((dc = devclass_find(msm_pf_names[i])) == NULL)
			continue;
		for (u = 0; u < devclass_get_maxunit(dc); u++) {
			dev = devclass_get_device(dc, u);
			if (dev != NULL && device_is_attached(dev) &&
			    ofw_bus_get_node(dev) == (phandle_t)node)
				return (device_get_softc(dev));
		}
	}
	return (NULL);
}

/* --- register blocks ---------------------------------------------------------------- */

static struct sx msm_pwr_lock;
SX_SYSINIT(msm_pwr, &msm_pwr_lock, "msm power");

struct msm_regs {
	const char	*compat;
	vm_paddr_t	pa;
	vm_size_t	size;
	volatile uint8_t *va;
	phandle_t	node;
};
static struct msm_regs gpucc = { .compat = "qcom,sm6115-gpucc" };
static struct msm_regs gcc = { .compat = "qcom,gcc-sm6115" };
static struct msm_regs smmu;	/* the GPU node's iommus[0] */

static int
regs_map(struct msm_regs *r)
{
	bus_addr_t pa;
	bus_size_t size;

	if (r->va != NULL)
		return (0);
	if (r->node == 0) {
		r->node = ofw_bus_find_compatible(OF_finddevice("/"), r->compat);
		if (r->node <= 0) {
			r->node = 0;
			printf("msm: no %s in the device tree\n", r->compat);
			return (-ENXIO);
		}
	}
	if (ofw_reg_to_paddr(r->node, 0, &pa, &size, NULL) != 0)
		return (-ENXIO);
	r->pa = pa;
	r->size = size;
	r->va = pmap_mapdev(pa, size);
	return (0);
}

static void
regs_unmap(struct msm_regs *r)
{
	if (r->va != NULL)
		pmap_unmapdev((void *)(uintptr_t)r->va, r->size);
	r->va = NULL;
}

static uint32_t
rd(struct msm_regs *r, uint32_t off)
{
	return (*(volatile uint32_t *)(r->va + off));
}

static void
wr(struct msm_regs *r, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(r->va + off) = v;
	dsb(sy);
}

static void
rmw(struct msm_regs *r, uint32_t off, uint32_t mask, uint32_t val)
{
	wr(r, off, (rd(r, off) & ~mask) | (val & mask));
}

/* poll until (reg & mask) == want, about every microsecond: 0, -ETIMEDOUT */
static int
poll(struct msm_regs *r, uint32_t off, uint32_t mask, uint32_t want, int us)
{
	do {
		if ((rd(r, off) & mask) == want)
			return (0);
		DELAY(1);
	} while (us-- > 0);
	return ((rd(r, off) & mask) == want ? 0 : -ETIMEDOUT);
}

static int
regs_up(void)
{
	int error;

	if ((error = regs_map(&gpucc)) != 0 || (error = regs_map(&gcc)) != 0)
		return (error);
	return (0);
}

/* --- clocks: branches (clk-branch.c) ------------------------------------------------ */

#define	CBCR_CLK_OFF		(1U << 31)
#define	CBCR_NOC_FSM(v)		(((v) >> 28) & 0x7)
#define	CBCR_FSM_ON		0x2
#define	CBCR_FORCE_MEM_CORE	(1U << 14)
#define	CBCR_FORCE_PERIPH_ON	(1U << 13)
#define	CBCR_WAKEUP_MASK	(0xfU << 8)
#define	CBCR_SLEEP_MASK		(0xfU << 4)

enum halt { HALT, HALT_DELAY, HALT_SKIP, HALT_VOTED };

struct msm_branch {
	const char	*name;
	struct msm_regs	*cc;
	uint32_t	en_reg, en_bit;
	uint32_t	halt_reg;	/* 0: none */
	enum halt	halt;
	uint32_t	hwcg_reg;	/* 0: none; its bit 1 */
	int		count;
};

/* Linux v6.12's gpucc-sm6115.c and gcc-sm6115.c: registers, bits, halts */
static struct msm_branch msm_br[MSM_CLK_COUNT] = {
	[MSM_CLK_GX_GFX3D] = { "gpu_cc_gx_gfx3d_clk", &gpucc, 0x1054, 0,
	    0x1054, HALT_SKIP, 0 },
	[MSM_CLK_GPU_AHB] = { "gpu_cc_ahb_clk", &gpucc, 0x1078, 0, 0x1078,
	    HALT_DELAY, 0 },
	[MSM_CLK_CX_GMU] = { "gpu_cc_cx_gmu_clk", &gpucc, 0x1098, 0, 0x1098,
	    HALT, 0 },
	[MSM_CLK_CXO] = { "gpu_cc_cxo_clk", &gpucc, 0x109c, 0, 0x109c, HALT,
	    0 },
	[MSM_CLK_SMMU_VOTE] = { "gpu_cc_hlos1_vote_gpu_smmu_clk", &gpucc,
	    0x5000, 0, 0x5000, HALT_VOTED, 0 },
	[MSM_CLK_BIMC_GPU_AXI] = { "gcc_bimc_gpu_axi_clk", &gcc, 0x71154, 0,
	    0x71154, HALT_DELAY, 0x71154 },
	[MSM_CLK_GPU_MEMNOC_GFX] = { "gcc_gpu_memnoc_gfx_clk", &gcc, 0x3600c,
	    0, 0x3600c, HALT_VOTED, 0x3600c },
	[MSM_CLK_GPU_SNOC_DVM] = { "gcc_gpu_snoc_dvm_gfx_clk", &gcc, 0x36018,
	    0, 0x36018, HALT, 0 },
	[MSM_CLK_GPU_GPLL0] = { "gcc_gpu_gpll0_clk_src", &gcc, 0x79004, 15, 0,
	    HALT_DELAY, 0 },
	[MSM_CLK_GPU_GPLL0_DIV] = { "gcc_gpu_gpll0_div_clk_src", &gcc,
	    0x79004, 16, 0, HALT_DELAY, 0 },
};

static int
branch_wait(struct msm_branch *b, bool on)
{
	uint32_t v;
	int us;

	if (b->halt == HALT_SKIP || (b->hwcg_reg != 0 &&
	    (rd(b->cc, b->hwcg_reg) & (1U << 1)) != 0))
		return (0);
	if (b->halt == HALT_DELAY || (!on && b->halt == HALT_VOTED)) {
		DELAY(10);
		return (0);
	}
	for (us = 200; us-- > 0; DELAY(1)) {
		v = rd(b->cc, b->halt_reg);
		if (on && ((v & CBCR_CLK_OFF) == 0 ||
		    CBCR_NOC_FSM(v) == CBCR_FSM_ON))
			return (0);
		if (!on && (v & CBCR_CLK_OFF) != 0)
			return (0);
	}
	printf("msm: %s status stuck at '%s' (%#x)\n", b->name,
	    on ? "off" : "on", rd(b->cc, b->halt_reg));
	return (-EBUSY);
}

static int
branch_set(struct msm_branch *b, bool on)
{
	rmw(b->cc, b->en_reg, 1U << b->en_bit, on ? 1U << b->en_bit : 0);
	return (branch_wait(b, on));
}

/* --- clocks: the alpha PLLs (clk-alpha-pll.c, CLK_ALPHA_PLL_TYPE_DEFAULT) ----------- */

#define	PLL_MODE		0x00
#define	PLL_L_VAL		0x04
#define	PLL_ALPHA_VAL		0x08
#define	PLL_ALPHA_VAL_U		0x0c
#define	PLL_USER_CTL		0x10
#define	PLL_CONFIG_CTL		0x18
#define	PLL_OUTCTRL		(1U << 0)
#define	PLL_BYPASSNL		(1U << 1)
#define	PLL_RESET_N		(1U << 2)
#define	PLL_VOTE_FSM_ENA	(1U << 20)
#define	PLL_UPDATE		(1U << 22)
#define	PLL_UPDATE_BYPASS	(1U << 23)
#define	PLL_ACK_LATCH		(1U << 29)
#define	PLL_LOCK_DET		(1U << 31)
#define	PLL_ALPHA_EN		(1U << 24)
#define	PLL_VCO_SHIFT		20
#define	PLL_VCO_MASK		(0x3U << PLL_VCO_SHIFT)
#define	XO_HZ			19200000UL

struct msm_pll {
	const char	*name;
	uint32_t	off;
	/* gpu_cc_pll<n>_config */
	uint32_t	l, alpha, alpha_hi, vco_val, outputs, config_ctl;
	/* the VCO range of its vco_table (one entry each) */
	unsigned long	vco_min, vco_max;
	uint32_t	vco_sel;
	/* the post divider of the output gfx3d takes (div 1: field 0) */
	uint32_t	postdiv_shift, postdiv_width;
	int		count;
	unsigned long	rate;
};

static struct msm_pll msm_pll[2] = {
	/* "1200MHz configuration"; its AUX2 output feeds gfx3d */
	{ "gpu_cc_pll0", 0x0, 0x3e, 0, 0x80, 0x0U << 20, 0x7, 0x4001055b,
	    1000000000, 2000000000, 0, 8, 4, 0, 0 },
	/* "640MHz configuration"; its AUX output feeds gfx3d */
	{ "gpu_cc_pll1", 0x100, 0x21, 0x55555555, 0x55, 0x2U << 20, 0x3,
	    0x4001055b, 500000000, 1000000000, 2, 15, 3, 0, 0 },
};
static bool msm_gpucc_inited;

static int
pll_wait(struct msm_pll *p, uint32_t mask, bool clear, const char *what)
{
	uint32_t v;
	int n;

	for (n = 200; n > 0; n--) {
		v = rd(&gpucc, p->off + PLL_MODE);
		if (clear ? (v & mask) == 0 : (v & mask) == mask)
			return (0);
		DELAY(1);
	}
	printf("msm: %s failed to %s (mode %#x)\n", p->name, what,
	    rd(&gpucc, p->off + PLL_MODE));
	return (-ETIMEDOUT);
}

/* clk_alpha_pll_configure, as gpucc's probe calls it; a running PLL stays */
static void
pll_configure(struct msm_pll *p)
{
	uint32_t mode;

	mode = rd(&gpucc, p->off + PLL_MODE);
	if ((mode & PLL_OUTCTRL) != 0) {
		printf("msm: %s runs (mode %#x): left as it is\n", p->name, mode);
		return;
	}
	wr(&gpucc, p->off + PLL_L_VAL, p->l);
	wr(&gpucc, p->off + PLL_ALPHA_VAL, p->alpha);
	wr(&gpucc, p->off + PLL_CONFIG_CTL, p->config_ctl);
	wr(&gpucc, p->off + PLL_ALPHA_VAL_U, p->alpha_hi);
	/* outputs and VCO (alpha_en is in Linux's value, not in its mask) */
	rmw(&gpucc, p->off + PLL_USER_CTL, p->outputs | PLL_VCO_MASK,
	    p->outputs | p->vco_val);
	/* the post divider gfx3d takes: 1 */
	rmw(&gpucc, p->off + PLL_USER_CTL,
	    ((1U << p->postdiv_width) - 1) << p->postdiv_shift, 0);
}

static int
pll_latch(struct msm_pll *p)
{
	uint32_t mode;
	int error;

	mode = rd(&gpucc, p->off + PLL_MODE);
	rmw(&gpucc, p->off + PLL_MODE, PLL_UPDATE, PLL_UPDATE);
	DELAY(1);
	if (mode & PLL_UPDATE_BYPASS) {
		if ((error = pll_wait(p, PLL_ACK_LATCH, false,
		    "update_ack_set")) != 0)
			return (error);
		rmw(&gpucc, p->off + PLL_MODE, PLL_UPDATE, 0);
	} else if ((error = pll_wait(p, PLL_UPDATE, true, "update")) != 0)
		return (error);
	if ((error = pll_wait(p, PLL_ACK_LATCH, true, "update_ack_clear")) != 0)
		return (error);
	DELAY(10);
	return (0);
}

/* __clk_alpha_pll_set_rate: L, 40-bit alpha (32 fraction bits), VCO */
static int
pll_set_rate(struct msm_pll *p, unsigned long rate)
{
	uint64_t rem, a;
	uint32_t l;

	if (rate < p->vco_min || rate > p->vco_max) {
		printf("msm: %s: %lu Hz not in its VCO range\n", p->name, rate);
		return (-EINVAL);
	}
	if (rate == p->rate)
		return (0);
	l = rate / XO_HZ;
	rem = rate % XO_HZ;
	a = rem != 0 ? howmany(rem << 32, XO_HZ) : 0;
	a <<= 8;
	wr(&gpucc, p->off + PLL_L_VAL, l);
	wr(&gpucc, p->off + PLL_ALPHA_VAL_U, (uint32_t)(a >> 32));
	wr(&gpucc, p->off + PLL_ALPHA_VAL, (uint32_t)a);
	rmw(&gpucc, p->off + PLL_USER_CTL, PLL_VCO_MASK,
	    p->vco_sel << PLL_VCO_SHIFT);
	rmw(&gpucc, p->off + PLL_USER_CTL, PLL_ALPHA_EN, PLL_ALPHA_EN);
	p->rate = rate;
	/* SUPPORTS_DYNAMIC_UPDATE: a running PLL latches the new values */
	if (p->count > 0)
		return (pll_latch(p));
	return (0);
}

static int
pll_enable(struct msm_pll *p)
{
	uint32_t v, mask = PLL_OUTCTRL | PLL_RESET_N | PLL_BYPASSNL;
	int error;

	if (p->count > 0) {
		p->count++;
		return (0);
	}
	v = rd(&gpucc, p->off + PLL_MODE);
	if (v & PLL_VOTE_FSM_ENA) {
		printf("msm: %s in FSM mode (%#x): not ours\n", p->name, v);
		return (-EINVAL);
	}
	if ((v & mask) != mask) {
		rmw(&gpucc, p->off + PLL_MODE, PLL_BYPASSNL, PLL_BYPASSNL);
		/* 5 us between bypass off and reset deasserted */
		DELAY(5);
		rmw(&gpucc, p->off + PLL_MODE, PLL_RESET_N, PLL_RESET_N);
		if ((error = pll_wait(p, PLL_LOCK_DET, false, "enable")) != 0) {
			rmw(&gpucc, p->off + PLL_MODE,
			    PLL_RESET_N | PLL_BYPASSNL, 0);
			return (error);
		}
		rmw(&gpucc, p->off + PLL_MODE, PLL_OUTCTRL, PLL_OUTCTRL);
	}
	p->count = 1;
	return (0);
}

static void
pll_disable(struct msm_pll *p)
{
	if (p->count == 0 || --p->count > 0)
		return;
	rmw(&gpucc, p->off + PLL_MODE, PLL_OUTCTRL, 0);
	DELAY(1);
	rmw(&gpucc, p->off + PLL_MODE, PLL_RESET_N | PLL_BYPASSNL, 0);
}

/* --- clocks: the RCGs (clk-rcg2.c) --------------------------------------------------- */

#define	RCG_CMD			0x0
#define	RCG_CFG			0x4
#define	RCG_CMD_UPDATE		(1U << 0)
#define	RCG_CFG_DIV_MASK	0x1fU		/* hid_width 5 */
#define	RCG_CFG_SRC_SHIFT	8
#define	RCG_CFG_SRC_MASK	(0x7U << RCG_CFG_SRC_SHIFT)
#define	RCG_CFG_MODE_MASK	(0x3U << 12)
#define	RCG_CFG_HW_CTL		(1U << 20)

#define	GFX3D_RCG		0x101c	/* gpu_cc_gx_gfx3d_clk_src */
#define	GMU_RCG			0x1120	/* gpu_cc_gmu_clk_src */
/* gpu_cc_parent_map_1 (gfx3d): source selects */
#define	SRC1_PLL0_AUX2		2
#define	SRC1_PLL1_AUX		3
/* gpu_cc_parent_map_0 (gmu) */
#define	SRC0_XO			0
#define	SRC0_PLL0		1
#define	SRC0_PLL1		3
#define	SRC0_GPLL0		5
#define	SRC0_GPLL0_DIV		6

/* ftbl_gpu_cc_gx_gfx3d_clk_src: F(rate, source, 2, 0, 0) */
static const struct msm_gfx3d_freq {
	unsigned long	rate;
	int		pll;		/* msm_pll[] */
	uint32_t	src;
} msm_gfx3d_ftbl[] = {
	{ 320000000, 1, SRC1_PLL1_AUX },
	{ 465000000, 1, SRC1_PLL1_AUX },
	{ 600000000, 0, SRC1_PLL0_AUX2 },
	{ 745000000, 0, SRC1_PLL0_AUX2 },
	{ 820000000, 0, SRC1_PLL0_AUX2 },
	{ 900000000, 0, SRC1_PLL0_AUX2 },
	{ 950000000, 0, SRC1_PLL0_AUX2 },
	{ 980000000, 0, SRC1_PLL0_AUX2 },
};
#define	GFX3D_DIV		2

static const struct msm_gfx3d_freq *msm_gfx3d_cur;
static int msm_gmu_gpll0 = MSM_CLK_NONE;	/* the GMU RCG's GCC input */

static int
rcg_update(uint32_t rcg, const char *name)
{
	rmw(&gpucc, rcg + RCG_CMD, RCG_CMD_UPDATE, RCG_CMD_UPDATE);
	if (poll(&gpucc, rcg + RCG_CMD, RCG_CMD_UPDATE, 0, 500) != 0) {
		printf("msm: %s didn't update its configuration (cmd %#x)\n",
		    name, rd(&gpucc, rcg + RCG_CMD));
		return (-EBUSY);
	}
	return (0);
}

/*
 * clk_rcg2_set_rate with CLK_SET_RATE_PARENT and CLK_OPS_PARENT_ENABLE:
 * the PLL's rate, the PLL on while the RCG switches; while gfx3d is
 * enabled the new PLL takes over the old one's enable.
 */
static int
gfx3d_set_rate(unsigned long rate)
{
	const struct msm_gfx3d_freq *f = NULL;
	struct msm_pll *p;
	uint32_t cfg;
	u_int i;
	int error;

	/* qcom_find_freq: the first entry at or above the rate */
	for (i = 0; i < nitems(msm_gfx3d_ftbl); i++)
		if (msm_gfx3d_ftbl[i].rate >= rate) {
			f = &msm_gfx3d_ftbl[i];
			break;
		}
	if (f == NULL)
		f = &msm_gfx3d_ftbl[nitems(msm_gfx3d_ftbl) - 1];
	if (f == msm_gfx3d_cur)
		return (0);
	p = &msm_pll[f->pll];
	if ((error = pll_set_rate(p, f->rate * GFX3D_DIV)) != 0)
		return (error);
	if ((error = pll_enable(p)) != 0)
		return (error);
	cfg = rd(&gpucc, GFX3D_RCG + RCG_CFG);
	cfg &= ~(RCG_CFG_DIV_MASK | RCG_CFG_SRC_MASK | RCG_CFG_MODE_MASK |
	    RCG_CFG_HW_CTL);
	cfg |= (2 * GFX3D_DIV - 1) | f->src << RCG_CFG_SRC_SHIFT;
	wr(&gpucc, GFX3D_RCG + RCG_CFG, cfg);
	if ((error = rcg_update(GFX3D_RCG, "gpu_cc_gx_gfx3d_clk_src")) != 0) {
		pll_disable(p);
		return (error);
	}
	if (msm_br[MSM_CLK_GX_GFX3D].count > 0) {
		if (msm_gfx3d_cur != NULL)
			pll_disable(&msm_pll[msm_gfx3d_cur->pll]);
	} else
		pll_disable(p);
	msm_gfx3d_cur = f;
	return (0);
}

/*
 * The GMU RCG (clk_rcg2_shared_ops): Linux enables it with the
 * configuration it found, nothing in msm sets its rate for a GMU wrapper.
 * So here: its GCC input voted if that is GPLL0, parked on XO if it is one
 * of the GPU's PLLs (which may be off).
 */
static void
gmu_src_prepare(void)
{
	uint32_t src;

	src = (rd(&gpucc, GMU_RCG + RCG_CFG) & RCG_CFG_SRC_MASK) >>
	    RCG_CFG_SRC_SHIFT;
	msm_gmu_gpll0 = MSM_CLK_NONE;
	if (src == SRC0_PLL0 || src == SRC0_PLL1) {
		rmw(&gpucc, GMU_RCG + RCG_CFG, RCG_CFG_SRC_MASK |
		    RCG_CFG_DIV_MASK, SRC0_XO << RCG_CFG_SRC_SHIFT);
		rcg_update(GMU_RCG, "gpu_cc_gmu_clk_src");
	} else if (src == SRC0_GPLL0)
		msm_gmu_gpll0 = MSM_CLK_GPU_GPLL0;
	else if (src == SRC0_GPLL0_DIV)
		msm_gmu_gpll0 = MSM_CLK_GPU_GPLL0_DIV;
}

/* gpucc's probe: the PLLs' configuration, the CX GMU branch's wakeup and
 * sleep counts, gfx3d's memories forced on */
static void
gpucc_init(void)
{
	if (msm_gpucc_inited)
		return;
	pll_configure(&msm_pll[0]);
	pll_configure(&msm_pll[1]);
	rmw(&gpucc, 0x1098, CBCR_WAKEUP_MASK | CBCR_SLEEP_MASK,
	    0xfU << 8 | 0xfU << 4);
	rmw(&gpucc, 0x1054, CBCR_FORCE_MEM_CORE | CBCR_FORCE_PERIPH_ON,
	    CBCR_FORCE_MEM_CORE | CBCR_FORCE_PERIPH_ON);
	msm_gpucc_inited = true;
}

static int
clk_on_locked(int id)
{
	struct msm_branch *b = &msm_br[id];
	int error;

	if (b->count > 0) {
		b->count++;
		return (0);
	}
	if (id == MSM_CLK_GX_GFX3D) {
		gpucc_init();
		/* the lowest rate if nothing set one yet */
		if (msm_gfx3d_cur == NULL &&
		    (error = gfx3d_set_rate(msm_gfx3d_ftbl[0].rate)) != 0)
			return (error);
		if ((error = pll_enable(&msm_pll[msm_gfx3d_cur->pll])) != 0)
			return (error);
	}
	if (id == MSM_CLK_CX_GMU) {
		gmu_src_prepare();
		if (msm_gmu_gpll0 != MSM_CLK_NONE &&
		    (error = clk_on_locked(msm_gmu_gpll0)) != 0)
			return (error);
	}
	if ((error = branch_set(b, true)) != 0) {
		if (id == MSM_CLK_GX_GFX3D)
			pll_disable(&msm_pll[msm_gfx3d_cur->pll]);
		return (error);
	}
	b->count = 1;
	return (0);
}

static void clk_off_locked(int id);

static void
clk_off_locked(int id)
{
	struct msm_branch *b = &msm_br[id];

	if (b->count == 0 || --b->count > 0)
		return;
	branch_set(b, false);
	if (id == MSM_CLK_GX_GFX3D && msm_gfx3d_cur != NULL)
		pll_disable(&msm_pll[msm_gfx3d_cur->pll]);
	if (id == MSM_CLK_CX_GMU && msm_gmu_gpll0 != MSM_CLK_NONE)
		clk_off_locked(msm_gmu_gpll0);
}

/* --- clocks: the interface ------------------------------------------------------------ */

const char *
msm_bsd_clk_name(int id)
{
	if (id <= MSM_CLK_NONE || id >= MSM_CLK_COUNT)
		return ("(none)");
	return (msm_br[id].name);
}

/* dt-bindings/clock/qcom,sm6115-gpucc.h and qcom,gcc-sm6115.h */
static const struct {
	const char	*compat;
	uint32_t	index;
	int		id;
} msm_clk_map[] = {
	{ "qcom,sm6115-gpucc", 13, MSM_CLK_GX_GFX3D },
	{ "qcom,sm6115-gpucc", 4, MSM_CLK_GPU_AHB },
	{ "qcom,sm6115-gpucc", 7, MSM_CLK_CX_GMU },
	{ "qcom,sm6115-gpucc", 10, MSM_CLK_CXO },
	{ "qcom,sm6115-gpucc", 16, MSM_CLK_SMMU_VOTE },
	{ "qcom,gcc-sm6115", 150, MSM_CLK_BIMC_GPU_AXI },
	{ "qcom,gcc-sm6115", 76, MSM_CLK_GPU_MEMNOC_GFX },
	{ "qcom,gcc-sm6115", 77, MSM_CLK_GPU_SNOC_DVM },
	{ "qcom,gcc-sm6115", 73, MSM_CLK_GPU_GPLL0 },
	{ "qcom,gcc-sm6115", 74, MSM_CLK_GPU_GPLL0_DIV },
};

int
msm_bsd_clk_of(uintptr_t node, int index)
{
	pcell_t cells[2 * 16];
	phandle_t prov;
	ssize_t n;
	u_int i;

	n = OF_getencprop((phandle_t)node, "clocks", cells, sizeof(cells));
	if (n < 0 || (index + 1) * 2 * (ssize_t)sizeof(pcell_t) > n)
		return (-ENOENT);
	prov = OF_node_from_xref(cells[2 * index]);
	for (i = 0; i < nitems(msm_clk_map); i++)
		if (cells[2 * index + 1] == msm_clk_map[i].index &&
		    ofw_bus_node_is_compatible(prov, msm_clk_map[i].compat))
			return (msm_clk_map[i].id);
	return (MSM_CLK_NONE);
}

int
msm_bsd_clk_enable(int id)
{
	int error;

	if (id <= MSM_CLK_NONE || id >= MSM_CLK_COUNT)
		return (0);
	sx_xlock(&msm_pwr_lock);
	if ((error = regs_up()) == 0)
		error = clk_on_locked(id);
	sx_xunlock(&msm_pwr_lock);
	return (error);
}

void
msm_bsd_clk_disable(int id)
{
	if (id <= MSM_CLK_NONE || id >= MSM_CLK_COUNT)
		return;
	sx_xlock(&msm_pwr_lock);
	if (gpucc.va != NULL)
		clk_off_locked(id);
	sx_xunlock(&msm_pwr_lock);
}

int
msm_bsd_clk_set_rate(int id, unsigned long rate)
{
	int error;

	if (id != MSM_CLK_GX_GFX3D)
		return (0);	/* gfx3d's is the only rate msm sets here */
	sx_xlock(&msm_pwr_lock);
	if ((error = regs_up()) == 0) {
		gpucc_init();
		error = gfx3d_set_rate(rate);
	}
	sx_xunlock(&msm_pwr_lock);
	return (error);
}

unsigned long
msm_bsd_clk_get_rate(int id)
{
	switch (id) {
	case MSM_CLK_GX_GFX3D:
		return (msm_gfx3d_cur != NULL ? msm_gfx3d_cur->rate : 0);
	case MSM_CLK_CXO:
		return (XO_HZ);
	default:
		return (0);
	}
}

long
msm_bsd_clk_round_rate(int id, unsigned long rate)
{
	u_int i;

	if (id != MSM_CLK_GX_GFX3D)
		return (rate);
	for (i = 0; i < nitems(msm_gfx3d_ftbl); i++)
		if (msm_gfx3d_ftbl[i].rate >= rate)
			return (msm_gfx3d_ftbl[i].rate);
	return (msm_gfx3d_ftbl[nitems(msm_gfx3d_ftbl) - 1].rate);
}

/* --- the GPU's SMMU ------------------------------------------------------------------- */

#define	SMMU_SCR0		0x000
#define	SMMU_SCR0_CLIENTPD	(1U << 0)
#define	SMMU_IDR0		0x020
#define	SMMU_IDR1		0x024
#define	SMMU_TLBIALLNSNH	0x068
#define	SMMU_TLBGSYNC		0x070
#define	SMMU_TLBGSTATUS		0x074
#define	SMMU_SMR(n)		(0x800 + 4 * (n))
#define	SMMU_S2CR(n)		(0xc00 + 4 * (n))
#define	SMMU_SMR_VALID		(1U << 31)
#define	SMMU_S2CR_BYPASS	(1U << 16)
#define	SMMU_CB_SCTLR		0x000

static uint32_t msm_smmu_sid, msm_smmu_mask;
static bool msm_smmu_logged;

/* the GPU node's iommus[0]: <&adreno_smmu sid mask> */
static int
smmu_find(void)
{
	phandle_t gpu;
	pcell_t cells[3];

	if (smmu.node != 0)
		return (0);
	gpu = ofw_bus_find_compatible(OF_finddevice("/"), "qcom,adreno");
	if (gpu <= 0 || OF_getencprop(gpu, "iommus", cells, sizeof(cells)) <
	    (ssize_t)sizeof(cells)) {
		printf("msm: the GPU node has no iommus: SMMU left alone\n");
		return (-ENOENT);
	}
	smmu.node = OF_node_from_xref(cells[0]);
	msm_smmu_sid = cells[1];
	msm_smmu_mask = cells[2];
	return (0);
}

/*
 * After the CX GDSC came up (the SMMU's registers back from reset, or with
 * what Linux set before the kexec, whose page tables are gone): every
 * client transaction passes untranslated, or (mode 2) the GPU's stream is
 * matched to a bypass S2CR and no other match entry is valid.
 */
static void
smmu_bypass(void)
{
	uint32_t scr0, idr0, idr1, smr, s2cr;
	u_int i, nsmr, ncb, pgshift, numpage;
	vm_offset_t cbbase;

	if (msm_smmu_mode == 0 || smmu_find() != 0 || regs_map(&smmu) != 0)
		return;
	scr0 = rd(&smmu, SMMU_SCR0);
	idr0 = rd(&smmu, SMMU_IDR0);
	idr1 = rd(&smmu, SMMU_IDR1);
	nsmr = idr0 & 0xff;
	ncb = idr1 & 0xff;
	pgshift = (idr1 & (1U << 31)) ? 16 : 12;
	numpage = 1U << (((idr1 >> 28) & 0x7) + 1);
	if (!msm_smmu_logged) {
		printf("msm: GPU SMMU at %#jx: sCR0 %#x IDR0 %#x IDR1 %#x "
		    "(%u SMRs, %u context banks), the GPU's stream %#x/%#x\n",
		    (uintmax_t)smmu.pa, scr0, idr0, idr1, nsmr, ncb,
		    msm_smmu_sid, msm_smmu_mask);
		for (i = 0; i < nsmr && i < 128; i++) {
			smr = rd(&smmu, SMMU_SMR(i));
			s2cr = rd(&smmu, SMMU_S2CR(i));
			if (smr & SMMU_SMR_VALID)
				printf("msm:   SMR%u %#x S2CR %#x\n", i, smr,
				    s2cr);
		}
	}
	if (msm_smmu_mode == 2) {
		cbbase = (vm_offset_t)numpage << pgshift;
		for (i = 0; i < ncb && cbbase + ((vm_offset_t)(i + 1) <<
		    pgshift) <= smmu.size; i++)
			wr(&smmu, cbbase + ((vm_offset_t)i << pgshift) +
			    SMMU_CB_SCTLR, 0);
		for (i = 0; i < nsmr; i++)
			wr(&smmu, SMMU_SMR(i), 0);
		wr(&smmu, SMMU_S2CR(0), SMMU_S2CR_BYPASS);
		wr(&smmu, SMMU_SMR(0), SMMU_SMR_VALID |
		    (msm_smmu_mask & 0x7fff) << 16 | (msm_smmu_sid & 0x7fff));
		scr0 &= ~SMMU_SCR0_CLIENTPD;
	} else
		scr0 |= SMMU_SCR0_CLIENTPD;
	wr(&smmu, SMMU_TLBIALLNSNH, 0);
	wr(&smmu, SMMU_TLBGSYNC, 0);
	poll(&smmu, SMMU_TLBGSTATUS, 1, 0, 1000);
	wr(&smmu, SMMU_SCR0, scr0);
	if (!msm_smmu_logged)
		printf("msm: GPU SMMU: %s, sCR0 now %#x\n", msm_smmu_mode == 2 ?
		    "stream bypass" : "client bypass", rd(&smmu, SMMU_SCR0));
	msm_smmu_logged = true;
}

/* --- power domains: the GDSCs (gdsc.c) ---------------------------------------------- */

#define	GDSC_PWR_ON		(1U << 31)
#define	GDSC_SW_COLLAPSE	(1U << 0)
#define	GDSC_HW_CONTROL		(1U << 1)
#define	GDSC_SW_OVERRIDE	(1U << 2)
#define	GDSC_WAIT_MASK		(0xfffU << 12)
/* EN_REST_WAIT 2, EN_FEW_WAIT 8, CLK_DIS_WAIT 2 */
#define	GDSC_WAIT_VAL		(0x2U << 20 | 0x8U << 16 | 0x2U << 12)
#define	GDSC_CLAMP_IO		(1U << 0)

static struct msm_gdsc {
	const char	*name;
	uint32_t	gdscr, status, clamp, reset;	/* 0: none */
	int		count;
	bool		inited;
} msm_gdsc[2] = {
	/* gpu_cx_gdsc: VOTABLE, its status in gds_hw_ctrl */
	[MSM_PD_GPU_CX] = { "gpu_cx_gdsc", 0x106c, 0x1540, 0, 0 },
	/* gpu_gx_gdsc: CLAMP_IO | SW_RESET (GPU_GX_BCR) | VOTABLE */
	[MSM_PD_GPU_GX] = { "gpu_gx_gdsc", 0x100c, 0x100c, 0x1508, 0x1008 },
};

/* gdsc_init: HW trigger and SW override off, the wait times */
static void
gdsc_init(struct msm_gdsc *g)
{
	if (g->inited)
		return;
	rmw(&gpucc, g->gdscr, GDSC_HW_CONTROL | GDSC_SW_OVERRIDE |
	    GDSC_WAIT_MASK, GDSC_WAIT_VAL);
	g->inited = true;
}

static int
gdsc_on(struct msm_gdsc *g)
{
	int error;

	if (g->reset != 0) {			/* SW_RESET */
		rmw(&gpucc, g->reset, 1, 1);
		DELAY(1);
		rmw(&gpucc, g->reset, 1, 0);
	}
	if (g->clamp != 0)			/* CLAMP_IO */
		rmw(&gpucc, g->clamp, GDSC_CLAMP_IO, 0);
	rmw(&gpucc, g->gdscr, GDSC_SW_COLLAPSE, 0);
	/* the GDS controller updates its status after some XO cycles */
	DELAY(1);
	error = poll(&gpucc, g->status, GDSC_PWR_ON, GDSC_PWR_ON, 1500);
	if (error != 0)
		printf("msm: %s status stuck at 'off' (%#x)\n", g->name,
		    rd(&gpucc, g->status));
	DELAY(1);
	return (error);
}

static int
gdsc_off(struct msm_gdsc *g, bool synced)
{
	int error = 0;

	rmw(&gpucc, g->gdscr, GDSC_SW_COLLAPSE, GDSC_SW_COLLAPSE);
	/* votable: no status poll, unless the caller waits for it */
	if (!synced)
		DELAY(500);
	else if ((error = poll(&gpucc, g->status, GDSC_PWR_ON, 0, 1500)) != 0)
		printf("msm: %s status stuck at 'on' (%#x)\n", g->name,
		    rd(&gpucc, g->status));
	if (g->clamp != 0)
		rmw(&gpucc, g->clamp, GDSC_CLAMP_IO, GDSC_CLAMP_IO);
	return (error);
}

/* the GPU SMMU's clocks: "mem", "hlos", "iface" */
static const int msm_smmu_clks[] = { MSM_CLK_GPU_MEMNOC_GFX,
    MSM_CLK_SMMU_VOTE, MSM_CLK_GPU_SNOC_DVM };

static int
pd_set(int pd, bool on, bool synced)
{
	struct msm_gdsc *g = &msm_gdsc[pd];
	u_int i;
	int error;

	if (on) {
		if (g->count > 0) {
			g->count++;
			return (0);
		}
		/* GX's parent domain is CX (gpu_gx_gdsc.parent) */
		if (pd == MSM_PD_GPU_GX &&
		    (error = pd_set(MSM_PD_GPU_CX, true, false)) != 0)
			return (error);
		gdsc_init(g);
		if ((error = gdsc_on(g)) != 0) {
			if (pd == MSM_PD_GPU_GX)
				pd_set(MSM_PD_GPU_CX, false, false);
			return (error);
		}
		g->count = 1;
		if (pd == MSM_PD_GPU_CX) {
			for (i = 0; i < nitems(msm_smmu_clks); i++)
				clk_on_locked(msm_smmu_clks[i]);
			smmu_bypass();
		}
		return (0);
	}
	if (g->count == 0 || --g->count > 0)
		return (0);
	if (pd == MSM_PD_GPU_CX)
		for (i = nitems(msm_smmu_clks); i-- > 0; )
			clk_off_locked(msm_smmu_clks[i]);
	error = gdsc_off(g, synced);
	if (pd == MSM_PD_GPU_GX)
		pd_set(MSM_PD_GPU_CX, false, false);
	return (error);
}

int
msm_bsd_gdsc(int pd, bool on, bool synced)
{
	int error;

	if (pd != MSM_PD_GPU_CX && pd != MSM_PD_GPU_GX)
		return (-EINVAL);
	sx_xlock(&msm_pwr_lock);
	if ((error = regs_up()) == 0)
		error = pd_set(pd, on, synced);
	sx_xunlock(&msm_pwr_lock);
	return (error);
}

/* power-domains = <&gpucc GPU_CX_GDSC>, <&gpucc GPU_GX_GDSC>, by name */
int
msm_bsd_pd_of(uintptr_t node, const char *name)
{
	pcell_t cells[2 * 4];
	ssize_t n;
	int idx;

	if (ofw_bus_find_string_index((phandle_t)node, "power-domain-names",
	    name, &idx) != 0)
		return (-ENOENT);
	n = OF_getencprop((phandle_t)node, "power-domains", cells,
	    sizeof(cells));
	if (n < (idx + 1) * 2 * (ssize_t)sizeof(pcell_t))
		return (-ENOENT);
	if (!ofw_bus_node_is_compatible(OF_node_from_xref(cells[2 * idx]),
	    "qcom,sm6115-gpucc") || cells[2 * idx + 1] > MSM_PD_GPU_GX)
		return (-ENODEV);
	return ((int)cells[2 * idx + 1]);
}

/* --- the RPM's VDDCX (rpmpd "rwcx" 0) --------------------------------------------- */

#define	RPMPD_RWCX		0x78637772	/* "rwcx" */
#define	RPMPD_KEY_ENABLE	0x6e657773	/* "swen" */
#define	RPMPD_KEY_LEVEL		0x6c766c76	/* "vlvl" */

int
msm_bsd_opp_level(uintptr_t opp_node, uint32_t *level)
{
	pcell_t v;

	if (OF_getencprop((phandle_t)opp_node, "opp-level", &v, sizeof(v)) !=
	    sizeof(v))
		return (-ENOENT);
	*level = v;
	return (0);
}

int
msm_bsd_opp_table(uintptr_t node, struct msm_bsd_opp *opps, int max)
{
	phandle_t table, child;
	pcell_t hz[2], v, xref;
	ssize_t len;
	int n = 0;

	table = (phandle_t)msm_bsd_of_parse_phandle(node, "operating-points-v2",
	    0);
	if (table == 0)
		return (-ENODEV);
	for (child = OF_child(table); child > 0 && n < max;
	    child = OF_peer(child)) {
		if (!ofw_bus_node_status_okay(child))
			continue;
		len = OF_getencprop(child, "opp-hz", hz, sizeof(hz));
		if (len == sizeof(hz[0]))
			opps[n].hz = hz[0];
		else if (len == sizeof(hz))
			opps[n].hz = (uint64_t)hz[0] << 32 | hz[1];
		else
			continue;
		opps[n].level = 0;
		if (OF_getencprop(child, "required-opps", &xref,
		    sizeof(xref)) == sizeof(xref) &&
		    OF_getencprop(OF_node_from_xref(xref), "opp-level", &v,
		    sizeof(v)) == sizeof(v))
			opps[n].level = v;
		opps[n].supported_hw = 0;
		if (OF_getencprop(child, "opp-supported-hw", &v, sizeof(v)) ==
		    sizeof(v))
			opps[n].supported_hw = v;
		n++;
	}
	return (n);
}

int
msm_bsd_cx_level(unsigned int level)
{
	static bool told, enabled;
	const uint32_t on[2] = { RPMPD_KEY_ENABLE, 1 };
	uint32_t kv[2] = { RPMPD_KEY_LEVEL, 0 };
	unsigned int vote;
	int e1 = 0, e2, e3;

	msm_cx_wanted = level;
	if (!msm_cx_vote) {
		if (!told)
			printf("msm: VDDCX level %u wanted, not voted "
			    "(hw.msm.cx_vote=0): Linux's vote stays\n", level);
		told = true;
		return (0);
	}
	/* never below the floor: this vote replaces all of Linux's */
	vote = MAX(level, msm_cx_floor);
	if (vote == msm_cx_voted)
		return (0);
	if (!enabled)
		e1 = qcom_rpm_write(0, RPMPD_RWCX, 0, on, 1);
	kv[1] = vote;
	e2 = qcom_rpm_write(0, RPMPD_RWCX, 0, kv, 1);
	e3 = qcom_rpm_write(1, RPMPD_RWCX, 0, kv, 1);
	if (e1 == 0)
		enabled = true;
	if (e1 != 0 || e2 != 0 || e3 != 0) {
		printf("msm: VDDCX vote %u failed (%d %d %d)\n", vote, e1, e2,
		    e3);
		return (-EIO);
	}
	printf("msm: VDDCX voted at level %u (the GPU wants %u)\n", vote,
	    level);
	msm_cx_voted = vote;
	return (0);
}

/* --- the speed bin: an nvmem cell in the QFPROM ------------------------------------ */

int
msm_bsd_nvmem_cell_u32(uintptr_t node, const char *cell, uint32_t *val)
{
	pcell_t reg[2], bits[2];
	phandle_t cn, qfprom;
	bus_addr_t pa;
	bus_size_t size;
	volatile uint8_t *va;
	uint32_t v = 0;
	int idx, i;

	if (msm_speedbin >= 0 && strcmp(cell, "speed_bin") == 0) {
		*val = msm_speedbin;
		printf("msm: speed bin fuse %u (hw.msm.speedbin)\n", *val);
		return (0);
	}
	if (ofw_bus_find_string_index((phandle_t)node, "nvmem-cell-names", cell,
	    &idx) != 0)
		return (-ENOENT);
	cn = (phandle_t)msm_bsd_of_parse_phandle(node, "nvmem-cells", idx);
	if (cn == 0 || OF_getencprop(cn, "reg", reg, sizeof(reg)) !=
	    sizeof(reg) || reg[1] == 0 || reg[1] > 4)
		return (-EINVAL);
	/* the QFPROM's corrected region, read bytewise as Linux's qfprom */
	qfprom = OF_parent(cn);
	if (ofw_reg_to_paddr(qfprom, 0, &pa, &size, NULL) != 0 ||
	    reg[0] + reg[1] > size)
		return (-EINVAL);
	va = pmap_mapdev(pa, size);
	for (i = 0; i < (int)reg[1]; i++)
		v |= (uint32_t)va[reg[0] + i] << (8 * i);
	pmap_unmapdev((void *)(uintptr_t)va, size);
	if (OF_getencprop(cn, "bits", bits, sizeof(bits)) == sizeof(bits)) {
		v >>= bits[0];
		if (bits[1] < 32)
			v &= (1U << bits[1]) - 1;
	}
	*val = v;
	printf("msm: nvmem cell %s: %#x (QFPROM %#jx + %#x)\n", cell, v,
	    (uintmax_t)pa, reg[0]);
	return (0);
}

/* --- memory ------------------------------------------------------------------------- */

void *
msm_bsd_vram_alloc(size_t size, uint64_t *pa)
{
	vm_page_t m;
	u_long npages = atop(round_page(size));
	int tries;

	/* with KMS below 4 GiB: the display's source pipes take 32-bit
	 * addresses (its SMMU translates nothing, qcom_dpu) */
	vm_paddr_t high = msm_kms ? (vm_paddr_t)0xffffffff : ~(vm_paddr_t)0;

	for (tries = 0; tries < 3; tries++) {
		m = vm_page_alloc_noobj_contig(VM_ALLOC_WIRED | VM_ALLOC_ZERO |
		    VM_ALLOC_NORMAL, npages, 0, high, PAGE_SIZE, 0,
		    VM_MEMATTR_WRITE_COMBINING);
		if (m != NULL) {
			*pa = VM_PAGE_TO_PHYS(m);
			return (m);
		}
		if (vm_page_reclaim_contig(VM_ALLOC_NORMAL | VM_ALLOC_WAITOK,
		    npages, 0, high, PAGE_SIZE, 0) != 0)
			break;
	}
	printf("msm: no %zu bytes of contiguous memory for the VRAM%s\n", size,
	    msm_kms ? " below 4 GiB" : "");
	return (NULL);
}

void
msm_bsd_vram_free(uint64_t pa, size_t size)
{
	vm_page_t m = PHYS_TO_VM_PAGE(pa);
	u_long i, npages = atop(round_page(size));

	for (i = 0; i < npages; i++, m++) {
		if (m->object != NULL) {
			/* still in a user mapping's object: left to it */
			printf("msm: VRAM page %#jx still mapped, not freed\n",
			    (uintmax_t)VM_PAGE_TO_PHYS(m));
			continue;
		}
		pmap_page_set_memattr(m, VM_MEMATTR_DEFAULT);
		vm_page_unwire_noq(m);
		vm_page_free(m);
	}
}

void *
msm_bsd_contig_alloc(size_t size, uint64_t *pa)
{
	void *va;

	/* contiguous and uncached, as qcom_pas.c's metadata */
	va = kmem_alloc_contig(round_page(size), M_WAITOK | M_ZERO, 0,
	    ~(vm_paddr_t)0, PAGE_SIZE, 0, VM_MEMATTR_UNCACHEABLE);
	if (va != NULL)
		*pa = pmap_kextract((vm_offset_t)va);
	return (va);
}

void
msm_bsd_contig_free(void *va, size_t size)
{
	if (va != NULL)
		kmem_free(va, round_page(size));
}

void
msm_bsd_wb(void *va, size_t size)
{
	cpu_dcache_wb_range(va, size);
}

/* --- the display: KMS's FreeBSD side (msm_freebsd_kms.c, kms.md) ------------------- */

/*
 * qcom_dpu's eventhandler for the DPU's interrupts (patches/0053,
 * sys/arm64/qualcomm/qcom_dpu.h, declared again here: msm.ko is built
 * against kernel sources that may not have it yet, and registers by name)
 */
typedef void (*qcom_dpu_mdp_intr_fn)(void *arg, uint32_t intr, uint32_t intr2);
EVENTHANDLER_DECLARE(qcom_dpu_mdp_intr, qcom_dpu_mdp_intr_fn);

static eventhandler_tag msm_kms_eh;
static struct callout msm_kms_callout;
static void (*volatile msm_kms_poll_fn)(void *);
static void *msm_kms_poll_arg;
static sbintime_t msm_kms_poll_sbt;

static char msm_kms_info[160] = "off";
SYSCTL_STRING(_hw_msm, OID_AUTO, kms_info, CTLFLAG_RD, msm_kms_info,
    sizeof(msm_kms_info), "KMS: mode, source pipes, vblank source");
static u_long msm_kms_counts[MSM_KMS_NCOUNT];
SYSCTL_ULONG(_hw_msm, OID_AUTO, kms_vblanks, CTLFLAG_RD,
    &msm_kms_counts[MSM_KMS_VBLANKS], 0, "KMS vblanks handled");
SYSCTL_ULONG(_hw_msm, OID_AUTO, kms_flips, CTLFLAG_RD,
    &msm_kms_counts[MSM_KMS_FLIPS], 0, "KMS flips done (events sent)");
SYSCTL_ULONG(_hw_msm, OID_AUTO, kms_flips_late, CTLFLAG_RD,
    &msm_kms_counts[MSM_KMS_FLIPS_LATE], 0,
    "KMS vblanks with a flip's flush still pending");
SYSCTL_ULONG(_hw_msm, OID_AUTO, kms_underruns, CTLFLAG_RD,
    &msm_kms_counts[MSM_KMS_UNDERRUNS], 0, "INTF_1 underrun interrupts");

bool
msm_bsd_kms_enabled(void)
{
	return (msm_kms != 0);
}

int
msm_bsd_kms_intr_register(void (*fn)(void *, uint32_t, uint32_t), void *arg)
{
	char name[] = "dev.qcom_dpu.0.irq";
	size_t len = sizeof(int);
	int on = 0;

	if (msm_kms_irq == 0) {
		printf("msm: kms: vblank polled (hw.msm.kms_irq=0)\n");
		return (0);
	}
	if (kernel_sysctlbyname(curthread, name, &on, &len, NULL, 0, NULL,
	    0) != 0 || on == 0) {
		printf("msm: kms: qcom_dpu has no MDSS interrupt (%s: "
		    "patches/0053, hw.qcom_dpu.irq): vblank polled\n", name);
		return (0);
	}
	msm_kms_eh = EVENTHANDLER_REGISTER(qcom_dpu_mdp_intr,
	    (qcom_dpu_mdp_intr_fn)fn, arg, EVENTHANDLER_PRI_ANY);
	if (msm_kms_eh == NULL)
		return (-ENOMEM);
	printf("msm: kms: the DPU's interrupts from qcom_dpu's MDSS "
	    "interrupt\n");
	return (1);
}

void
msm_bsd_kms_intr_deregister(void)
{
	/* waits for a handler that runs */
	if (msm_kms_eh != NULL)
		EVENTHANDLER_DEREGISTER(qcom_dpu_mdp_intr, msm_kms_eh);
	msm_kms_eh = NULL;
}

static void
msm_kms_tick(void *arg)
{
	void (*fn)(void *) = msm_kms_poll_fn;

	if (fn == NULL)
		return;
	fn(msm_kms_poll_arg);
	if (msm_kms_poll_fn != NULL)
		callout_schedule_sbt(&msm_kms_callout, msm_kms_poll_sbt,
		    msm_kms_poll_sbt / 8, 0);
}

/*
 * Called with sleep mutexes held (DRM's vblank lock): no waiting here, a
 * tick that runs may come once more and finds nothing to call
 */
void
msm_bsd_kms_poll(void (*fn)(void *), void *arg, unsigned int us)
{
	if (fn == NULL || us == 0) {
		msm_kms_poll_fn = NULL;
		callout_stop(&msm_kms_callout);
		return;
	}
	msm_kms_poll_arg = arg;
	msm_kms_poll_sbt = us * SBT_1US;
	msm_kms_poll_fn = fn;
	callout_reset_sbt(&msm_kms_callout, msm_kms_poll_sbt,
	    msm_kms_poll_sbt / 8, msm_kms_tick, NULL, 0);
}

void
msm_bsd_kms_poll_drain(void)
{
	msm_kms_poll_fn = NULL;
	callout_drain(&msm_kms_callout);
}

void
msm_bsd_kms_count(int what)
{
	if (what >= 0 && what < MSM_KMS_NCOUNT)
		atomic_add_long(&msm_kms_counts[what], 1);
}

void
msm_bsd_kms_set_info(const char *info)
{
	strlcpy(msm_kms_info, info, sizeof(msm_kms_info));
}

static void
msm_bsd_kms_load(void *arg)
{
	callout_init(&msm_kms_callout, 1);
}
SYSINIT(msm_bsd_kms_load, SI_SUB_DRIVERS, SI_ORDER_FIRST, msm_bsd_kms_load,
    NULL);

/* --- secure monitor calls (qcom_dsp's qcom_scm.c) ---------------------------------- */

int
msm_bsd_scm_available(void)
{
	return (qcom_scm_available() == 0);
}

int
msm_bsd_pas_init_image(uint32_t pas_id, uint64_t metadata_pa)
{
	return (-qcom_scm_pas_init_image(pas_id, metadata_pa));
}

int
msm_bsd_pas_mem_setup(uint32_t pas_id, uint64_t pa, uint64_t size)
{
	return (-qcom_scm_pas_mem_setup(pas_id, pa, size));
}

int
msm_bsd_pas_auth_and_reset(uint32_t pas_id)
{
	return (-qcom_scm_pas_auth_and_reset(pas_id));
}

int
msm_bsd_pas_shutdown(uint32_t pas_id)
{
	return (-qcom_scm_pas_shutdown(pas_id));
}

int
msm_bsd_pas_supported(uint32_t pas_id)
{
	bool s = false;

	return (qcom_scm_pas_supported(pas_id, &s) == 0 && s);
}

/* --- the power-up self test (device run gpu1) --------------------------------------- */

#define	A6XX_RBBM_STATUS	(0x210 * 4)
#define	A6XX_CP_SCRATCH_REG(n)	((0x883 + (n)) * 4)
#define	SELFTEST_PATTERN	0x5a5a1234U

int
msm_bsd_selftest(void)
{
	static const int clks[] = { MSM_CLK_GPU_AHB, MSM_CLK_CXO,
	    MSM_CLK_CX_GMU, MSM_CLK_BIMC_GPU_AXI, MSM_CLK_GPU_MEMNOC_GFX,
	    MSM_CLK_GX_GFX3D };
	struct msm_regs gpu = { .compat = "qcom,adreno" };
	uint32_t st, v, old;
	int error, n = 0;

	printf("msm: selftest: GPU power-up (CX, SMMU, GX, clocks, gfx3d at "
	    "%lu Hz)\n", msm_gfx3d_ftbl[0].rate);
	if ((error = regs_map(&gpu)) != 0)
		return (error);
	msm_bsd_cx_level(64);	/* RPM_SMD_LEVEL_LOW_SVS: opp-320000000 */
	if ((error = msm_bsd_gdsc(MSM_PD_GPU_CX, true, false)) != 0)
		goto unmap;
	printf("msm: selftest: CX GDSC %#x (status %#x)\n",
	    rd(&gpucc, 0x106c), rd(&gpucc, 0x1540));
	if ((error = msm_bsd_gdsc(MSM_PD_GPU_GX, true, false)) != 0)
		goto cx;
	printf("msm: selftest: GX GDSC %#x, clamp %#x\n", rd(&gpucc, 0x100c),
	    rd(&gpucc, 0x1508));
	if ((error = msm_bsd_clk_set_rate(MSM_CLK_GX_GFX3D,
	    msm_gfx3d_ftbl[0].rate)) != 0)
		goto gx;
	for (n = 0; n < (int)nitems(clks); n++)
		if ((error = msm_bsd_clk_enable(clks[n])) != 0) {
			printf("msm: selftest: %s: %d\n",
			    msm_bsd_clk_name(clks[n]), error);
			goto clocks;
		}
	printf("msm: selftest: PLL1 mode %#x L %#x alpha %#x:%#x, gfx3d RCG "
	    "cmd %#x cfg %#x, CBCR %#x\n", rd(&gpucc, 0x100), rd(&gpucc, 0x104),
	    rd(&gpucc, 0x10c), rd(&gpucc, 0x108), rd(&gpucc, GFX3D_RCG),
	    rd(&gpucc, GFX3D_RCG + RCG_CFG), rd(&gpucc, 0x1054));
	st = rd(&gpu, A6XX_RBBM_STATUS);
	old = rd(&gpu, A6XX_CP_SCRATCH_REG(7));
	wr(&gpu, A6XX_CP_SCRATCH_REG(7), SELFTEST_PATTERN);
	v = rd(&gpu, A6XX_CP_SCRATCH_REG(7));
	wr(&gpu, A6XX_CP_SCRATCH_REG(7), old);
	printf("msm: selftest: RBBM_STATUS %#x, CP_SCRATCH_REG7 %#x -> %#x: "
	    "%s\n", st, SELFTEST_PATTERN, v, v == SELFTEST_PATTERN ?
	    "the GPU's registers answer" : "NO ANSWER");
	if (v != SELFTEST_PATTERN)
		error = -EIO;
clocks:
	while (n-- > 0)
		msm_bsd_clk_disable(clks[n]);
gx:
	msm_bsd_gdsc(MSM_PD_GPU_GX, false, false);
cx:
	msm_bsd_gdsc(MSM_PD_GPU_CX, false, false);
unmap:
	regs_unmap(&gpu);
	printf("msm: selftest: %s (%d)\n", error == 0 ? "passed" : "FAILED",
	    error);
	return (error);
}

static int
msm_selftest_sysctl(SYSCTL_HANDLER_ARGS)
{
	int error, v = 0;

	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL || v == 0)
		return (error);
	return (-msm_bsd_selftest());
}
SYSCTL_PROC(_hw_msm, OID_AUTO, selftest, CTLTYPE_INT | CTLFLAG_RW |
    CTLFLAG_MPSAFE, NULL, 0, msm_selftest_sysctl, "I",
    "Power the GPU up and check its registers (write 1)");

/* hw.msm.selftest=1 (loader): at load, before msm's drivers attach */
static void
msm_bsd_load(void *arg)
{
	int v = 0;

	if (TUNABLE_INT_FETCH("hw.msm.selftest", &v) && v != 0)
		msm_bsd_selftest();
}
SYSINIT(msm_bsd_load, SI_SUB_DRIVERS, SI_ORDER_ANY, msm_bsd_load, NULL);

static void
msm_bsd_unload(void *arg)
{
	msm_bsd_kms_intr_deregister();
	msm_bsd_kms_poll_drain();
	regs_unmap(&smmu);
	regs_unmap(&gcc);
	regs_unmap(&gpucc);
}
SYSUNINIT(msm_bsd_unload, SI_SUB_DRIVERS, SI_ORDER_ANY, msm_bsd_unload, NULL);
