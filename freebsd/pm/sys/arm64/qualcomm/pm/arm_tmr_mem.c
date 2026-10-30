/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The memory-mapped generic timer ("arm,armv7-timer-mem") as the wake-up
 * of CPUs whose own timer stops in an idle state (local-timer-stop,
 * psci_idle.c), as Linux uses it for its tick broadcast (arch_mem_timer;
 * on the Pro1X 34 interrupts a second, docs/power-management.md).
 *
 * The SM6115 (sm6115.dtsi): CNTCTLBase at 0xf120000, frame 0 at 0xf121000
 * with its physical timer on SPI 8 and its virtual one on SPI 7, frames
 * 1-6 disabled; 19.2 MHz. Linux's arm_arch_timer_mmio.c: CNTTIDR (0x08)
 * says which frames have a virtual timer, CNTACR(n) (0x40 + 4n) opens a
 * frame's registers to this side ("try enabling everything, and see what
 * sticks"), a virtual frame is preferred, else a physical one; in a
 * frame CNTPCT 0x00, CNTVCT 0x08, CNTFRQ 0x10, CNTP_CVAL 0x20, CNTP_CTL
 * 0x2c, CNTV_CVAL 0x30, CNTV_CTL 0x3c.
 *
 * One frame for all CPUs: each CPU going into such a state leaves its
 * deadline here, the frame is set to the earliest one; its interrupt
 * (on whichever CPU it is routed to, which wakes) sends the CPUs whose
 * deadline has come an IPI_AST, which only wakes them: back in cpu_idle,
 * cpu_activeclock() runs what is due.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/smp.h>
#include <sys/sysctl.h>

#include <machine/bus.h>
#include <machine/cpufunc.h>
#include <machine/intr.h>
#include <machine/smp.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "psci_idle.h"

#define	CNTTIDR		0x08
#define	CNTTIDR_VIRT(n)	(2u << ((n) * 4))
#define	CNTACR(n)	(0x40 + (n) * 4)
#define	CNTACR_RPCT	(1u << 0)
#define	CNTACR_RVCT	(1u << 1)
#define	CNTACR_RFRQ	(1u << 2)
#define	CNTACR_RVOFF	(1u << 3)
#define	CNTACR_RWVT	(1u << 4)
#define	CNTACR_RWPT	(1u << 5)
#define	F_CNTPCT	0x00
#define	F_CNTVCT	0x08
#define	F_CNTFRQ	0x10
#define	F_CNTP_CVAL	0x20
#define	F_CNTP_CTL	0x2c
#define	F_CNTV_CVAL	0x30
#define	F_CNTV_CTL	0x3c
#define	CTL_ENABLE	(1u << 0)
#define	CTL_IMASK	(1u << 1)
#define	CTL_ISTATUS	(1u << 2)

struct tmem_softc {
	device_t	dev;
	struct resource	*ctl, *frame, *irq;
	void		*ih;
	bool		virt;
	u_int		frameno;
	uint64_t	freq;
	struct mtx	mtx;
	uint64_t	*deadline;	/* per CPU, in counts; 0: none */
	uint64_t	set;		/* what the frame is set to; 0: off */
	uint64_t	fires, ipis;
};

static struct tmem_softc *tmem_sc;

static struct ofw_compat_data compat_data[] = {
	{ "arm,armv7-timer-mem",	1 },
	{ NULL,				0 }
};

static uint64_t
tmem_count(struct tmem_softc *sc)
{
	bus_size_t off = sc->virt ? F_CNTVCT : F_CNTPCT;
	uint32_t hi, lo, hi2;

	do {
		hi = bus_read_4(sc->frame, off + 4);
		lo = bus_read_4(sc->frame, off);
		hi2 = bus_read_4(sc->frame, off + 4);
	} while (hi != hi2);
	return ((uint64_t)hi << 32 | lo);
}

static void
tmem_program(struct tmem_softc *sc, uint64_t when)
{
	bus_size_t ctl = sc->virt ? F_CNTV_CTL : F_CNTP_CTL;
	bus_size_t cval = sc->virt ? F_CNTV_CVAL : F_CNTP_CVAL;

	/* CVAL is not written atomically: the timer off meanwhile */
	bus_write_4(sc->frame, ctl, CTL_IMASK);
	sc->set = when;
	if (when == 0)
		return;
	bus_write_8(sc->frame, cval, when);
	bus_write_4(sc->frame, ctl, CTL_ENABLE);
}

/* the earliest deadline left, or 0 */
static uint64_t
tmem_earliest(struct tmem_softc *sc)
{
	uint64_t m = 0;
	u_int cpu;

	CPU_FOREACH(cpu)
		if (sc->deadline[cpu] != 0 &&
		    (m == 0 || sc->deadline[cpu] < m))
			m = sc->deadline[cpu];
	return (m);
}

bool
arm_tmr_mem_ok(void)
{
	return (tmem_sc != NULL);
}

uint64_t
arm_tmr_mem_fires(void)
{
	return (tmem_sc != NULL ? tmem_sc->fires : 0);
}

void
arm_tmr_mem_arm(u_int cpu, sbintime_t sbt)
{
	struct tmem_softc *sc = tmem_sc;
	uint64_t ticks, when;

	if (sc == NULL)
		return;
	ticks = (uint64_t)(sbt >> 32) * sc->freq +
	    ((((uint64_t)sbt & 0xffffffffu) * sc->freq) >> 32);
	mtx_lock_spin(&sc->mtx);
	when = tmem_count(sc) + ticks;
	sc->deadline[cpu] = when;
	if (sc->set == 0 || when < sc->set)
		tmem_program(sc, when);
	mtx_unlock_spin(&sc->mtx);
}

void
arm_tmr_mem_disarm(u_int cpu)
{
	struct tmem_softc *sc = tmem_sc;
	uint64_t m;

	if (sc == NULL)
		return;
	mtx_lock_spin(&sc->mtx);
	if (sc->deadline[cpu] != 0) {
		sc->deadline[cpu] = 0;
		m = tmem_earliest(sc);
		if (m != sc->set)
			tmem_program(sc, m);
	}
	mtx_unlock_spin(&sc->mtx);
}

static int
tmem_intr(void *arg)
{
	struct tmem_softc *sc = arg;
	uint64_t now, slack;
	u_int cpu, self;

	self = PCPU_GET(cpuid);
	/* those due within 50 us now too, instead of another interrupt */
	slack = sc->freq / 20000;
	mtx_lock_spin(&sc->mtx);
	sc->fires++;
	bus_write_4(sc->frame, sc->virt ? F_CNTV_CTL : F_CNTP_CTL, CTL_IMASK);
	sc->set = 0;
	now = tmem_count(sc);
	CPU_FOREACH(cpu) {
		if (sc->deadline[cpu] == 0 || sc->deadline[cpu] > now + slack)
			continue;
		sc->deadline[cpu] = 0;
		if (cpu != self) {
			ipi_cpu(cpu, IPI_AST);
			sc->ipis++;
		}
	}
	tmem_program(sc, tmem_earliest(sc));
	mtx_unlock_spin(&sc->mtx);
	return (FILTER_HANDLED);
}

static int
tmem_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);
	device_set_desc(dev, "ARM memory-mapped timer (idle wake-up)");
	return (BUS_PROBE_DEFAULT);
}

/* A frame's first register window, through the timer node's ranges */
static int
tmem_frame_reg(phandle_t node, phandle_t frame, uint64_t *pa, uint64_t *size)
{
	pcell_t ac = 2, sc = 1, pac = 2, reg[4], *rg;
	phandle_t parent;
	uint64_t a, cb, pb, len;
	ssize_t n;
	int i, stride;

	OF_getencprop(node, "#address-cells", &ac, sizeof(ac));
	OF_getencprop(node, "#size-cells", &sc, sizeof(sc));
	parent = OF_parent(node);
	OF_getencprop(parent, "#address-cells", &pac, sizeof(pac));
	if (ac > 2 || sc > 2 || pac > 2 || ac == 0 || sc == 0)
		return (ENXIO);
	if (OF_getencprop(frame, "reg", reg, (ac + sc) * sizeof(pcell_t)) <
	    (ssize_t)((ac + sc) * sizeof(pcell_t)))
		return (ENXIO);
	a = ac == 2 ? ((uint64_t)reg[0] << 32 | reg[1]) : reg[0];
	*size = sc == 2 ? ((uint64_t)reg[ac] << 32 | reg[ac + 1]) : reg[ac];
	n = OF_getencprop_alloc_multi(node, "ranges", sizeof(pcell_t),
	    (void **)&rg);
	if (n <= 0) {
		*pa = a;		/* empty ranges: one to one */
		return (0);
	}
	stride = ac + pac + sc;
	for (i = 0; i + stride <= n; i += stride) {
		cb = ac == 2 ? ((uint64_t)rg[i] << 32 | rg[i + 1]) : rg[i];
		pb = pac == 2 ? ((uint64_t)rg[i + ac] << 32 | rg[i + ac + 1]) :
		    rg[i + ac];
		len = sc == 2 ? ((uint64_t)rg[i + ac + pac] << 32 |
		    rg[i + ac + pac + 1]) : rg[i + ac + pac];
		if (a >= cb && a + *size <= cb + len) {
			*pa = a - cb + pb;
			OF_prop_free(rg);
			return (0);
		}
	}
	OF_prop_free(rg);
	return (ENXIO);
}

static int
tmem_attach(device_t dev)
{
	struct tmem_softc *sc = device_get_softc(dev);
	phandle_t node, frame, iparent;
	pcell_t fn, icells, irq[3 * 2], freq;
	uint64_t pa, size;
	uint32_t tidr, acr, want;
	int rid, n, cells, irqno, idx, error;
	bool virt;

	sc->dev = dev;
	node = ofw_bus_get_node(dev);
	rid = 0;
	sc->ctl = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->ctl == NULL) {
		device_printf(dev, "no CNTCTLBase\n");
		return (ENXIO);
	}
	tidr = bus_read_4(sc->ctl, CNTTIDR);

	/* the first enabled frame this side can use, virtual preferred */
	error = ENXIO;
	for (frame = OF_child(node); frame != 0; frame = OF_peer(frame)) {
		if (!ofw_bus_node_status_okay(frame))
			continue;
		if (OF_getencprop(frame, "frame-number", &fn, sizeof(fn)) <= 0 ||
		    fn > 7)
			continue;
		want = CNTACR_RFRQ | CNTACR_RWPT | CNTACR_RPCT | CNTACR_RWVT |
		    CNTACR_RVOFF | CNTACR_RVCT;
		bus_write_4(sc->ctl, CNTACR(fn), want);
		acr = bus_read_4(sc->ctl, CNTACR(fn));
		n = OF_getencprop(frame, "interrupts", irq, sizeof(irq));
		iparent = ofw_bus_find_iparent(frame);
		icells = 3;
		if (iparent != 0)
			OF_getencprop(OF_node_from_xref(iparent),
			    "#interrupt-cells", &icells, sizeof(icells));
		cells = n > 0 ? n / (int)sizeof(pcell_t) : 0;
		/* the binding: the physical timer's interrupt, then the virtual */
		virt = (tidr & CNTTIDR_VIRT(fn)) != 0 &&
		    (acr & (CNTACR_RWVT | CNTACR_RVCT)) ==
		    (CNTACR_RWVT | CNTACR_RVCT) && cells >= 2 * (int)icells;
		if (!virt && ((acr & (CNTACR_RWPT | CNTACR_RPCT)) !=
		    (CNTACR_RWPT | CNTACR_RPCT) || cells < (int)icells)) {
			device_printf(dev, "frame %u: CNTACR %#x, not usable\n",
			    fn, acr);
			continue;
		}
		if (tmem_frame_reg(node, frame, &pa, &size) != 0)
			continue;
		idx = virt ? 1 : 0;
		irqno = ofw_bus_map_intr(dev, iparent, icells, &irq[idx * icells]);
		bus_set_resource(dev, SYS_RES_MEMORY, 1, pa, size);
		bus_set_resource(dev, SYS_RES_IRQ, 0, irqno, 1);
		rid = 1;
		sc->frame = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
		    RF_ACTIVE);
		rid = 0;
		sc->irq = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
		    RF_ACTIVE);
		if (sc->frame == NULL || sc->irq == NULL) {
			device_printf(dev, "frame %u: no resources\n", fn);
			continue;
		}
		sc->virt = virt;
		sc->frameno = fn;
		error = 0;
		break;
	}
	if (error != 0) {
		device_printf(dev, "no usable frame (CNTTIDR %#x)\n", tidr);
		goto fail;
	}
	if (OF_getencprop(node, "clock-frequency", &freq, sizeof(freq)) > 0)
		sc->freq = freq;
	else
		sc->freq = bus_read_4(sc->frame, F_CNTFRQ);
	if (sc->freq == 0)
		sc->freq = READ_SPECIALREG(cntfrq_el0);
	sc->deadline = malloc(sizeof(uint64_t) * (mp_maxid + 1), M_DEVBUF,
	    M_WAITOK | M_ZERO);
	mtx_init(&sc->mtx, "tmem", NULL, MTX_SPIN);
	tmem_program(sc, 0);
	if (bus_setup_intr(dev, sc->irq, INTR_TYPE_CLK | INTR_MPSAFE,
	    tmem_intr, NULL, sc, &sc->ih) != 0) {
		device_printf(dev, "no interrupt handler\n");
		goto fail;
	}
	device_printf(dev, "frame %u at %#jx, %s timer, %ju Hz, count %#jx\n",
	    sc->frameno, (uintmax_t)rman_get_start(sc->frame),
	    sc->virt ? "virtual" : "physical", (uintmax_t)sc->freq,
	    (uintmax_t)tmem_count(sc));
	tmem_sc = sc;
	return (0);
fail:
	if (sc->irq != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, 0, sc->irq);
	if (sc->frame != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, 1, sc->frame);
	bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->ctl);
	return (ENXIO);
}

static int
tmem_sysctl_state(SYSCTL_HANDLER_ARGS)
{
	struct tmem_softc *sc = arg1;
	char buf[96];

	snprintf(buf, sizeof(buf), "%ju interrupts, %ju IPIs, set %#jx, "
	    "now %#jx", (uintmax_t)sc->fires, (uintmax_t)sc->ipis,
	    (uintmax_t)sc->set, (uintmax_t)tmem_count(sc));
	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}

static device_method_t tmem_methods[] = {
	DEVMETHOD(device_probe,		tmem_probe),
	DEVMETHOD(device_attach,	tmem_attach),
	DEVMETHOD_END
};

static driver_t tmem_driver = {
	"arm_tmr_mem",
	tmem_methods,
	sizeof(struct tmem_softc),
};

/* after the generic timer and the GIC */
EARLY_DRIVER_MODULE(arm_tmr_mem, simplebus, tmem_driver, 0, 0,
    BUS_PASS_TIMER + BUS_PASS_ORDER_LATE);

static void
tmem_sysctl_init(void *dummy __unused)
{
	struct tmem_softc *sc = tmem_sc;

	if (sc == NULL)
		return;
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(sc->dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev)), OID_AUTO,
	    "state", CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, sc, 0,
	    tmem_sysctl_state, "A", "Interrupts, IPIs, the frame's setting");
}
SYSINIT(tmem_sysctl, SI_SUB_SMP, SI_ORDER_ANY, tmem_sysctl_init, NULL);
