/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * pro1x_pm.ko: the SoC's sleep and suspend-to-idle for FreeBSD on the
 * Pro1X (freebsd/pm/README.md, stage 3), on top of psci_idle.
 *
 * The SM6115 is RPM-based: its sleep states (vlow: XO shutdown; vmin: CX
 * and MX at retention too) are the RPM's to enter, when every master
 * sleeps and no vote of a sleep set holds a resource up. While the SoC
 * sleeps the GIC is off; the MPM, in the always-on domain, wakes it by
 * its timer or one of its pins. Linux (device/kernel/patches/0011, and
 * KolibriOS's kernel/modem/rpmsleep.c the same way) hands the MPM to the
 * RPM when the last online CPU goes down: the next wake-up into the vMPM's
 * TIMER0/TIMER1 (a count of the arch timer), the wake pins' enables and
 * types, the status cleared, then the RPM's IPC bit.
 *
 *   hw.pro1x_pm.rpm_stats    the RPM's sleep statistics ("qcom,rpm-stats"
 *                            at 0x4690000, Linux's qcom_stats.c: the
 *                            records' offset in the word at 0x14, two
 *                            records of 48 bytes: the name, the count, last
 *                            entered and left, the accumulated time in
 *                            19.2 MHz counts, the masters' votes)
 *   hw.pro1x_pm.mpm=1        the hand-over when every online CPU is down and
 *                            the last one goes into its cluster's deepest
 *                            state (psci_idle_soc_hook); off by default, as
 *                            Linux's notify_rpm
 *   hw.pro1x_pm.mpm_pins     the wake pins, "pin:type ..." (h high level,
 *                            l low, r rising, f falling, b both edges);
 *                            default "86:h": the SPMI arbiter (power and
 *                            volume keys, the PMIC's RTC) through the MPM
 *                            as the device tree routes it (interrupts-
 *                            extended = <&mpm 86 IRQ_TYPE_LEVEL_HIGH>,
 *                            qcom,mpm-pin-map <86 183>). The gpio-keys'
 *                            pins in Linux's sm6115_mpm_map: GPIO 93 Home
 *                            46, 83 right Shift 9, 84 left Ctrl 39, 19 left
 *                            Alt 53; the keyboard's GPIOs 106, 107, 112
 *                            have none
 *   hw.pro1x_pm.usb_probe    stage 4's first look: the USB controller's
 *                            clocks, power domain and resets as Linux left
 *                            them, its registers if they are clocked
 *   hw.pro1x_pm.suspend=N    suspend to idle for N seconds (0: until a
 *                            device interrupt): the power_suspend handlers,
 *                            then every CPU in its deepest state with its
 *                            timer masked (psci_idle_freeze) in epochs of
 *                            hw.pro1x_pm.epoch seconds (15), between which
 *                            the callouts run (the watchdog is fed); a
 *                            device interrupt (the power key: PON through
 *                            the SPMI arbiter, INTID 215) or the deadline
 *                            ends it. hw.pro1x_pm.last: what it did
 *
 * The sleep set's votes are pro1x_rpmsleep.ko's (it needs the RPM channel
 * of freebsd/dsp's qcom_dsp.ko).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/eventhandler.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/sbuf.h>
#include <sys/sched.h>
#include <sys/smp.h>
#include <sys/sx.h>
#include <sys/sysctl.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/armreg.h>
#include <machine/cpufunc.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_subr.h>

#include "psci_idle.h"

#define	MPM_REG_ENABLE		0
#define	MPM_REG_FALLING		1
#define	MPM_REG_RISING		2
#define	MPM_REG_POLARITY	3
#define	MPM_REG_STATUS		4
#define	MPM_BANKS		4	/* up to 128 pins */
#define	RPM_STATS_OFFSET	0x14
#define	RPM_STATS_RECORD	48

static struct {
	struct sx	lock;
	/* the vMPM in the RPM's message RAM, the RPM's doorbell */
	volatile uint32_t *vmpm;
	vm_paddr_t	vmpm_pa;
	vm_size_t	vmpm_size;
	u_int		pins, banks;
	volatile uint32_t *ipc;
	vm_paddr_t	ipc_pa;
	uint32_t	ipc_bit;
	uint32_t	en[MPM_BANKS], rise[MPM_BANKS], fall[MPM_BANKS],
			pol[MPM_BANKS];
	int		mpm_on;
	uint64_t	handovers;
	uint32_t	seen[MPM_BANKS];	/* pins found in the status */
	/* the RPM's statistics */
	volatile uint32_t *stats;
	vm_size_t	stats_size;
	/* suspend */
	int		epoch;
	char		pins_str[96];
	char		last[320];
} pm = {
	.epoch = 15,
	.pins_str = "86:h",
};

static int pm_debug;

static int
pm_reg_index(u_int reg, u_int bank)
{
	/* Linux's qcom_mpm_write: (reg * stride + bank + 2) words */
	return (reg * pm.banks + bank + 2);
}

/* The wake pins from "pin:type ..." */
static int
pm_parse_pins(const char *s)
{
	uint32_t en[MPM_BANKS] = { 0 }, rise[MPM_BANKS] = { 0 },
	    fall[MPM_BANKS] = { 0 }, pol[MPM_BANKS] = { 0 }, bit;
	const char *p = s;
	char *end;
	u_long pin;
	u_int b;

	while (*p != '\0') {
		while (*p == ' ')
			p++;
		if (*p == '\0')
			break;
		pin = strtoul(p, &end, 10);
		if (end == p || *end != ':' || pin >= pm.pins ||
		    pin >= MPM_BANKS * 32)
			return (EINVAL);
		b = pin / 32;
		bit = 1u << (pin % 32);
		en[b] |= bit;
		switch (end[1]) {
		case 'h': pol[b] |= bit; break;
		case 'l': break;
		case 'r': rise[b] |= bit; break;
		case 'f': fall[b] |= bit; break;
		case 'b': rise[b] |= bit; fall[b] |= bit; break;
		default: return (EINVAL);
		}
		p = end + 2;
	}
	memcpy(pm.en, en, sizeof(en));
	memcpy(pm.rise, rise, sizeof(rise));
	memcpy(pm.fall, fall, sizeof(fall));
	memcpy(pm.pol, pol, sizeof(pol));
	return (0);
}

/*
 * psci_idle_soc_hook: every online CPU down, the last one into its
 * cluster's deepest state. Interrupts are off. As Linux's
 * mpm_cpu_pm_callback with notify_rpm (patch 0011): the next wake-up, the
 * wake pins (the other pins' types left alone, as KolibriOS's
 * mpm_prepare), the status cleared, then the RPM's doorbell.
 */
static void
pm_soc_hook(uint64_t wake)
{
	volatile uint32_t *v = pm.vmpm;
	uint32_t st;
	u_int b;

	if (!pm.mpm_on || v == NULL)
		return;
	if (wake == 0)
		wake = ~0ULL;
	v[0] = (uint32_t)wake;
	v[1] = (uint32_t)(wake >> 32);
	for (b = 0; b < pm.banks; b++) {
		st = v[pm_reg_index(MPM_REG_STATUS, b)];
		pm.seen[b] |= st & pm.en[b];
		v[pm_reg_index(MPM_REG_RISING, b)] =
		    (v[pm_reg_index(MPM_REG_RISING, b)] & ~pm.en[b]) | pm.rise[b];
		v[pm_reg_index(MPM_REG_FALLING, b)] =
		    (v[pm_reg_index(MPM_REG_FALLING, b)] & ~pm.en[b]) | pm.fall[b];
		v[pm_reg_index(MPM_REG_POLARITY, b)] =
		    (v[pm_reg_index(MPM_REG_POLARITY, b)] & ~pm.en[b]) | pm.pol[b];
		v[pm_reg_index(MPM_REG_ENABLE, b)] = pm.en[b];
		v[pm_reg_index(MPM_REG_STATUS, b)] = 0;
	}
	dsb(st);
	*pm.ipc = 1u << pm.ipc_bit;
	dsb(st);
	pm.handovers++;
}

/* The RPM's two records: name, count, accumulated seconds */
struct pm_rpmstat {
	char		name[5];
	uint32_t	count;
	uint64_t	accumulated;
};

static int
pm_rpm_stats(struct pm_rpmstat out[2])
{
	volatile uint32_t *r;
	uint32_t off, t;
	int i, j, n;

	if (pm.stats == NULL)
		return (0);
	off = pm.stats[RPM_STATS_OFFSET / 4] & 0xffff;
	for (n = 0, i = 0; i < 2; i++, off += RPM_STATS_RECORD) {
		if (off + RPM_STATS_RECORD > pm.stats_size || (off & 3) != 0)
			break;
		r = pm.stats + off / 4;
		t = r[0];
		for (j = 0; j < 4; j++) {
			char c = (char)(t >> (8 * j));

			out[i].name[j] = (c > ' ' && c < 127) ? c : '?';
		}
		out[i].name[4] = '\0';
		out[i].count = r[1];
		out[i].accumulated = (uint64_t)r[6] | (uint64_t)r[7] << 32;
		n++;
	}
	return (n);
}

static int
pm_sysctl_rpm_stats(SYSCTL_HANDLER_ARGS)
{
	struct pm_rpmstat st[2];
	struct sbuf sb;
	uint64_t f = READ_SPECIALREG(cntfrq_el0);
	int i, n, error;

	error = sysctl_wire_old_buffer(req, 0);
	if (error != 0)
		return (error);
	sbuf_new_for_sysctl(&sb, NULL, 256, req);
	n = pm_rpm_stats(st);
	if (n == 0)
		sbuf_printf(&sb, "no RPM statistics");
	for (i = 0; i < n; i++)
		sbuf_printf(&sb, "%s%s %u (%ju.%03ju s)", i ? ", " : "",
		    st[i].name, st[i].count, (uintmax_t)(st[i].accumulated / f),
		    (uintmax_t)(st[i].accumulated % f * 1000 / f));
	sbuf_printf(&sb, "; MPM hand-overs %ju, pins seen", (uintmax_t)pm.handovers);
	for (i = 0; i < (int)pm.banks; i++)
		sbuf_printf(&sb, " %08x", pm.seen[i]);
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

static int
pm_sysctl_mpm(SYSCTL_HANDLER_ARGS)
{
	int v = pm.mpm_on, error;

	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (v != 0 && (pm.vmpm == NULL || pm.ipc == NULL))
		return (ENXIO);
	pm.mpm_on = v != 0;
	return (0);
}

static int
pm_sysctl_pins(SYSCTL_HANDLER_ARGS)
{
	char buf[sizeof(pm.pins_str)];
	int error;

	strlcpy(buf, pm.pins_str, sizeof(buf));
	error = sysctl_handle_string(oidp, buf, sizeof(buf), req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	sx_xlock(&pm.lock);
	error = pm_parse_pins(buf);
	if (error == 0)
		strlcpy(pm.pins_str, buf, sizeof(pm.pins_str));
	sx_xunlock(&pm.lock);
	return (error);
}

static int
pm_sysctl_suspend(SYSCTL_HANDLER_ARGS)
{
	struct psci_idle_freeze_info fi;
	struct pm_rpmstat b[2], a[2];
	sbintime_t t0, end, until, dt;
	uint64_t downs = 0, wakes = 0, timer = 0, hand0;
	u_int intid = 0, frozen = UINT_MAX, wait = 0;
	int secs = 0, error, r, epochs = 0, nb, na;

	error = sysctl_handle_int(oidp, &secs, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (secs < 0)
		return (EINVAL);
	sx_xlock(&pm.lock);
	memset(b, 0, sizeof(b));
	memset(a, 0, sizeof(a));
	nb = pm_rpm_stats(b);
	hand0 = pm.handovers;
	EVENTHANDLER_INVOKE(power_suspend_early);
	EVENTHANDLER_INVOKE(power_suspend);
	thread_lock(curthread);
	sched_bind(curthread, 0);
	thread_unlock(curthread);
	t0 = sbinuptime();
	end = secs > 0 ? t0 + secs * SBT_1S : SBT_MAX;
	for (;;) {
		until = MIN(end, sbinuptime() + pm.epoch * SBT_1S);
		if (pm_debug)
			printf("pro1x_pm: freeze %d until +%jd ms\n", epochs,
			    (intmax_t)((until - sbinuptime()) / SBT_1MS));
		r = psci_idle_freeze(until, &fi);
		if (pm_debug)
			printf("pro1x_pm: freeze %d: %d, %ju wake-ups, %u frozen\n",
			    epochs, r, (uintmax_t)fi.wakes, fi.frozen);
		downs += fi.downs;
		wakes += fi.wakes;
		timer += fi.timer;
		frozen = MIN(frozen, fi.frozen);
		wait = MAX(wait, fi.wait_us);
		if (r == -2) {
			error = EOPNOTSUPP;
			break;
		}
		if (r > 0) {
			intid = r;
			break;
		}
		if (sbinuptime() >= end)
			break;
		/* between the epochs: the callouts run, the watchdog is fed */
		epochs++;
		pause_sbt("pmfrz", SBT_1MS * 50, 0, 0);
	}
	dt = sbinuptime() - t0;
	thread_lock(curthread);
	sched_unbind(curthread);
	thread_unlock(curthread);
	EVENTHANDLER_INVOKE(power_resume);
	na = pm_rpm_stats(a);
	snprintf(pm.last, sizeof(pm.last), "%s %ju.%03ju s in %d epochs, "
	    "ended by %s %u%s; %ju power-downs of CPU 0, %ju wake-ups (%ju "
	    "by its timer or the frame), at least %u other CPUs frozen "
	    "(after %u us); RPM: %s +%u, %s +%u; MPM hand-overs +%ju",
	    error != 0 ? "not suspended:" : "suspended",
	    (uintmax_t)(dt / SBT_1S), (uintmax_t)((dt % SBT_1S) * 1000 / SBT_1S),
	    epochs, intid != 0 ? "INTID" : "the deadline", intid,
	    intid >= 32 ? " (a device)" : "", (uintmax_t)downs,
	    (uintmax_t)wakes, (uintmax_t)timer, frozen == UINT_MAX ? 0 : frozen,
	    wait, na > 0 ? a[0].name : "-",
	    nb > 0 && na > 0 ? a[0].count - b[0].count : 0,
	    na > 1 ? a[1].name : "-",
	    nb > 1 && na > 1 ? a[1].count - b[1].count : 0,
	    (uintmax_t)(pm.handovers - hand0));
	printf("pro1x_pm: %s\n", pm.last);
	sx_xunlock(&pm.lock);
	return (error);
}

/*
 * Stage 4's first look (hw.pro1x_pm.usb_probe): what Linux leaves of the
 * USB controller at the kexec, read only. The GCC is always clocked; the
 * DWC3's and the PHY's registers are read only when their clocks run (a
 * read of an unclocked block can hang the bus). Offsets from Linux 7.1's
 * gcc-sm6115.c (branches: CLK_OFF bit 31, CLK_ENABLE bit 0; the GDSC:
 * PWR_ON bit 31, SW_COLLAPSE bit 0; BCRs: BLK_ARES bit 0), the DWC3's
 * global registers at 0xc100 (GSNPSID 0xc120, GCTL 0xc110) and device
 * registers at 0xc700 (DCFG, DCTL 0xc704, DSTS 0xc70c), dwc3/core.h.
 */
static const struct {
	uint32_t	off;
	const char	*name;
} pm_usb_gcc[] = {
	{ 0x1a000, "USB30_PRIM_BCR" },
	{ 0x1a004, "USB30_PRIM_GDSCR" },
	{ 0x1a010, "usb30_prim_master_cbcr" },
	{ 0x1a014, "usb30_prim_sleep_cbcr" },
	{ 0x1a018, "usb30_prim_mock_utmi_cbcr" },
	{ 0x1a01c, "usb30_prim_master_rcg_cmd" },
	{ 0x1a020, "usb30_prim_master_rcg_cfg" },
	{ 0x1a034, "usb30_prim_mock_utmi_rcg_cmd" },
	{ 0x1a080, "sys_noc_usb3_prim_axi_cbcr" },
	{ 0x1a084, "cfg_noc_usb3_prim_axi_cbcr" },
	{ 0x9f000, "usb3_prim_clkref_cbcr" },
	{ 0x1d008, "ahb2phy_usb_cbcr" },
	{ 0x1c000, "QUSB2PHY_PRIM_BCR" },
};

static uint32_t
pm_rd(vm_paddr_t pa)
{
	void *va = pmap_mapdev(trunc_page(pa), PAGE_SIZE);
	uint32_t v = *(volatile uint32_t *)((char *)va + (pa & PAGE_MASK));

	pmap_unmapdev(va, PAGE_SIZE);
	return (v);
}

static int
pm_sysctl_usb_probe(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sb;
	phandle_t gcc, dwc;
	bus_addr_t gpa, dpa;
	bus_size_t size;
	uint32_t v[nitems(pm_usb_gcc)];
	bool gdsc_on, master_on, iface_on;
	u_int i;
	int error;

	error = sysctl_wire_old_buffer(req, 0);
	if (error != 0)
		return (error);
	sbuf_new_for_sysctl(&sb, NULL, 1024, req);
	gcc = ofw_bus_find_compatible(OF_finddevice("/"), "qcom,gcc-sm6115");
	if (gcc <= 0 || ofw_reg_to_paddr(gcc, 0, &gpa, &size, NULL) != 0) {
		sbuf_printf(&sb, "no qcom,gcc-sm6115");
		goto out;
	}
	for (i = 0; i < nitems(pm_usb_gcc); i++) {
		v[i] = pm_rd(gpa + pm_usb_gcc[i].off);
		sbuf_printf(&sb, "%s%s %08x", i ? ", " : "", pm_usb_gcc[i].name,
		    v[i]);
	}
	gdsc_on = (v[1] & (1u << 31)) != 0;
	master_on = (v[2] & (1u << 31)) == 0;
	iface_on = (v[9] & (1u << 31)) == 0;
	sbuf_printf(&sb, "; GDSC %s, master clock %s, its bus clock %s, "
	    "reset %s", gdsc_on ? "on" : "off", master_on ? "on" : "off",
	    iface_on ? "on" : "off", (v[0] & 1) ? "asserted" : "released");
	dwc = ofw_bus_find_compatible(OF_finddevice("/"), "snps,dwc3");
	if (dwc <= 0 || ofw_reg_to_paddr(dwc, 0, &dpa, &size, NULL) != 0) {
		sbuf_printf(&sb, "; no snps,dwc3");
		goto out;
	}
	if (!gdsc_on || !master_on || !iface_on || (v[0] & 1) != 0) {
		sbuf_printf(&sb, "; the DWC3 at %#jx not read (not clocked)",
		    (uintmax_t)dpa);
		goto out;
	}
	sbuf_printf(&sb, "; DWC3 at %#jx: GSNPSID %08x GCTL %08x DCFG %08x "
	    "DCTL %08x DSTS %08x", (uintmax_t)dpa, pm_rd(dpa + 0xc120),
	    pm_rd(dpa + 0xc110), pm_rd(dpa + 0xc700), pm_rd(dpa + 0xc704),
	    pm_rd(dpa + 0xc70c));
out:
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

static SYSCTL_NODE(_hw, OID_AUTO, pro1x_pm, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "Pro1X: the SoC's sleep and suspend-to-idle");
SYSCTL_PROC(_hw_pro1x_pm, OID_AUTO, rpm_stats,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0,
    pm_sysctl_rpm_stats, "A", "The RPM's sleep counts, the MPM's hand-overs");
SYSCTL_PROC(_hw_pro1x_pm, OID_AUTO, mpm,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0, pm_sysctl_mpm, "I",
    "Hand the MPM to the RPM when every CPU is down");
SYSCTL_PROC(_hw_pro1x_pm, OID_AUTO, mpm_pins,
    CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0, pm_sysctl_pins,
    "A", "The MPM's wake pins, pin:type (h l r f b)");
SYSCTL_INT(_hw_pro1x_pm, OID_AUTO, debug, CTLFLAG_RWTUN, &pm_debug, 0,
    "Each freeze on the console");
SYSCTL_INT(_hw_pro1x_pm, OID_AUTO, epoch, CTLFLAG_RW, &pm.epoch, 0,
    "Seconds between the suspend's wake-ups (the watchdog's feeding)");
SYSCTL_PROC(_hw_pro1x_pm, OID_AUTO, suspend,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0, pm_sysctl_suspend,
    "I", "Suspend to idle for N seconds (0: until a device interrupt)");
SYSCTL_STRING(_hw_pro1x_pm, OID_AUTO, last, CTLFLAG_RD, pm.last, 0,
    "The last suspend");
SYSCTL_PROC(_hw_pro1x_pm, OID_AUTO, usb_probe,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0,
    pm_sysctl_usb_probe, "A", "The USB controller's clocks, power, registers");

/* The device tree's MPM, its message RAM and doorbell, the statistics */
static void
pm_setup(void)
{
	phandle_t root, mpm, ram, apcs;
	pcell_t cells[2], pins;
	bus_addr_t pa;
	bus_size_t size;

	root = OF_finddevice("/");
	mpm = ofw_bus_find_compatible(root, "qcom,mpm");
	if (mpm > 0 &&
	    OF_getencprop(mpm, "qcom,rpm-msg-ram", cells, sizeof(cells[0])) > 0 &&
	    (ram = OF_node_from_xref(cells[0])) > 0 &&
	    ofw_reg_to_paddr(ram, 0, &pa, &size, NULL) == 0 &&
	    OF_getencprop(mpm, "qcom,mpm-pin-count", &pins, sizeof(pins)) > 0 &&
	    pins > 0 && pins <= MPM_BANKS * 32 &&
	    size >= 4 * (2 + 5 * howmany(pins, 32))) {
		pm.pins = pins;
		pm.banks = howmany(pins, 32);
		pm.vmpm_pa = pa;
		pm.vmpm_size = size;
		pm.vmpm = pmap_mapdev(pa, size);
	} else
		printf("pro1x_pm: no MPM (qcom,mpm with its message RAM)\n");
	/* the doorbell: mboxes = <&apcs_glb bit>, the IPC register at 8 */
	if (mpm > 0 &&
	    OF_getencprop(mpm, "mboxes", cells, sizeof(cells)) == sizeof(cells) &&
	    (apcs = OF_node_from_xref(cells[0])) > 0 &&
	    ofw_bus_node_is_compatible(apcs, "qcom,msm8994-apcs-kpss-global") &&
	    ofw_reg_to_paddr(apcs, 0, &pa, &size, NULL) == 0 && size >= 12) {
		pm.ipc_pa = pa + 8;
		pm.ipc_bit = cells[1];
		pm.ipc = (volatile uint32_t *)((char *)pmap_mapdev(pa, size) + 8);
	} else if (mpm > 0)
		printf("pro1x_pm: the MPM's doorbell not found\n");
	ram = ofw_bus_find_compatible(root, "qcom,rpm-stats");
	if (ram > 0 && ofw_reg_to_paddr(ram, 0, &pa, &size, NULL) == 0 &&
	    size >= RPM_STATS_OFFSET + 4 && size <= 0x10000) {
		pm.stats_size = size;
		pm.stats = pmap_mapdev(pa, size);
	}
	pm_parse_pins(pm.pins_str);
	printf("pro1x_pm: vMPM %s%#jx (%u pins), doorbell %#jx bit %u, RPM "
	    "statistics %s; the hand-over %s, wake pins %s\n",
	    pm.vmpm != NULL ? "at " : "none ", (uintmax_t)pm.vmpm_pa, pm.pins,
	    (uintmax_t)pm.ipc_pa, pm.ipc_bit, pm.stats != NULL ? "yes" : "none",
	    pm.mpm_on ? "on" : "off (hw.pro1x_pm.mpm=1)", pm.pins_str);
}

static int
pm_modevent(module_t mod, int what, void *arg)
{
	switch (what) {
	case MOD_LOAD:
		sx_init(&pm.lock, "pro1x_pm");
		if (!ofw_bus_node_is_compatible(OF_finddevice("/"),
		    "qcom,sm6115")) {
			printf("pro1x_pm: not an SM6115: only the suspend\n");
		} else
			pm_setup();
		psci_idle_soc_hook = pm_soc_hook;
		return (0);
	case MOD_UNLOAD:
		psci_idle_soc_hook = NULL;
		pm.mpm_on = 0;
		/* a CPU may be in the hook right now */
		DELAY(10000);
		if (pm.vmpm != NULL)
			pmap_unmapdev(__DEVOLATILE(void *, pm.vmpm), pm.vmpm_size);
		sx_destroy(&pm.lock);
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t pm_mod = {
	"pro1x_pm",
	pm_modevent,
	NULL
};
DECLARE_MODULE(pro1x_pm, pm_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(pro1x_pm, 1);
