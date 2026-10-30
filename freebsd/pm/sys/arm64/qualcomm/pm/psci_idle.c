/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * CPU idle states through PSCI for FreeBSD/arm64, which has no cpuidle
 * framework: its cpu_idle() only executes wfi. With patches/0032 it asks
 * arm64_cpu_idle_hook first, with the time to the CPU's next event
 * (cpu_idleclock()).
 *
 * The states come from the device tree as Linux's cpuidle-psci and
 * cpuidle-psci-domain read them (bindings cpu/idle-states.yaml,
 * power/domain-idle-state.yaml): a CPU's power domain (power-domains,
 * "psci") names its own states (domain-idle-states), its parent domain the
 * cluster's; without domains, the CPU's cpu-idle-states. Each state:
 * arm,psci-suspend-param, entry/exit latency, min-residency-us,
 * local-timer-stop. On the Pro1X (sm6115.dtsi): the cores' rail power
 * collapse 0x40000003 (silver: at least 1182 us, gold: 1110 us), the
 * clusters' GDHS (0x40000022 / 0x40000042) and power collapse
 * (0x41000044, 7376 us / 8094 us), all power-down states, the cores'
 * timer stopping in them.
 *
 * As Linux does in OS-initiated mode (PSCI 1.0 SET_SUSPEND_MODE, when
 * PSCI_FEATURES(CPU_SUSPEND) has bit 0): the last online CPU of a cluster
 * going down asks for the cluster's state, whose parameter replaces the
 * core's (Linux: psci_pd_power_off sets the domain state, which wins);
 * the firmware refuses (DENIED) when another core of the cluster is in
 * fact running. Without OSI only the cores' states (pc_cluster: the
 * cluster's too, composed by the firmware; QEMU).
 *
 * A power-down loses the core's context: psci_idle_asm.S keeps it and
 * comes back through psci_idle_resume with the MMU off, which needs an
 * identity map of the kernel image (built here, a 1 GiB block). The FP
 * registers are not kept: the thread that used them last had them saved
 * at its switch, and fpcurthread is cleared so that they are loaded
 * again. The core's timer stops (local-timer-stop): the memory-mapped
 * timer frame wakes the CPU instead (arm_tmr_mem.c); without it such
 * states are not used.
 *
 * Off by default: hw.psci_idle.enable=1 (tunable and sysctl).
 * hw.psci_idle.depth: 0 wfi only, 1 the cores' states, 2 and more the
 * cluster's first (depth - 1) states. hw.psci_idle.osi=0: no OS-initiated
 * mode; hw.psci_idle.pc_cluster=1: the cluster's states also without it;
 * hw.psci_idle.simulate=1 (QEMU, whose CPU_SUSPEND is a wfi): each
 * power-down played after the call (psci_idle_asm.S). hw.psci_idle.stats:
 * per CPU how often each state was entered, the time in it, refusals and
 * returns through psci_idle_resume.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/sbuf.h>
#include <sys/smp.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>

#include <machine/armreg.h>
#include <machine/cpu.h>
#include <machine/cpufunc.h>
#include <machine/machdep.h>
#include <machine/pte.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/psci/psci.h>
#include <dev/psci/smccc.h>

#include "psci_idle.h"

_Static_assert(sizeof(struct psci_idle_ctx) == PIC_SIZE, "context size");
_Static_assert(offsetof(struct psci_idle_ctx, sp) == PIC_SP, "sp");
_Static_assert(offsetof(struct psci_idle_ctx, ttbr0) == PIC_TTBR0, "ttbr0");
_Static_assert(offsetof(struct psci_idle_ctx, mdscr) == PIC_MDSCR, "mdscr");
_Static_assert(offsetof(struct psci_idle_ctx, cntv_cval) == PIC_CNTV_CVAL,
    "cval");
_Static_assert(offsetof(struct psci_idle_ctx, icc_grpen1) == PIC_ICC_GRPEN1,
    "grpen1");
_Static_assert(offsetof(struct psci_idle_ctx, idmap) == PIC_IDMAP, "idmap");
_Static_assert(offsetof(struct psci_idle_ctx, sim_pa) == PIC_SIM_PA, "sim");
_Static_assert(offsetof(struct psci_idle_ctx, resumed) == PIC_RESUMED, "res");
_Static_assert(PAGE_SIZE == 4096, "the identity map assumes 4 KiB pages");

/* cpu_idle()'s hook (patches/0032) */
extern int (*arm64_cpu_idle_hook)(sbintime_t);

#define	PSCI_FNID_SET_SUSPEND_MODE	0x8400000f
#define	PSCI_MODE_PC			0
#define	PSCI_MODE_OSI			1
#define	PIDLE_MAXST	4
#define	PIDLE_MAXCL	4

struct pidle_state {
	char		name[40];
	uint32_t	param;
	uint32_t	entry_us, exit_us, min_us;
	bool		timer_stop, powerdown, off;
	u_int		fails;
};

struct pidle_cluster {
	phandle_t	pd;
	int		nstates;
	struct pidle_state st[PIDLE_MAXST];
	int		ncpus;		/* online CPUs in it */
	volatile u_int	down;		/* of them in a power-down state */
	u_int		denied_row;	/* refusals in a row */
	bool		off;		/* refused too often */
};

struct pidle_cpu {
	bool		present;
	int		cluster;	/* -1: none */
	int		nstates;
	struct pidle_state st[PIDLE_MAXST];
	struct psci_idle_ctx *ctx;
	vm_paddr_t	ctx_pa;
	/* statistics: entries and time per core and cluster state */
	uint64_t	n_wfi, n_core[PIDLE_MAXST], n_cl[PIDLE_MAXST];
	sbintime_t	t_core[PIDLE_MAXST], t_cl[PIDLE_MAXST];
	uint64_t	n_denied, n_failed, n_early;
	int		last_err;
};

static struct pidle_cpu *pidle_cpus;
static struct pidle_cluster pidle_cl[PIDLE_MAXCL];
static int pidle_ncl;
static vm_paddr_t pidle_resume_pa, pidle_sim_pa, pidle_idmap_pa;
static bool pidle_hvc, pidle_ext, pidle_osi, pidle_usable;
static uint32_t pidle_version;
static int pidle_feat = -1;
static uint32_t pidle_off_at_boot;	/* AFFINITY_INFO before the APs */
static int pidle_ndt_cpus;

static int pidle_enable;
static int pidle_depth = 99;
static int pidle_want_osi = 1;
static int pidle_pc_cluster;
static int pidle_simulate;

static SYSCTL_NODE(_hw, OID_AUTO, psci_idle, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "CPU idle states through PSCI");
SYSCTL_INT(_hw_psci_idle, OID_AUTO, enable, CTLFLAG_RWTUN, &pidle_enable, 0,
    "Use the PSCI idle states (0: wfi only)");
SYSCTL_INT(_hw_psci_idle, OID_AUTO, depth, CTLFLAG_RWTUN, &pidle_depth, 0,
    "0 wfi, 1 the cores' states, 2+ the clusters' first depth-1 states");
SYSCTL_INT(_hw_psci_idle, OID_AUTO, osi, CTLFLAG_RDTUN, &pidle_want_osi, 0,
    "Ask for OS-initiated mode");
SYSCTL_INT(_hw_psci_idle, OID_AUTO, pc_cluster, CTLFLAG_RDTUN,
    &pidle_pc_cluster, 0, "The clusters' states without OS-initiated mode");
SYSCTL_INT(_hw_psci_idle, OID_AUTO, simulate, CTLFLAG_RDTUN, &pidle_simulate,
    0, "Play each power-down after CPU_SUSPEND (QEMU)");

static long
pidle_call(register_t fid, register_t a1, register_t a2, register_t a3)
{
	return (psci_call(fid, a1, a2, a3));
}

/* The /cpus node of an MPIDR */
static phandle_t
pidle_cpu_node(uint64_t mpidr)
{
	phandle_t cpus, child;
	pcell_t reg[2], ac;
	uint64_t r;
	int n;

	cpus = OF_finddevice("/cpus");
	if (cpus == -1)
		return (-1);
	ac = 1;
	OF_getencprop(cpus, "#address-cells", &ac, sizeof(ac));
	for (child = OF_child(cpus); child != 0; child = OF_peer(child)) {
		n = OF_getencprop(child, "reg", reg, sizeof(reg));
		if (n < (int)sizeof(pcell_t))
			continue;
		r = (ac == 2 && n >= 8) ? ((uint64_t)reg[0] << 32 | reg[1]) :
		    reg[0];
		if (r == (mpidr & CPU_AFF_MASK))
			return (child);
	}
	return (-1);
}

static bool
pidle_powerdown(uint32_t param)
{
	/* Linux's psci_power_state_loses_context */
	return ((param & (pidle_ext ? (1u << 30) : (1u << 16))) != 0);
}

static int
pidle_parse_state(phandle_t node, struct pidle_state *st)
{
	char buf[40];

	memset(st, 0, sizeof(*st));
	if (!ofw_bus_node_status_okay(node))
		return (ENXIO);
	if (OF_getencprop(node, "arm,psci-suspend-param", &st->param,
	    sizeof(st->param)) != sizeof(st->param))
		return (ENXIO);
	OF_getencprop(node, "entry-latency-us", &st->entry_us,
	    sizeof(st->entry_us));
	OF_getencprop(node, "exit-latency-us", &st->exit_us,
	    sizeof(st->exit_us));
	OF_getencprop(node, "min-residency-us", &st->min_us,
	    sizeof(st->min_us));
	st->timer_stop = OF_hasprop(node, "local-timer-stop");
	st->powerdown = pidle_powerdown(st->param);
	if (OF_getprop(node, "idle-state-name", buf, sizeof(buf)) <= 0 &&
	    OF_getprop(node, "name", buf, sizeof(buf)) <= 0)
		strlcpy(buf, "?", sizeof(buf));
	buf[sizeof(buf) - 1] = '\0';
	strlcpy(st->name, buf, sizeof(st->name));
	return (0);
}

/* The states of a phandle list property, by min-residency */
static int
pidle_parse_list(phandle_t node, const char *prop, struct pidle_state *st)
{
	pcell_t *refs;
	ssize_t len;
	int i, j, n;

	len = OF_getencprop_alloc_multi(node, prop, sizeof(pcell_t),
	    (void **)&refs);
	if (len <= 0)
		return (0);
	n = 0;
	for (i = 0; i < len && n < PIDLE_MAXST; i++) {
		if (pidle_parse_state(OF_node_from_xref(refs[i]), &st[n]) != 0)
			continue;
		/* keep them sorted, shallowest first */
		for (j = n; j > 0 && st[j - 1].min_us > st[j].min_us; j--) {
			struct pidle_state t = st[j];

			st[j] = st[j - 1];
			st[j - 1] = t;
		}
		n++;
	}
	OF_prop_free(refs);
	return (n);
}

/* A node's power domain named "psci" (or its first) */
static phandle_t
pidle_domain(phandle_t node)
{
	pcell_t ref;
	int idx;

	if (!OF_hasprop(node, "power-domains"))
		return (-1);
	idx = 0;
	if (OF_hasprop(node, "power-domain-names") &&
	    ofw_bus_find_string_index(node, "power-domain-names", "psci",
	    &idx) != 0)
		idx = 0;
	if (OF_getencprop(node, "power-domains", &ref, sizeof(ref)) <= 0)
		return (-1);
	if (idx != 0) {
		pcell_t *refs;
		ssize_t n;

		n = OF_getencprop_alloc_multi(node, "power-domains",
		    sizeof(pcell_t), (void **)&refs);
		if (n <= idx) {
			OF_prop_free(refs);
			return (-1);
		}
		ref = refs[idx];
		OF_prop_free(refs);
	}
	return (OF_node_from_xref(ref));
}

static void
pidle_parse_cpu(u_int cpu, phandle_t node)
{
	struct pidle_cpu *pc = &pidle_cpus[cpu];
	phandle_t pd, cpd;
	int i;

	pc->cluster = -1;
	pd = pidle_domain(node);
	if (pd != -1) {
		pc->nstates = pidle_parse_list(pd, "domain-idle-states",
		    pc->st);
		cpd = pidle_domain(pd);
		if (cpd != -1) {
			for (i = 0; i < pidle_ncl; i++)
				if (pidle_cl[i].pd == cpd)
					break;
			if (i == pidle_ncl && i < PIDLE_MAXCL) {
				pidle_cl[i].pd = cpd;
				pidle_cl[i].nstates = pidle_parse_list(cpd,
				    "domain-idle-states", pidle_cl[i].st);
				pidle_ncl++;
			}
			if (i < pidle_ncl)
				pc->cluster = i;
		}
	} else
		pc->nstates = pidle_parse_list(node, "cpu-idle-states", pc->st);
}

/*
 * Early, before the other CPUs start (SI_SUB_CPU, after psci_init): the
 * firmware's version and features, which CPUs are off (Linux's kexec
 * takes them down with CPU_OFF), and the suspend mode.
 */
static void
psci_idle_early(void *dummy __unused)
{
	phandle_t cpus, child;
	pcell_t reg[2], ac;
	uint64_t r;
	long a;
	int n, i;

	if (!psci_present)
		return;
	/* the psci device attaches later: the firmware asked directly */
	pidle_version = (uint32_t)pidle_call(PSCI_FNID_VERSION, 0, 0, 0);
	if ((int32_t)pidle_version < 0)
		pidle_version = 0;
	if (PSCI_VER_MAJOR(pidle_version) >= 1)
		pidle_feat = pidle_call(PSCI_FNID_FEATURES,
		    PSCI_FNID_CPU_SUSPEND, 0, 0);
	pidle_ext = pidle_feat >= 0 && (pidle_feat & 2) != 0;
	pidle_hvc = psci_callfn == arm_smccc_hvc;

	cpus = OF_finddevice("/cpus");
	ac = 1;
	if (cpus != -1)
		OF_getencprop(cpus, "#address-cells", &ac, sizeof(ac));
	i = 0;
	for (child = cpus == -1 ? 0 : OF_child(cpus); child != 0;
	    child = OF_peer(child)) {
		if (!ofw_bus_node_is_compatible(child, "arm,armv8") &&
		    !OF_hasprop(child, "enable-method"))
			continue;
		n = OF_getencprop(child, "reg", reg, sizeof(reg));
		if (n < (int)sizeof(pcell_t))
			continue;
		r = (ac == 2 && n >= 8) ? ((uint64_t)reg[0] << 32 | reg[1]) :
		    reg[0];
		if (PSCI_VER_MAJOR(pidle_version) >= 1 ||
		    PSCI_VER_MINOR(pidle_version) >= 2) {
			a = pidle_call(PSCI_FNID_AFFINITY_INFO, r, 0, 0);
			if (a == PSCI_AFFINITY_INFO_OFF && i < 32)
				pidle_off_at_boot |= 1u << i;
		}
		i++;
	}
	pidle_ndt_cpus = i;
	printf("psci_idle: PSCI %d.%d over %s, CPU_SUSPEND features %#x "
	    "(%s state ids%s); %d CPUs in the device tree, off at boot: %#x\n",
	    PSCI_VER_MAJOR(pidle_version), PSCI_VER_MINOR(pidle_version),
	    pidle_hvc ? "hvc" : "smc", pidle_feat < 0 ? 0 : pidle_feat,
	    pidle_ext ? "extended" : "original",
	    pidle_feat >= 0 && (pidle_feat & 1) ? ", OSI there" : "",
	    pidle_ndt_cpus, pidle_off_at_boot);

	/* the mode, as Linux's cpuidle-psci-domain sets it; only when used */
	if (!pidle_enable || pidle_feat < 0 || (pidle_feat & 1) == 0)
		return;
	a = pidle_call(PSCI_FNID_SET_SUSPEND_MODE,
	    pidle_want_osi ? PSCI_MODE_OSI : PSCI_MODE_PC, 0, 0);
	if (a == 0)
		pidle_osi = pidle_want_osi != 0;
	else
		printf("psci_idle: SET_SUSPEND_MODE %s refused (%ld)\n",
		    pidle_want_osi ? "OSI" : "PC", a);
}
SYSINIT(psci_idle_early, SI_SUB_CPU, SI_ORDER_SECOND, psci_idle_early, NULL);

/* The identity map for the way back: the 1 GiB block with the code */
static vm_paddr_t
pidle_make_idmap(vm_paddr_t pa)
{
	pd_entry_t *l0, *l1;

	l0 = (pd_entry_t *)kmem_malloc(PAGE_SIZE, M_WAITOK | M_ZERO);
	l1 = (pd_entry_t *)kmem_malloc(PAGE_SIZE, M_WAITOK | M_ZERO);
	l0[pmap_l0_index(pa)] = PHYS_TO_PTE(vtophys(l1)) | L0_TABLE;
	l1[pmap_l1_index(pa)] = PHYS_TO_PTE(pa & ~L1_OFFSET) | ATTR_AF |
	    pmap_sh_attr | ATTR_S1_IDX(VM_MEMATTR_WRITE_BACK) | ATTR_S1_UXN |
	    L1_BLOCK;
	cpu_dcache_wb_range(l0, PAGE_SIZE);
	cpu_dcache_wb_range(l1, PAGE_SIZE);
	return (vtophys(l0));
}

static void
pidle_print_state(const char *what, struct pidle_state *s)
{
	printf("psci_idle:   %s %s: %#x, %u us at least, exit %u us%s%s\n",
	    what, s->name, s->param, s->min_us, s->exit_us,
	    s->powerdown ? ", power-down" : "",
	    s->timer_stop ? ", the timer stops" : "");
}

static int psci_idle_enter(sbintime_t sbt);

/*
 * After the APs run (SI_SUB_SMP): the states of every CPU, the clusters'
 * online CPUs, the contexts and the identity map; then the hook.
 */
static void
psci_idle_start(void *dummy __unused)
{
	struct pcpu *pc;
	struct psci_idle_ctx *ctxs;
	vm_offset_t ctxva;
	phandle_t node;
	uint64_t isar1;
	u_int cpu, n;
	int i;
	bool any = false;

	if (!psci_present)
		return;
	pidle_cpus = malloc(sizeof(*pidle_cpus) * (mp_maxid + 1), M_DEVBUF,
	    M_WAITOK | M_ZERO);
	CPU_FOREACH(cpu) {
		pc = pcpu_find(cpu);
		node = pidle_cpu_node(pc->pc_mpidr);
		if (node == -1)
			continue;
		pidle_cpus[cpu].present = true;
		pidle_parse_cpu(cpu, node);
		if (pidle_cpus[cpu].nstates > 0)
			any = true;
		if (pidle_cpus[cpu].cluster >= 0)
			pidle_cl[pidle_cpus[cpu].cluster].ncpus++;
	}
	if (!any) {
		printf("psci_idle: no idle states in the device tree: wfi\n");
		return;
	}

	/* what the context does not keep */
	if (has_hyp() || in_vhe()) {
		printf("psci_idle: the kernel runs at EL2: wfi only\n");
		return;
	}
	isar1 = READ_SPECIALREG(id_aa64isar1_el1);
	if ((isar1 & (ID_AA64ISAR1_APA_MASK | ID_AA64ISAR1_API_MASK)) != 0) {
		printf("psci_idle: pointer authentication (its keys are not "
		    "kept): wfi only\n");
		return;
	}
	if ((READ_SPECIALREG(tcr_el1) & TCR_DS) != 0) {
		printf("psci_idle: LPA2 translation: wfi only\n");
		return;
	}

	pidle_resume_pa = vtophys((vm_offset_t)&psci_idle_resume);
	pidle_sim_pa = vtophys((vm_offset_t)&psci_idle_sim_off);
	if ((pidle_resume_pa >> L1_SHIFT) != (pidle_sim_pa >> L1_SHIFT)) {
		printf("psci_idle: the way back crosses a 1 GiB boundary\n");
		return;
	}
	pidle_idmap_pa = pidle_make_idmap(pidle_resume_pa);

	n = mp_maxid + 1;
	ctxva = (vm_offset_t)kmem_malloc(round_page(n * sizeof(*ctxs)),
	    M_WAITOK | M_ZERO);
	ctxs = (struct psci_idle_ctx *)ctxva;
	CPU_FOREACH(cpu) {
		pidle_cpus[cpu].ctx = &ctxs[cpu];
		pidle_cpus[cpu].ctx_pa = vtophys((vm_offset_t)&ctxs[cpu]);
		ctxs[cpu].idmap = pidle_idmap_pa;
		ctxs[cpu].sim_pa = pidle_sim_pa;
	}

	printf("psci_idle: %s mode, %s; %d CPUs, %d clusters; broadcast "
	    "timer: %s; resume at %#jx\n",
	    pidle_osi ? "OS-initiated" : "platform-coordinated",
	    pidle_enable ? "enabled" : "off (hw.psci_idle.enable=0)",
	    mp_ncpus, pidle_ncl, arm_tmr_mem_ok() ? "yes" : "none",
	    (uintmax_t)pidle_resume_pa);
	CPU_FOREACH(cpu) {
		struct pidle_cpu *p = &pidle_cpus[cpu];

		if (cpu != 0 && p->cluster == pidle_cpus[cpu - 1].cluster &&
		    p->nstates == pidle_cpus[cpu - 1].nstates &&
		    (p->nstates == 0 ||
		    p->st[0].param == pidle_cpus[cpu - 1].st[0].param))
			continue;
		printf("psci_idle: CPU %u (cluster %d) on:\n", cpu, p->cluster);
		for (i = 0; i < p->nstates; i++)
			pidle_print_state("core", &p->st[i]);
	}
	for (i = 0; i < pidle_ncl; i++) {
		printf("psci_idle: cluster %d: %d of its CPUs online\n", i,
		    pidle_cl[i].ncpus);
		for (int j = 0; j < pidle_cl[i].nstates; j++)
			pidle_print_state("cluster", &pidle_cl[i].st[j]);
	}
	pidle_usable = true;
	atomic_store_rel_ptr((volatile uintptr_t *)&arm64_cpu_idle_hook,
	    (uintptr_t)psci_idle_enter);
}
SYSINIT(psci_idle_start, SI_SUB_SMP + 1, SI_ORDER_ANY, psci_idle_start, NULL);

/* The deepest of n states (sorted) fitting us, below the limit */
static int
pidle_choose(struct pidle_state *st, int n, int limit, uint32_t us,
    bool bcast)
{
	int i;

	for (i = MIN(n, limit) - 1; i >= 0; i--) {
		if (st[i].off || st[i].min_us > us)
			continue;
		if (st[i].timer_stop && !bcast)
			continue;
		return (i);
	}
	return (-1);
}

static int
psci_idle_enter(sbintime_t sbt)
{
	struct pidle_cpu *pc;
	struct pidle_cluster *cl;
	struct pidle_state *s;
	sbintime_t t0, dt;
	uint32_t us, param;
	u_int cpu;
	int ci, cli, r;
	bool timer, counted, stop, down;

	if (!pidle_enable || pidle_depth < 1 || sbt <= 0)
		return (-1);
	cpu = PCPU_GET(cpuid);
	pc = &pidle_cpus[cpu];
	if (pc->nstates == 0)
		return (-1);
	us = sbt >= SBT_1S * 4000 ? UINT32_MAX : (uint32_t)sbttous(sbt);
	/* a state whose timer stops needs the broadcast frame (or QEMU) */
	timer = arm_tmr_mem_ok() || pidle_simulate;
	ci = pidle_choose(pc->st, pc->nstates, pc->nstates, us, timer);
	if (ci < 0) {
		pc->n_wfi++;
		return (-1);
	}
	s = &pc->st[ci];
	param = s->param;
	stop = s->timer_stop;
	down = s->powerdown;
	cli = -1;
	counted = false;
	cl = pc->cluster >= 0 ? &pidle_cl[pc->cluster] : NULL;
	if (cl != NULL && !cl->off && pidle_depth >= 2 && down &&
	    (pidle_osi || pidle_pc_cluster)) {
		counted = true;
		if (atomic_fetchadd_int(&cl->down, 1) + 1 == (u_int)cl->ncpus) {
			/* the cluster's last: its state replaces the core's */
			cli = pidle_choose(cl->st, cl->nstates, pidle_depth - 1,
			    us, timer);
			if (cli >= 0) {
				param = cl->st[cli].param;
				stop |= cl->st[cli].timer_stop;
				down |= cl->st[cli].powerdown;
			}
		}
	}
	if (stop && arm_tmr_mem_ok())
		arm_tmr_mem_arm(cpu, sbt);
	t0 = sbinuptime();
	if (down) {
		/* the last user of the FP registers loads them again */
		PCPU_SET(fpcurthread, NULL);
		r = psci_idle_suspend(PSCI_FNID_CPU_SUSPEND, param,
		    pidle_resume_pa, pc->ctx, pc->ctx_pa, pidle_hvc,
		    pidle_simulate);
	} else
		r = pidle_call(PSCI_FNID_CPU_SUSPEND, param, 0, 0);
	dt = sbinuptime() - t0;
	if (counted)
		atomic_subtract_int(&cl->down, 1);
	if (stop && arm_tmr_mem_ok())
		arm_tmr_mem_disarm(cpu);

	if (r == 0) {
		if (cli >= 0) {
			pc->n_cl[cli]++;
			pc->t_cl[cli] += dt;
			cl->denied_row = 0;
		} else {
			pc->n_core[ci]++;
			pc->t_core[ci] += dt;
		}
		s->fails = 0;
		return (0);
	}
	pc->last_err = r;
	if (r == PSCI_RETVAL_DENIED && cli >= 0) {
		/* another core of the cluster was not down after all */
		pc->n_denied++;
		if (++cl->denied_row >= 1000 && !cl->off) {
			cl->off = true;
			printf("psci_idle: cluster %d's states refused 1000 "
			    "times in a row: the cores' states only\n",
			    pc->cluster);
		}
		return (0);
	}
	pc->n_failed++;
	if (++s->fails >= 100 && !s->off) {
		s->off = true;
		printf("psci_idle: CPU %u: %s (%#x) refused 100 times (%d): "
		    "not used\n", cpu, s->name, param, r);
	}
	return (-1);
}

static int
pidle_sysctl_stats(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sb;
	struct pidle_cpu *pc;
	u_int cpu;
	int i, error;

	error = sysctl_wire_old_buffer(req, 0);
	if (error != 0)
		return (error);
	sbuf_new_for_sysctl(&sb, NULL, 512, req);
	if (!pidle_usable)
		sbuf_printf(&sb, "not in use");
	CPU_FOREACH(cpu) {
		if (!pidle_usable)
			break;
		pc = &pidle_cpus[cpu];
		sbuf_printf(&sb, "\ncpu%u: wfi %ju", cpu, (uintmax_t)pc->n_wfi);
		for (i = 0; i < pc->nstates; i++)
			sbuf_printf(&sb, ", %s %ju (%ju ms)", pc->st[i].name,
			    (uintmax_t)pc->n_core[i],
			    (uintmax_t)(pc->t_core[i] / SBT_1MS));
		if (pc->cluster >= 0)
			for (i = 0; i < pidle_cl[pc->cluster].nstates; i++)
				sbuf_printf(&sb, ", %s %ju (%ju ms)",
				    pidle_cl[pc->cluster].st[i].name,
				    (uintmax_t)pc->n_cl[i],
				    (uintmax_t)(pc->t_cl[i] / SBT_1MS));
		sbuf_printf(&sb, ", denied %ju, failed %ju (last %d), "
		    "resumed %ju", (uintmax_t)pc->n_denied,
		    (uintmax_t)pc->n_failed, pc->last_err,
		    (uintmax_t)(pc->ctx != NULL ? pc->ctx->resumed : 0));
	}
	if (pidle_usable)
		sbuf_printf(&sb, "\nbroadcast timer: %s, %ju interrupts",
		    arm_tmr_mem_ok() ? "yes" : "none",
		    (uintmax_t)arm_tmr_mem_fires());
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}
SYSCTL_PROC(_hw_psci_idle, OID_AUTO, stats,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0,
    pidle_sysctl_stats, "A", "Idle states entered per CPU");

static int
pidle_sysctl_mode(SYSCTL_HANDLER_ARGS)
{
	char buf[160];

	snprintf(buf, sizeof(buf), "PSCI %d.%d %s, %s mode, off at boot %#x "
	    "of %d", PSCI_VER_MAJOR(pidle_version),
	    PSCI_VER_MINOR(pidle_version), pidle_hvc ? "hvc" : "smc",
	    pidle_osi ? "OS-initiated" : "platform-coordinated",
	    pidle_off_at_boot, pidle_ndt_cpus);
	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}
SYSCTL_PROC(_hw_psci_idle, OID_AUTO, mode,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0,
    pidle_sysctl_mode, "A", "The firmware and its suspend mode");
