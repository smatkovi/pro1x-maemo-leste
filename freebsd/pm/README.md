# FreeBSD power management on the Pro1X: SMP, CPU idle, sleep, USB

FreeBSD on the phone ran on one CPU with `wfi` as its only idle state.
This directory adds, in stages:

| Stage | What | State |
| --- | --- | --- |
| 1 | SMP: all eight CPUs (PSCI CPU_ON) | FreeBSD's own code; checked against the device tree, tried in QEMU (8 CPUs); device run prepared |
| 2 | CPU idle: the PSCI idle states of the device tree, OS-initiated mode, the core's context across a power-down, the memory-mapped timer as the wake-up | built, tried in QEMU with every power-down played (`simulate`); device runs prepared |
| 3 | the SoC's sleep (RPM sleep set, MPM hand-over) and suspend-to-idle | built; the freeze tried in QEMU (no RPM there); device runs prepared |
| 4 | USB device mode (CDC ethernet to the PC) | dwc3dci (a DWC_usb3 device controller driver) and the SM6115 glue built, the encodings tested on the PC; a probe and the gadget run prepared |
| 4b | the USB-C port | the Type-C detection (`pro1x_typec`, by its interrupts) built, its decoding tested |
| 4c | host mode, USB 3, the role switch | xHCI on the DWC3, the QMP USB3 PHY (Linux's tables, checked), devd role switch: built, untested; SuperSpeed in device mode not done |
| 5 | the PMIC RTC's alarm as the suspend's wake-up, alarmd's alarms through it | built; the registers tested on the PC against Linux's rtc-pm8xxx, the wake-up in QEMU (a PL031 in the PMIC's place), alarmd's side in the VM; device runs prepared |
| 6 | energy: the suspend while nobody uses the phone (`pro1x-autosleep`), the power profile (`pro1x.power=on`), the jack without polling, Wi-Fi's power save, fewer wake-ups | built; the policy host tested, the daemon in the VM, wake-ups measured in QEMU and the VM; device runs prepared |

Nothing here has run on the phone yet (it is unreachable until
2026-10-31). Everything is off by default: the phone's normal start is
unchanged until the device runs below pass.

## Files

| File | |
| --- | --- |
| `sys/arm64/qualcomm/pm/psci_idle.c` | the idle states from the device tree, the choice per idle, OS-initiated mode, statistics (`hw.psci_idle.*`) |
| `sys/arm64/qualcomm/pm/psci_idle_asm.S` | the core's context across CPU_SUSPEND, the warm boot entry `psci_idle_resume` |
| `sys/arm64/qualcomm/pm/arm_tmr_mem.c` | `arm,armv7-timer-mem`: the frame that wakes CPUs whose own timer stops |
| `sys/arm64/qualcomm/pm/pro1x_pm.c` | `device pro1x_pm`: the MPM's hand-over to the RPM, the RPM's counts, suspend-to-idle (`hw.pro1x_pm.*`) |
| `sys/arm64/qualcomm/pm/pro1x_rpmsleep.c`, `sys/modules/pro1x_rpmsleep` | `pro1x_rpmsleep.ko`: the apps processor's RPM sleep set at 0, through `qcom_dsp.ko` |
| `../patches/0033-files.arm64-pro1x_pm.patch` | the file for `device pro1x_pm` |
| `sys/dev/usb/controller/dwc3dci*.{c,h}`, `sys/modules/pro1x_usb` | `pro1x_usb.ko`: the DWC_usb3 device controller driver and its SM6115 glue (stage 4) |
| `../patches/0034-usb-usbus-on-dwc3dci.patch` | `usbus` on `dwc3dci` |
| `../patches/0035-qcom_rtc-alarm.patch` | `qcom_rtc`'s alarm (`dev.qcom_rtc.0.wakealarm`), `power_wakeup` (`qcom_spmi.h`) from it and from `qcom_pon`'s power key (stage 5) |
| `sys/dev/qcom_spmi/qcom_rtc_alarm.h` | the RTC alarm's registers, the conversion from the real time, the register sequences (Linux's `rtc-pm8xxx.c`) |
| `sys/dev/qcom_spmi/qcom_rtc_sim.c` | QEMU's stand-in (`device qcom_rtc_sim`, PRO1XQ only): `qcom_rtc.c` on QEMU virt's PL031 |
| `test/qcom_rtc_test.c` | the host test of `qcom_rtc_alarm.h` against Linux's `rtc-pm8xxx` and Leste's `librtc-realtime` |
| `test/dwc3dci_test.c` | the host test of `dwc3dci_hw.h` |
| `sys/dev/qcom_spmi/pro1x_typec.{c,h}`, `sys/modules/pro1x_typec` | `pro1x_typec.ko`: the PMI632's Type-C detection (stage 4b); `test/typec_test.c` its host test |
| `sys/dev/usb/controller/dwc3dci_host.c`, `dwc3dci_qmp.h` | host mode (xHCI), the QMP USB3 PHY (stage 4c); `test/qmp_test.c` |
| `../patches/0032-arm64-cpu_idle-hook-psci-idle.patch` | `cpu_idle()` asks `arm64_cpu_idle_hook` first; the files for `device psci_idle` |
| `../mfsroot/etc/pro1x-pmtest.sh` | the device tests in the RAM root (`pro1x.test=smp`, `idle`), sourced by `mfsroot/etc/rc` |
| `build.sh` | the kernels: `qemu` (PRO1XQ) and `phone` (PRO1XPM = `freebsd/PRO1X` with `device psci_idle`) into `$WORK/pm-out` (`PMOUT=`: elsewhere; `LOCKNAME=`: the name in `$WORK/.kbuild-lock`) |
| `tools/pro1x-suspend` | the Pro1X's `zzz`: panel, touch and keyboard light off, the sleep set and the hand-over, the next alarm as the RTC's alarm, suspend until the power key, the alarm or N seconds, everything back (for the root image's `/usr/local/sbin`) |
| `test/qemu/` | PRO1XQ, its device tree (`mkdtb.py`: QEMU's virt with the SM6115's idle topology), its rc (`pm.wake=N`: the wake-ups idle and suspended), `run.sh` |
| `tools/pro1x-autosleep/` | the suspend while nobody uses the phone (stage 6): `policy.c` decides, `pro1x-autosleep.c` looks and runs `pro1x-suspend -k`; `test/autosleep_test.c` the policy's host test, `test/vm-autosleep.sh` the daemon in the VM |
| `tools/pro1x-drain` | the battery's drain over an hour with the RPM's and autosleep's counts (stage 6's device runs) |
| `test/vm-wakeups.sh` | a Hildon session's idle wake-ups in the VM (DTrace: per process, the kernel's timers, interrupts) |
| `../patches/0036-*`, `0037-*` | `pro1x.power=on`: the profile's kernel settings; LinuxKPI 802.11: the driver's power save (`ifconfig wlan0 powersave`) |
| `../rootfs/usr/local/etc/rc.d/pro1x_autosleep`, `pro1x_power`, `../rootfs/usr/local/share/pro1x/power.subr` | the profile in the full system |

`freebsd/build.sh` copies `sys/` into the sources like `dsp/` and `fm/`;
`freebsd/PRO1X` has `device psci_idle` and `device pro1x_pm` (inert
while `hw.psci_idle.enable=0`, `hw.pro1x_pm.mpm=0` and no suspend is
asked for). `pro1x_rpmsleep` is a module (it needs `qcom_dsp.ko`): for
the root image, `MODULES` in `freebsd/build.sh` would take it.

## Stage 1: SMP

FreeBSD's arm64 SMP is generic and needs nothing Pro1X-specific, which is
why it was only switched off (`kern.smp.disabled=1` in `pro1x.env`), not
missing. What it uses, against the phone's device tree
(`sm6115.dtsi`, Linux 7.1):

- **Bring-up:** `enable-method = "psci"` on all eight CPU nodes, PSCI 1.0
  over `smc`. `cpu_mp_start` calls CPU_ON with `mpentry_psci`'s physical
  address for each CPU node (`reg` 0x0-0x3: silver, cluster 0; 0x100-0x103:
  gold, cluster 1). Linux's kexec takes the other CPUs down with CPU_OFF
  before it jumps (arm64 kexec requires it), so they are off when
  FreeBSD starts; `psci_idle` asks AFFINITY_INFO for each before the APs
  start and prints it (`off at boot: 0xfe` expected).
- **GIC:** GICv3, one redistributor region at 0xf300000, stride 0x20000:
  FreeBSD walks GICR_TYPER for each CPU's affinity, which matches the
  default stride of a GICv3 without VLPIs.
- **Timer:** the generic timer's per-CPU PPIs (`arm,armv8-timer`, the
  virtual one at EL1 as FreeBSD picks it without a hypervisor of its own).
- **Frequency:** `qcom_cpufreq_hw` (patches/0010) takes each CPU's
  `qcom,freq-domain`: domain 0 for CPUs 0-3, 1 for 4-7.
- **Interrupts:** FreeBSD spreads the SPIs over all CPUs at SI_SUB_SMP
  (`intr_irq_shuffle`): the drivers' interrupts then come on other CPUs
  than 0, which the device run shows for the first time.

What may go wrong on the phone, which QEMU cannot show: Qualcomm's
hypervisor or TZ refusing CPU_ON (FreeBSD prints "Failed to start CPU N"
and goes on with fewer), a CPU that starts but hangs before it reports
(FreeBSD waits for each and moves on; the watchdog resets the phone if the
boot hangs), and drivers that never ran with their interrupt on another
CPU.

**QEMU** (`test/qemu/run.sh smptest "pro1x.test=smp"`, 8 Cortex-A53,
2026-10-30): 8 CPUs, `off at boot: 0xfe`, the busy loop on every CPU at
92-99 %, the work check (below) 8 x 81 rounds without a wrong result.

## Stage 2: CPU idle through PSCI

FreeBSD/arm64 has no cpuidle framework: `cpu_idle()` executes `wfi`.
`patches/0032` lets it ask `arm64_cpu_idle_hook` first, with the time to
the CPU's next event (`cpu_idleclock()`); `psci_idle.c` is that hook.

**The states, from the device tree** (as Linux's `cpuidle-psci` and
`cpuidle-psci-domain` read them): each CPU's power domain (`power-domains`,
named "psci") has the core's state (`domain-idle-states`), its parent
domain the cluster's. On the Pro1X (`sm6115.dtsi`, the same numbers
KolibriOS took for its PSCI idle, `docs/power.md` there):

| | parameter | min. residency | exit latency |
| --- | --- | --- | --- |
| silver core: rail power collapse | 0x40000003 | 1182 us | 376 us |
| gold core: rail power collapse | 0x40000003 | 1110 us | 324 us |
| cluster 0: GDHS / power collapse | 0x40000022 / 0x41000044 | 782 / 7376 us | 421 / 2118 us |
| cluster 1: GDHS / power collapse | 0x40000042 / 0x41000044 | 660 / 8094 us | 345 / 1654 us |

All of them are power-down states (bit 30 in PSCI 1.0's extended state
ids) and the core states have `local-timer-stop`.

**The choice** per idle: the deepest core state whose minimum residency
fits the time to the next event; the cluster's state when this CPU is the
last online CPU of its cluster in a power-down state, the deepest fitting
one, whose parameter replaces the core's (Linux: the domain state wins).
This needs OS-initiated mode (PSCI_FEATURES(CPU_SUSPEND) bit 0,
SET_SUSPEND_MODE before the APs start, as Linux's cpuidle-psci-domain
sets it); the firmware refuses a cluster state (DENIED) when another core
of the cluster is in fact running. After 1000 refusals in a row a cluster
uses its cores' states only, after 100 failures a state is not used any
more; both are printed.

**The context** (`psci_idle_asm.S`, after Linux's `sleep.S`/`cpu_do_resume`
and KolibriOS's `suspend.S`): the callee-saved registers with x18 (the
per-CPU pointer), the stack pointer, the EL1 system registers FreeBSD
sets (translation, vectors, CPACR, CNTKCTL, the thread pointers,
CONTEXTIDR, MDSCR, the virtual timer, the GIC's CPU interface), cleaned
to the point of coherency; CPU_SUSPEND with `psci_idle_resume`'s physical
address and the context's as the context id. FreeBSD's kernel does not
run at its physical addresses, so the way back needs an identity map:
`psci_idle.c` builds one (a 1 GiB block with the code); the resume entry
loads MAIR, TCR, TTBR1, the identity map into TTBR0, turns the MMU on
under itself, jumps to the kernel's address and loads the rest. The OS
lock is opened as FreeBSD's `dbg_init` does. The FP/SIMD registers are
not kept: the last thread that used them had them saved at its switch,
and `fpcurthread` is cleared, so they are loaded again at its next use.
Refused (wfi only, printed): a kernel at EL2, pointer authentication (its
keys), LPA2 translation; none of them on the Pro1X's ARMv8.0 cores.

**The wake-up** (`arm_tmr_mem.c`): the cores' timer stops in these states.
The SM6115 has the memory-mapped timer at 0xf120000 (CNTCTLBase), frame 0
at 0xf121000 with its physical timer on SPI 8 and its virtual one on
SPI 7 (frames 1-6 disabled), 19.2 MHz; Linux uses it the same way for its
tick broadcast (`arch_mem_timer`, 34 interrupts a second in
`docs/power-management.md`). As Linux's `arm_arch_timer_mmio.c`: CNTACR
opened, a virtual frame preferred, else the physical. One frame for all
CPUs: each CPU going into such a state leaves its deadline, the frame is
set to the earliest; its interrupt sends the CPUs whose deadline has come
an IPI_AST, which only wakes them (`cpu_activeclock()` then runs what is
due). Without the frame these states are not used.

**Settings** (kernel environment, `freebsd/cmdline`; also sysctls):

| | |
| --- | --- |
| `hw.psci_idle.enable=1` | use the states (default 0: wfi only, as before) |
| `hw.psci_idle.depth=N` | 0 wfi, 1 the cores' states, 2 also the clusters' GDHS, 3 (or more) all |
| `hw.psci_idle.osi=0` | platform-coordinated mode: the cores' states only |
| `hw.psci_idle.pc_cluster=1` | the clusters' states also without OSI (QEMU) |
| `hw.psci_idle.simulate=1` | QEMU: each power-down played after CPU_SUSPEND (below) |
| `sysctl hw.psci_idle.stats` | per CPU: entries and time per state, refusals, failures, returns through `psci_idle_resume`; the frame's interrupts |
| `sysctl hw.psci_idle.mode` | the firmware's version, conduit, mode, the CPUs off at boot |

**QEMU** (`test/qemu/run.sh`, 2026-10-30): QEMU's CPU_SUSPEND is a `wfi`
and takes only PSCI 0.2's state format, so `mkdtb.py` gives QEMU's tree
the phone's hierarchy with parameters in that format (0x10003, ...), and
`simulate` destroys the core's state after each successful CPU_SUSPEND as
a power-down would (MMU and caches off through the identity map, the
system registers, the timer, the GIC's CPU interface, the FP/SIMD
registers and the stack pointer cleared) and takes the way back through
`psci_idle_resume`. The work check: 8 workers decompress xz into md5 and
compare with the reference, a short sleep between rounds, so that the
CPUs keep entering and leaving the states with user FP/SIMD state live
(libc's memcpy and string functions use the SIMD registers).

| run | result |
| --- | --- |
| `core`: depth 1 | 4349 power-downs on the 8 CPUs, every one back through `psci_idle_resume`, 8 x 70 rounds without a wrong result |
| `cluster`: `pc_cluster=1` | 3821 power-downs, of them the clusters' power collapse 914 times (cluster 0) and 1228 times (cluster 1) by the cluster's last CPU, all back, no wrong result |
| `negfp`: as `core`, `fpcurthread` not cleared | init dies at once ("Going nowhere without my init!"): the check does see lost FP state |
| `idletest`: `pro1x.test=idle` of the device test | as `cluster` |

Not testable in QEMU: the firmware's real power-down and warm boot,
OS-initiated mode (QEMU has none), the memory-mapped timer (QEMU's virt
has none).

## Stage 3: the SoC's sleep and suspend-to-idle

The SM6115 is RPM-based: its sleep states (vlow: XO shutdown; vmin: CX
and MX at retention too) are the RPM's to enter, when every master (the
apps processor, the modem, the ADSP, the CDSP) sleeps and no vote in a
sleep set holds a resource up. While the SoC sleeps the GIC is off; the
MPM, in the always-on domain, wakes it by its timer or one of its pins
(`docs/power-management.md`, KolibriOS's `docs/power.md` stage 3).

**The sleep set** (`pro1x_rpmsleep.ko`, `sysctl hw.pro1x_rpmsleep.set=1`):
after a kexec the apps processor's sleep set is what Linux left (its
consumers' votes and the boot's maxima). The module sends what Linux
sends when nothing uses a resource, 0 in the sleep set, through the RPM
channel of `freebsd/dsp`'s `qcom_dsp.ko` (`qcom_rpm_write`); the table is
KolibriOS's `rpmsleep.c`, from Linux 7.1's `clk-smd-rpm.c` and `rpmpd.c`:
the XO (`clk0` 0 `Enab`), its buffers (`clka` 1-3, with `=2` also the RF
buffers 4-5 the Wi-Fi uses; `swen`), CX and MX (`rwcx`, `rwmx` 0
`vlvl`), the bus clocks (`clk2` 0 bimc, `clk1` 1 cnoc and 2 snoc,
`mmxi` 0-1, `qup` 0; `KHz`). The active set stays; the votes last until
the next boot.

**The MPM's hand-over** (`pro1x_pm`, built in, `hw.pro1x_pm.mpm=1`, off by
default as Linux's `notify_rpm`): when every online CPU is in a
power-down state and the last one goes into its cluster's deepest state
(`psci_idle_soc_hook`), as `device/kernel/patches/0011` does it in Linux:
the earliest wake-up of all CPUs as an arch timer count into the vMPM's
TIMER0/TIMER1, the wake pins' enables and types, the status cleared, the
RPM's doorbell rung. From the device tree: the vMPM at 0x45f01b8
(`apss_mpm`, 0x48 bytes in the RPM's message RAM at 0x45f0000), 96 pins
in 3 banks (Linux's `irq-qcom-mpm.c`: TIMER0/1, then ENABLE,
FALLING_EDGE, RISING_EDGE, POLARITY, STATUS, a word per bank each), the
doorbell `mboxes = <&apcs_glb 1>`: bit 1 of the IPC register at
0xf111000 + 8 (`qcom,msm8994-apcs-kpss-global`, Linux's
`qcom-apcs-ipc-mailbox.c`). The wake pins: `hw.pro1x_pm.mpm_pins`,
default `86:h`, the SPMI arbiter (power and volume keys, the PMIC's RTC)
as the device tree routes it (`interrupts-extended = <&mpm 86
IRQ_TYPE_LEVEL_HIGH>`, `qcom,mpm-pin-map <86 183>`: GIC SPI 183, INTID
215). The gpio-keys have pins too (Linux's `sm6115_mpm_map`: GPIO 93
Home 46, 83 right Shift 9, 84 left Ctrl 39, 19 left Alt 53: `"86:h 46:b
9:b 39:b 53:b"`), the keyboard's GPIOs 106, 107 and 112 have none.
`sysctl hw.pro1x_pm.rpm_stats`: the RPM's counts (`qcom,rpm-stats` at
0x4690000, Linux's `qcom_stats.c`: the records' offset in the word at
0x14, two records of 48 bytes, the accumulated time in 19.2 MHz counts),
the hand-overs, the pins seen in the MPM's status.

**Suspend-to-idle** (`sysctl hw.pro1x_pm.suspend=N`, N seconds, 0 until
a device interrupt): FreeBSD/arm64 has no suspend at all
(`acpi_sleep_machdep` is a stub, no `zzz` backend, no IPI_SUSPEND). This
is the s2idle idea on top of `psci_idle`: the `power_suspend` handlers,
then `psci_idle_freeze`: every other CPU stays in its deepest state with
its own timer masked, so that its callouts wait (an IPI or an interrupt
wakes it, is handled as usual, and it goes back); the calling thread,
bound to CPU 0, does the same until its deadline (its own timer and the
broadcast frame). A device interrupt (the power key: PON through the
SPMI arbiter, INTID 215) ends it, on whichever CPU it comes: that CPU
tells CPU 0 by an IPI; the RTC alarm (stage 5) comes the same way.
The suspend runs in epochs of
`hw.pro1x_pm.epoch` seconds (5): between them the callouts run 50 ms,
which feeds the watchdog (the boot menu arms it to bite about 32 s after
a feed, `qcom_wdt` feeds it from a callout every 15 s with 5 s of slack,
`patches/0023`; a freeze that starts just before a feed delays the feed
by the freeze's length, hence 5 s and not more). With `hw.pro1x_pm.mpm=1` and the sleep set the SoC can
reach vlow/vmin while frozen. `hw.pro1x_pm.last` says how long, what
ended it (the deadline, or the INTID), the power-downs and wake-ups of
CPU 0, how many CPUs froze, and the RPM's counts; `hw.pro1x_pm.debug=1`
prints each freeze on the console. The panel, the touch
screen and the charger input are quiesced from user space beforehand
(`patches/0022`, `0028`, `0030`; the device test's `pm_quiet`).

The PMIC's RTC alarm as a wake-up: stage 5. Not done:
`DEVICE_SUSPEND` of the device tree (no Pro1X driver has suspend
methods). The modem, ADSP and CDSP run their own firmware: whether they
let the SoC sleep shows in the RPM's counts.

**QEMU** (2026-10-30; QEMU has no MPM or RPM, so only the freeze): `run.sh
susp "... pm.suspend=40 pm.epoch=15"`: suspended 40.0 s in three
freezes, 3 power-downs of CPU 0 (one per freeze), the other 7 CPUs
frozen within 300 us, each woken once per freeze; the work check
afterwards without a wrong result (with the epochs at 15 s; at the
default 5 s: 30 s in 6 freezes, twice in a row, the same). Four
suspends of 40 s in a row
(`pm.suspend_n=4`), the same. The device test `pro1x.test=suspend` (A,
B, D with `pro1x.power_min=2` and `5`, no MPM or RPM in QEMU): three
runs through, 16 and 24 freezes, every one ended by its deadline with
all 7 other CPUs frozen. One earlier run of it stopped in its fourth
suspend (the first of D) without any message and was killed by the
time-out 3 minutes later; not seen again in those runs, cause not
found. On the phone such a stop ends in the watchdog's reset (it is not
fed while frozen); `hw.pro1x_pm.debug=1` shows which freeze it was.

## Stage 4: USB device mode (analysis, the first device run)

Goal: the PC reaches FreeBSD over the cable as it reaches Leste (a CDC
Ethernet device on the PC, the phone at 172.16.42.1). What is there and
what is missing:

- **FreeBSD 15.1's DWC3 driver is host only** (`dev/usb/controller/dwc3/
  dwc3.c`: "only 'host' supported", it hands the core to `xhci`). FreeBSD's
  USB stack has a device side with templates (`dev/usb/template/`:
  `usb_template_cdce.c` CDC Ethernet, `usb_template_serialnet.c`
  Ethernet plus serial, chosen by `hw.usb.template`), but only with
  controller drivers that do device mode: `dwc_otg` (DWC2), `musb`,
  `atmegadci`, `avr32dci`, `uss820dci`. A DWC3 device-mode controller
  driver for that stack does not exist: it has to be written (the DWC3's
  event buffer, EP0's control transfers, the TRB rings of the bulk and
  interrupt endpoints CDC Ethernet needs, reset, connect-done and
  disconnect events; Linux's `dwc3/gadget.c` and `ep0.c` as the model),
  with the glue `dwc_otg.c` has to the stack's pipe methods.
- **The platform around it**, from the device tree (`sm6115.dtsi`, the
  Pro1X's dts) and Linux 7.1: the Qualcomm wrapper at 0x4ef8800 (its
  clocks, interrupts `pwr_event` SPI 302, `qusb2_phy` 260, `hs_phy_irq`
  254, `ss_phy_irq` 422), the DWC3 core at 0x4e00000 (SPI 255) behind the
  apps SMMU (stream 0x120: FreeBSD gives physical addresses, so the stream
  needs an identity mapping, as the boot menu does it for UFS), the QUSB2
  PHY at 0x1613000 (`phy-qcom-qusb2.c`'s `sm6115_init_tbl`, its HSTX trim
  from the fuse `qusb2_hstx_trim`; supplies L4A, L12A, L15A through the
  RPM, which `freebsd/dsp`'s `qcom_rpm` can vote), the GCC (no FreeBSD
  driver for the SM6115's: `gcc-sm6115.c`'s offsets: the GDSC at
  0x1a004, the BCR 0x1a000, the master clock 0x1a010 with its RCG at
  0x1a01c (66.67 MHz: GPLL0_OUT_AUX2 / 4.5), sleep 0x1a014, mock UTMI
  0x1a018 (RCG 0x1a034, 19.2 MHz), the NoC clocks 0x1a080/0x1a084, the
  reference clock 0x9f000, the PHY's AHB clock 0x1d008, its BCR
  0x1c000). The role: device mode can be forced in the core (GCTL's
  port capability) without the PMI632's Type-C role switch.
- **What Linux leaves at the kexec** decides the order: the README says
  Linux takes the controller down; how far (the GDSC, the clocks, the
  PHY) is what the first run shows. If the boot menu could leave it
  powered (as `device/kernel/patches/0004` does for UFS and 0005 for the
  display) with the SMMU stream on an identity mapping, FreeBSD would
  only need the core driver; otherwise the GCC, GDSC and PHY bring-up
  comes first.

**Done:**

- `hw.pro1x_pm.usb_probe` (`pro1x_pm`): the GCC's USB registers above
  (always readable), and only when the GDSC is on, the master and bus
  clocks run and the reset is released, the DWC3's GSNPSID, GCTL, DCFG,
  DCTL and DSTS (an unclocked block's read can hang the bus).
- **dwc3dci** (`sys/dev/usb/controller/dwc3dci.c`, `pro1x_usb.ko`): a
  DWC_usb3 device controller driver for FreeBSD's USB device side. The
  framework (transfer descriptor chains, the emulated root hub with its
  one device-mode port, the transfer setup) is `atmegadci.c`'s, taken as
  it is; the hardware part follows Linux's `dwc3/gadget.c` and `ep0.c`:
  the event buffer (reset, connect-done, disconnect; XferNotReady of the
  control status stage), one TRB per transfer on an uncached bounce
  buffer per physical endpoint (16 KiB, 4 KiB for EP0), the control
  endpoint as SETUP, DATA, STATUS2/STATUS3 TRBs, SET_ADDRESS into DCFG at
  the setup stage, the other endpoints configured (STARTCFG 2 once,
  DEPCFG, XFERCFG, DALEPENA) when the stack resets them at
  SET_CONFIGURATION, stalls by SETSTALL/CLEARSTALL, cancelled transfers
  by ENDXFER. High speed only (no USB3 PHY), bulk and interrupt
  endpoints, no isochronous. `patches/0034`: `usbus` attaches to it.
- **The SM6115 glue** (`dwc3dci_pro1x.c`): attaches to the wrapper
  (`qcom,sm6115-dwc3`) only with `hw.dwc3dci.enable=1`; the PHY's
  supplies on through the RPM and stream 0x120 untranslated
  (`qcom_dsp.ko`'s `qcom_rpm_regulator`, `qcom_smmu_bypass`), the GDSC
  and the seven clocks on where they are off, the master RCG at
  66.67 MHz, the block reset pulsed, the QUSB2 PHY initialised as
  Linux's `qusb2_phy_init` (its reset, `sm6115_init_tbl`, the
  single-ended reference clock, PLL lock checked; the HSTX trim fuse is
  not applied: the table's TUNE2 0x53 stays), the wrapper set to UTMI as
  the PIPE clock and VBUS valid. It does this whatever Linux left (the
  reset makes a running gadget of Linux's stop its DMA), and says what it
  found in `dev.dwc3dci.0.bringup`; `dev.dwc3dci.0.stats` counts events,
  resets, connections, setups, TRBs and command errors.
- **Function:** FreeBSD's own: `usb_template.ko` with `hw.usb.template=1`
  (CDC Ethernet), `if_cdce.ko` and `uether.ko` in device mode, the
  interface `ue0`. `freebsd/build.sh`'s `MODULES` has `pro1x_usb`,
  `usb/template`, `usb/uether`, `usb/cdce` (none loaded unless asked).
- **Tested on the PC:** the build (kernel PRO1XPM and the modules); the
  encodings of `dwc3dci_hw.h` (DEPCFG parameters as Linux's dwc3 trace
  prints them for a high-speed gadget, TRB words, completion lengths,
  whole-packet OUT lengths, the device and endpoint event fields,
  command words) against expected values: `test/dwc3dci_test.c`, 0
  failures. Not tested: the driver against hardware or a model (QEMU has
  no DWC3 device side); the control state machine, the PHY and clock
  bring-up and the SMMU stream are first tried on the phone.
- **The full system** (`freebsd/rootfs/usr/local/etc/rc.d/pro1x_usbnet`,
  in the root image): off by default (the driver is untested), on with
  any of `pro1x.usb=1` in `freebsd/cmdline`, a file `freebsd/usb-net` next
  to the kernel on userdata (seen while userdata is mounted below
  `/dev/pro1x-host`, i.e. with the read-only `root.zfs`), or
  `pro1x_usbnet_enable="YES"` in `rc.conf`. It loads `qcom_dsp`, `usb`,
  `usb_template`, `uether`, `if_cdce`, sets `hw.usb.template=1` and
  `hw.dwc3dci.enable=1`, loads `pro1x_usb`, waits up to 30 s for `ue0`,
  gives it 172.16.42.1/24 and starts an sshd of its own listening on that
  address only, public keys only (`PasswordAuthentication no`,
  `KbdInteractiveAuthentication no`, `PermitRootLogin prohibit-password`),
  the keys from `/usr/local/etc/ssh/pro1x_authorized_keys`: `rootfs.sh`
  puts the PC user's `SSH_PUBKEY` there (default `~/.ssh/id_ed25519.pub`,
  the convention of `scripts/40-make-rootfs.sh` for Leste; only a `.pub`
  file, never a private key). Host keys: `/etc/ssh`'s on the writable
  root, else made at each start in `/var/run/pro1x-sshd` (the read-only
  root: a new host key each boot). Use a known hosts file of its own for
  the phone (`-o UserKnownHostsFile=~/.ssh/known_hosts.pro1x-freebsd`),
  so a changed key on the writable root still shows; on the read-only
  root, delete that file when the key changed after a reboot.
  `service pro1x_usbnet status` shows the bring-up, the counters, `ue0`.
- **Serial console:** `pro1x.usb=serial` (or `pro1x_usbnet_serial="YES"`):
  `usb_template` 8 (CDC Ethernet and a modem port), `ucom` and `umodem`
  too, and a getty on `/dev/ttyU0` (kept running by `daemon -r`); on the
  PC `/dev/ttyACM0`, 115200 baud.
- **Not done:** the HSTX trim, USB3, the PMI632's VBUS and Type-C
  detection (VBUS is forced valid).

**Device run:**

9. **USB probe** (`usb`), seconds, read only:

       pro1x.test=usb hint.qcom_wdt.0.lifetime=120

   Expected: `hw.pro1x_pm.usb_probe: USB30_PRIM_BCR ..., USB30_PRIM_GDSCR
   ..., ...; GDSC on|off, master clock on|off, its bus clock on|off,
   reset asserted|released` and either the DWC3's registers (GSNPSID
   0x5533xxxx for a DWC_usb3) or `not read (not clocked)`. That is the
   input for the next step; nothing is changed. For the power: `GDSC on`,
   `master clock on` with no cable and no gadget means Linux left the
   block running at the kexec and FreeBSD keeps it so (nothing switches
   it off yet): note it for a power-down at boot (the GDSC collapsed, the
   clocks off, the QUSB2 PHY powered down, L15A's vote off; never L4A or
   L12A, the UFS PHY's too).

10. **USB gadget, CDC Ethernet** (`usbnet`), the cable to the PC, about
    6 minutes. First, from Leste, `pm-out/modules/{qcom_dsp,usb,
    usb_template,uether,if_cdce,pro1x_usb}.ko` into `/host/freebsd/pm/`.
    Then:

        pro1x.test=usbnet pro1x.usb_min=5 hint.qcom_wdt.0.lifetime=480

    On the PC, once the phone shows up as a CDC Ethernet device (`dmesg`:
    cdc_ether, 16c0:27e1 "USB Ethernet Adapter", FreeBSD's template):
    `ip addr add 172.16.42.2/24 dev <its interface>`, `ip link set ...
    up`, `ping 172.16.42.1`. Expected on the console:
    `dwc3dci0: bring-up: SMMU stream 0x120 untranslated, GDSC ...,
    N of 7 clocks switched on, ..., QUSB2 PLL locked, UTMI as PIPE clock,
    VBUS valid`, `dwc3dci0: DWC_usb3 5533xxxx in device mode, high
    speed`, `usbus0 on dwc3dci0`, `dwc3dci0: connected (run)`, then with
    the cable: `stats: events N, resets 1 or more, connects 1 (high
    speed), setups N, ...`, `cdce0`/`ue0` attaching, `ue0 up as
    172.16.42.1`, and `172.16.42.2 answers` once the PC has its address.
    Failure signs: `bring-up: ... does not come on` or `QUSB2 PLL not
    locked` (the power or the PHY: the probe of run 9 shows the state;
    a supply the RPM refused is named); `not a DWC_usb3` (the core not
    clocked or not out of reset); a reset of the phone right after
    `bring-up` (a read of an unclocked block hung the bus); `resets 0`
    with the cable in (the pull-up/VBUS or the PHY); `resets` growing,
    `setups 0` (EP0's SETUP TRB never completes: the event buffer or the
    SMMU stream: the core's DMA does not reach memory); setups but no
    `ue0` (the control stage machine: the stack's descriptors not sent,
    the PC's `dmesg` shows "device descriptor read error"); `ue0` but no
    ping (the bulk endpoints).

11. **The full system over the cable** (needs 10; the joint root image with
    `rc.d/pro1x_usbnet`, the modules and the key): in `freebsd/cmdline`
    `pro1x.usb=1` (or, with a serial port, `pro1x.usb=serial`), boot
    FreeBSD from the menu with the cable to the PC. On the PC: the
    address as in 10, then `ssh -o
    UserKnownHostsFile=~/.ssh/known_hosts.pro1x-freebsd root@172.16.42.1`
    (the key of `SSH_PUBKEY`; passwords are refused),
    with the serial port also `screen /dev/ttyACM0 115200`. Expected in
    the system log: `pro1x_usbnet: ...bring-up...`, `USB gadget: ue0
    172.16.42.1, sshd on it`. Failure signs: `no ue0 after 30 s` (as in
    10: `service pro1x_usbnet status` on the console shows the counters);
    `pro1x_authorized_keys is empty: no sshd` (the image was built
    without the PC's public key: `SSH_PUBKEY=... freebsd/rootfs.sh`).
    Afterwards `pro1x.usb=1` out of `cmdline` again until it works.

## Stage 5: the RTC alarm, the alarm clock's wake-up

Goal: an alarm set in the alarm clock (clock-ui, alarmd) wakes the phone
from the suspend and rings.

**How Leste does it.** alarmd (`src/hwrtc.c`, `server.c`'s
`server_rethink_queued`) opens `/dev/rtc0` and sets `RTC_WKALM_SET` to
the next event that may wake the device (`ALARM_EVENT_BOOT` or
`ALARM_EVENT_ACTDEAD`; clock-ui's alarms have `ACTDEAD`), 60 s before it
(`POWERUP_COMPENSATION_SECS`), at least 60 s from now
(`ALARM_INTERRUPT_LIMIT_SECS`), on every look at its queue; dsme is not
involved (alarmd only asks it to power up, reboot or shut down). On the
Pro1X the RTC's count cannot be set, so Leste preloads
`librtc-realtime.so` into alarmd (`tools/pro1x-rtc`), which translates the
alarm's real time by the system clock's difference to the count;
`rtc-pm8xxx` (its offset 0 there) writes it to the alarm registers, and
the alarm's interrupt (`<0 0x61 1 IRQ_TYPE_EDGE_RISING>`, through the SPMI
arbiter, MPM pin 86) wakes the suspend (`docs/power-management.md`,
the notify_rpm test).

**The kernel** (`patches/0035`, `sys/dev/qcom_spmi/qcom_rtc_alarm.h`):
`qcom_rtc` takes the node's second reg (0x6100, the alarm peripheral) and
Linux 7.1's `pm8941_regs`: ALARM_RW at 0x40 (the count it fires at, four
bytes little endian), ALARM_CTRL 0x46 (bit 7 on), ALARM_CTRL2 0x48 (bit 0
the status clear). Setting it is Linux's `pm8xxx_rtc_set_alarm` (off, the
four bytes, on; then read back), switching it off its
`alarm_irq_enable(0)` (off, the count zeroed), its interrupt its
`pm8xxx_alarm_trigger` (off, the status: Linux 7.1 does
`regmap_update_bits(CTRL2, CLEAR, 0)`, a write only when the bit reads 1,
so there the switching off clears it; Linux before 6.3 and Qualcomm's
qpnp-rtc write the bit 1: `hw.qcom_rtc.alarm_clear=1`). The count cannot
be set, the alarm can:

| | |
| --- | --- |
| `dev.qcom_rtc.0.wakealarm` | Linux's `/sys/class/rtc/rtc0/wakealarm`: the alarm's real time (seconds since the epoch, UTC), 0 while it is off; a time not in the future (0) switches it off. Converted as count + (time - now), the count and the system clock read together, as `librtc-realtime` does (`test/qcom_rtc_test.c` checks it gives the same four bytes as Linux's path); at least 2 s, at most 2^31 - 1 s ahead |
| `dev.qcom_rtc.0.alarm` | the counter, the alarm (on at counter N, in S s, its real time) or off, the alarms that fired, the interrupt |
| `dev.qcom_rtc.0.alarms` | the alarms that fired |
| `hw.qcom_rtc.alarm_clear` | 1: the status cleared as qpnp-rtc does (CTRL2 written 1) |

The interrupt (an ithread on the arbiter's, which is INTID 215, SPI 183,
and MPM pin 86 while the SoC sleeps, the power key's too) switches the
alarm off, counts it, prints `qcom_rtc0: alarm: counter N, set for N`,
tells devd (`!system=PMIC subsystem=RTC type=ALARM`) and raises
`power_wakeup` (an event handler declared in `qcom_spmi.h`), which
`qcom_pon` raises for a press of the power key too. An alarm the previous
system left on stays on, as Linux keeps it, and is named at the attach.

**The suspend** (`pro1x_pm`, `psci_idle`): the RTC alarm comes as the
same device interrupt as the power key (INTID 215), which ends a freeze
(`ended by INTID 215 (a device: the RTC alarm)`: which source it was,
from `power_wakeup`, once its interrupt thread has run). `pro1x_pm`
handles `power_wakeup`: a wake-up that comes while the CPUs are awake,
between the 5 s freezes (50 ms of every 5 s), also ends the suspend (`ended by a
wake-up (the RTC alarm)`; before, it was lost and the suspend went on to
its deadline), and kicks a freeze in progress (`psci_idle_freeze`'s new
`stop` flag, looked at after each wake-up). Found in QEMU: when another
CPU takes the device interrupt and wakes CPU 0 by an IPI, CPU 0's highest
pending interrupt (ICC_HPPIR1) showed a PPI or nothing, not the IPI, and
the freeze went back to sleep until the IPI was seen; the freeze now ends
as soon as another CPU has noted the device interrupt. The vMPM side is
the default wake pin `86:h`: the arbiter's line, which carries the RTC
alarm as well as the keys; nothing new is handed over.

**Userland.** alarmd (`../hildon/patches/alarmd/0002`, `0006`, revision
2): `hwrtc_set_alarm` writes the wake-up time to
`dev.qcom_rtc.0.wakealarm` (`sysctlbyname`; alarmd runs as root like the
session; without the sysctl a warning once, the alarm rings while the
system runs), and the next event that wakes the system goes to
`/var/run/alarmd-wakeup` (seconds, 0 for none; with none the RTC's alarm is
switched off too, so that an alarm deleted in the clock does not wake the
phone). `tools/pro1x-suspend` sets the RTC's alarm from that file before
it suspends, 60 s before the event (`PRO1X_ALARM_LEAD`), as alarmd does,
with the clock as it is then, and ends the suspend at the event's time at
the latest (the deadline, through the timer and the vMPM, should the RTC
not wake it); an event within 70 s: `an alarm in N s: not suspended`,
exit 3 (alarmd's own timeout, which makes it ring, does not run while
frozen, and its RTC alarm may already be past); no file (no alarmd): the
RTC's alarm as it is. Its last line has `; the RTC alarm fired` when
`dev.qcom_rtc.0.alarms` grew.

**Tested on the PC (2026-10-30):**

- `test/qcom_rtc_test.c` (`cc -I freebsd/pm/sys -o /tmp/qrtct
  freebsd/pm/test/qcom_rtc_test.c && /tmp/qrtct`): the register addresses
  against `pm8941_regs`; the count and the four bytes for alarms 2 s to a
  year ahead, counts 0 to 2^31 and the unset clock, against alarmd's
  `gmtime` through `librtc-realtime` into `pm8xxx_rtc_set_alarm`, and the
  read back against `read_alarm`; the write order (off, the bytes, on),
  nothing written to the count, the alarm firing at its count on a model
  of the two peripherals, both ways of the clear, a second alarm, off, the
  carry between the count's bytes: 159 checks, 0 failures.
- QEMU (`run.sh rtc "pro1x.test=rtcalarm hw.psci_idle.enable=1
  hw.psci_idle.simulate=1 hw.pro1x_pm.debug=1 pro1x.rtc_sleep=30"`, PMOUT
  as build.sh's): `qcom_rtc_sim` is `qcom_rtc.c` on QEMU virt's PL031 (its
  count less 1700000000 as the PMIC's, the alarm's count, enable and clear
  as its match, mask and clear registers, its interrupt, INTID 34, as the
  alarm's: a real device interrupt), `hint.qcom_rtc.0.offset=1700000000`
  in `pro1xq.env`. The device test below: A, B, C, D PASS, B `suspended
  59.980 s in 11 epochs, ended by INTID 34 (a device: the RTC alarm)` with
  7 CPUs frozen, D after 30 s the same. Before the freeze's change above
  the same run ended B and D by the wake-up path (`ended by a wake-up (the
  RTC alarm)`, CPU 0 woken 7 times to a PPI in the last freeze).
- The VM (alarmd 1.1.23_2 built with the patches, on a session bus there,
  `-lstderr -Ldebug`): alarms as clock-ui makes them 600 s and 200 s
  ahead: `hwrtc: alarm at: ... (T - 60)` for the earlier one, the file
  `T`; `pro1x-suspend` with a stand-in sysctl (no `pro1x_pm` there): `the
  next alarm at ...: the RTC alarm at ..., the deadline in 197 s`,
  `dev.qcom_rtc.0.wakealarm=T-60`, `hw.pro1x_pm.suspend=197`; with 30 s
  given, 30; an alarm 50 s ahead: `an alarm in 47 s: not suspended`, exit
  3 (alarmd itself set the RTC to now + 60 there, after the alarm); all
  deleted: the file 0, `wakealarm=0`. Without the sysctl alarmd says so
  once and runs on.

Not tested: the PMIC's alarm registers and interrupt themselves (whether
this EE may write the alarm peripheral and owns its interrupt: the
attach line says), the MPM waking the SoC from vlow/vmin for it, a
power-up from off by the alarm (the PMIC's power-on trigger; Leste's is
untested too).

## Stage 4b: the USB-C port (detection) and USB 3 (facts, not done)

**What the sources say** (mainline Linux 7.1: `sm6115.dtsi`, `pmi632.dtsi`,
`sm6115-fxtec-pro1x.dts`, whose USB-C wiring came with the 7.1 merge, not
with this project's patches; Linux's drivers):

- **Type-C** is the PMI632's (SPMI slave 2): `qcom,pmi632-typec` at
  0x1500 (Linux's `qcom_pmic_typec`, TCPM, PD disabled: `pd-disable`), a
  `usb-c-connector` with `power-role = "dual"`, `data-role = "dual"`,
  `typec-power-opmode = "default"`; its port 0 (high speed) goes to the
  DWC3 (`usb-role-switch`: TCPM sets the controller's role), its port 1
  (SuperSpeed) to the QMP PHY. VBUS out for host mode: the PMI632's
  `qcom,pmi632-vbus-reg` at 0x1100, 500 mA to 1 A in the Pro1X's tree.
- **USB 3 is wired, according to the device tree:** the QMP USB3 PHY
  (`qcom,sm6115-qmp-usb3-phy` at 0x1615000, Linux's `phy-qcom-qmp-usbc`,
  loaded on the phone: `~/ps/pro1x-power` logs) is enabled in the Pro1X's
  tree with its supplies (L4A, L12A), its output endpoint goes to the
  connector's SuperSpeed port, the DWC3 has both PHYs and no
  `maximum-speed` limit. The PHY is the `orientation-switch`: it selects
  the lanes by the plug's orientation in its own PCS_MISC register
  (0xa00 in its window: SW_PORTSELECT_MUX, and _VAL for CC2,
  `qmp_usbc_usb_power_on`); the TCSR register 0xb244 (`qcom,tcsr-reg`) is
  its VLS clamp for the autonomous (suspend) mode, 0 in normal operation. No SuperSpeed connection has been seen on the phone
  yet (none in the logs); that the lanes reach the plug is the device
  tree's statement, not a measurement.

**Done:** `pro1x_typec.ko` (`sys/dev/qcom_spmi/pro1x_typec.c`, a child of
FreeBSD's `qcom_pmic` bus): only with `hw.pro1x_typec.enable=1`. It
is woken by the block's interrupts (`attach-detach`, `cc-state-change`,
`vbus-change`, through the SPMI arbiter as `qcom_pon`'s keys; a look 20
ms after each; `hw.pro1x_typec.poll_ms=N` also polls, the fallback, and
without interrupts it polls every 250 ms) and reads MISC_STATUS (attached, orientation
CC1/CC2, VBUS detected, sink or source mode), SNK_STATUS (the partner's
Rp: 500 mA, 1.5 A, 3 A) and SRC_STATUS (Rd: a device, Ra/Ra audio,
debug), the registers of Linux's `qcom_pmic_typec_port.c`, and reports
`role=device` (we are the sink: a PC, or a charger, which CC cannot tell
apart), `role=host` (a device's Rd: an OTG adapter, a stick, a hub),
`orientation=cc1|cc2`, `vbus`, `rp` in `dev.pro1x_typec.0.state` and to
devd (system PRO1X, subsystem TYPEC, ATTACH/DETACH;
`rootfs/usr/local/etc/devd/pro1x_typec.conf` logs it). Read only by
default; `hw.pro1x_typec.drp=1` sets dual role if Linux left the port
otherwise (MODE_CFG 0x44), `hw.pro1x_typec.vbus=1` switches the VBUS
output on while a device is attached and off when it leaves (Linux's
`qcom_usb_vbus-regulator`: OTG_CFG source by command, the current limit
from `regulator-max-microamp`, CMD_OTG). Tested on the PC:
`test/typec_test.c` (the decoding of PC, flipped charger, OTG adapter,
powered cable, audio and debug accessories, the mode names, the VBUS
limits): 0 failures.

**Host mode, USB 3, the role switch (stage 4c, built, untested):**

- **Host mode** (`dwc3dci_host.c` in `pro1x_usb.ko`, `hw.dwc3dci.host=1`):
  the same bring-up as for the device, then the core's soft reset and its
  port capability to host (GCTL), and FreeBSD's xHCI driver on the core
  through a child device `xhci` (`generic_xhci_attach`, as FreeBSD's own
  `dwc3.c` does it) that uses the glue's register window and interrupt.
  Needs `xhci.ko` (and `umass.ko` for a stick). VBUS out comes from
  `pro1x_typec` (`hw.pro1x_typec.vbus=1`, now a sysctl).
- **USB 3** (`hw.dwc3dci.usb3=1`, host mode only): the QMP USB3 PHY as
  Linux's `qmp_usbc_com_init` and `qmp_usbc_usb_power_on` for the
  SM6115 (`qcm2290_usb3phy_cfg`): its supplies (L4A, L12A) through the
  RPM, its resets (GCC 0x1b000 and 0x1b008), the com_aux clock (0x1a054),
  the PCS powered down, the lane select by the orientation
  (`hw.dwc3dci.cc2`), the serdes table, the pipe clock (0x1a058), the TX
  and RX tables on both lanes, the PCS table, out of reset, started,
  PHYSTATUS clear within 10 ms. The tables (38 serdes, 5 TX, 17 RX, 21 PCS
  entries) are generated from Linux's (`dwc3dci_qmp.h`, each entry with
  its register's name); `test/qmp_test.c` checks entries against Linux's
  source and headers by hand, the offsets unique and inside their blocks:
  0 failures. Without `usb3` the port is USB 2 only (UTMI as the PIPE
  clock). SuperSpeed in device mode (`dwc3dci`) is not done: FreeBSD's
  device side has no SuperSpeed controller driver to follow (EP0 at 512
  bytes, bursts, the templates' SuperSpeed descriptors), so it stays a
  high-speed gadget.
- **The role switch** (`rootfs/usr/local/libexec/pro1x-usbrole`, run by
  devd on `pro1x_typec`'s events; off unless `pro1x.usbrole=1` in
  `freebsd/cmdline` or `pro1x_usbrole_enable="YES"`): `role=device` (a PC)
  starts `rc.d/pro1x_usbnet` (dwc3dci, CDC Ethernet, sshd); `role=host`
  (an OTG adapter, a stick, a hub) loads `xhci` and `umass`, switches
  VBUS on and loads `pro1x_usb` in host mode (with `pro1x.usb3=1` USB 3,
  its lanes by the event's orientation); `DETACH` unloads it and switches
  VBUS off. A change of role reloads the controller's module.

**Device run:**

14. **The port's detection** (`pro1x_typec`), with someone at the phone,
    from the full system's console (or ssh over Wi-Fi):
    `kenv hw.pro1x_typec.enable=1; kldload pro1x_typec`, then
    `tail -f /var/log/messages` and plug, one after the other: the PC's
    cable (both orientations), a charger, an OTG adapter with a USB
    stick, a hub. Expected: `pro1x_typec0: at 0x1500, mode 00 (dual
    role), VBUS out at 0x1100 up to 1000 mA (left alone)`, `pro1x_typec0:
    3 of 3 interrupts` (fewer: it polls every 250 ms, 4 wake-ups a
    second for good, so not into the profile then), then per plug
    `role=device orientation=cc1 vbus=1 rp=500 (misc 29 ...)` and with
    the plug turned `orientation=cc2`; the charger `role=device ...
    rp=1500` or `3000`; the OTG adapter `role=host ... vbus=0` (with
    `hw.pro1x_typec.vbus=1` also `VBUS out on` and the stick's light on);
    `DETACH` lines on unplugging. Failure signs: no `pro1x_typec0` (the
    node not enumerated below the PMIC: `devinfo -v`), `mode 01
    (disabled)` (Linux switched the port off at the kexec: then
    `hw.pro1x_typec.drp=1`), no change on plugging (`misc` stays: the
    block not running, or the debounce bits differ from Linux's reading).

15. **Host mode, USB 2** (needs 14), with someone at the phone, the full
    system: in `freebsd/cmdline` `pro1x.usbrole=1 hw.pro1x_typec.enable=1`
    (rc.conf or the console: `kldload pro1x_typec`), then an OTG adapter
    with a USB stick. Expected in `/var/log/messages`: `pro1x_typec:
    ATTACH role=host ...`, `pro1x-usbrole: ATTACH host ...: host`,
    `pro1x_typec0: VBUS out on`, `dwc3dci0: bring-up: ... host mode`,
    `dwc3dci0: host mode (xHCI), USB 2 only`, `xhci0: <DWC_usb3 xHCI
    (SM6115)> on dwc3dci0`, `usbus0 on xhci0`, `umass0` and `da1` for the
    stick; `DETACH` on unplugging, VBUS off. With a hub: the hub and what
    hangs on it. Failure signs: no `VBUS out on` or the stick unpowered
    (its light off: the VBUS output, `hw.pro1x_typec.vbus`); `xhci0`
    failing to reset or start (the core not in host mode: GCTL); the
    stick not seen (the PHY in host mode, VBUS).
16. **USB 3** (needs 15): as 15 with `pro1x.usb3=1` and a USB 3 stick, in
    both orientations of the adapter. Expected additionally `dwc3dci0:
    QMP USB3 PHY started (lanes CC1)` (CC2 turned), the stick on the
    SuperSpeed port (`usbconfig` shows it at 5.0 Gbps). Failure signs:
    `QMP USB3 PHY failed` (PHYSTATUS stays: the pipe clock, the resets,
    the supplies), the stick only at high speed in one orientation (the
    lane select reversed: CC1/CC2 swapped), at high speed in both (the
    lanes not reaching the plug after all, or the PHY's tables).
17. **The switch between the roles** (needs 11 and 15): the PC's cable,
    then the OTG adapter, then the PC again, with `pro1x.usbrole=1`.
    Expected: `ue0` and ssh with the PC, the stick with the adapter, `ue0`
    again with the PC, each change in the log (`pro1x-usbrole`).

## Stage 6: energy (autosleep, the power profile, fewer wake-ups)

Everything here is off by default; one switch, `pro1x.power=on` in
`freebsd/cmdline`, turns it on once the device runs below have
confirmed the parts (each part also has its own switch).

**Measured on the PC (2026-10-30).** QEMU (PRO1XQ, the rc's `pm.wake=30`:
every idle entry counted by `hw.psci_idle.stats`, 30 s idle, then 30 s
suspended in 5 s epochs; `run.sh NAME "hw.psci_idle.enable=1
hw.psci_idle.simulate=1 pm.wake=30 pm.secs=1"`):

| | awake, idle | suspended |
| --- | --- | --- |
| one CPU (`-smp 1`, the phone's default) | 16.7 wake-ups a second | 0.57 (11 between the epochs, 6 of CPU 0) |
| eight CPUs, the clusters' states (`pc_cluster=1`) | 33.9 | 6.4 (143 between the epochs, 42 of the frozen CPUs, 6 of CPU 0) |

The first run found a bug: with one CPU the suspend panicked at once
(`intr_ipi_send: not setup IPI 0`: no IPIs exist without other CPUs);
device run 6 on one CPU would have stopped there. Fixed (the freeze
sends IPIs only with more than one CPU).

The VM's Hildon session (`test/vm-wakeups.sh`: started as on the phone,
left 60 s, then 60 s under DTrace): with osso-systemui-alarm missing
from the VM, systemui ended at once and `hildon-session` restarted it
every 2 s, which with D-Bus, syslogd, ConsoleKit and polkit made about
85 wake-ups a second (systemui 1811, Xvfb 1323, dbus-daemon 741 in 60 s).
With the plugin installed the idle session's whole user space wakes
about 2.6 times a second (dbus-daemon 32, hildon-sv-notification 29,
gconfd-2 23, gvfs-udisks2-volume 20, polkitd 14, nsv-decoder-service 13,
hildon-desktop 4, alarmd 2 in 60 s). The restart now backs off (4, 8, 16
... up to 300 s; host check: 5 starts in 62 s instead of 31). FreeBSD's
kernel itself, measured there too: the page daemon 10 a second (its
inactive scan's pace, `VM_INACT_SCAN_RATE`), the buffer daemons 6, the
random harvester 9 (1 on the phone, `patches/0027`), IGMP's and MLD's
fast timeouts 5 each and their slow ones 2 each, IPv6's fragment timer 2,
the kernel log 4.75 (`kern.log_wakeups_per_second=1` in
`rootfs/etc/sysctl.conf` now), syncer and vnlru 1 each. The page daemon's
and IGMP/MLD's timers would need kernel changes; while suspended they
run only between the epochs.

Our daemons, changed: `pro1x-audiod` woke every second for nothing (its
poll timeout) and every 100 ms for the jack (a SoundWire command each,
the bus and the TX macro's clocks running for good): now it sleeps until
something is due, the jack comes by interrupt (below; `auto` checks it
with a read every 2 s, `clkstop` not at all). `pro1x-modemd` asked the
modem for the signal every 30 s (the modem woken, its answer a GLINK
interrupt that ends a suspend): no longer while the panel is off or
shows the stand-by screen; its timer for long SMS ran once a minute for
good: now only while parts wait.

**The jack without polling** (`../audio/README.md`, "The headset jack"):
`qcom_swr.ko` hands swr0's interrupt lines (the master's, SPI 296, and
the wake-up while its clock is stopped, SPI 79) to `pro1x-audiod`, which
handles the WCD9370's SoundWire alerts as Linux's `soundwire-qcom` does
and stops the bus' clock between events (`pro1x_audiod_jack`:
`auto` by default, `clkstop` with the profile; `poll` as before).

**pro1x-autosleep** (`tools/pro1x-autosleep`, `rc.d/pro1x_autosleep`):
Android's autosleep and mce/dsme's policy, Leste's `pro1x-autosuspend`
for FreeBSD. It suspends (`pro1x-suspend -k`: the panel as it is, the
next alarm as the RTC's, none within 70 s of one) when the display has
been off, or showing the stand-by screen (`-L`: not then), for 30 s
(`-D`) and nothing holds the phone up: sound (`pro1x-audioctl status`:
a stream, a voice session, FM), a call (`pro1x-callaudio` runs),
positions (`pro1x-gnss` runs: an app wants them, `../modem/README.md`
stage 3; the modem's position every second would end each suspend), a
transfer (the interfaces' bytes, more than 4 KiB a second), the USB
network (`ue0` up), external power (`hw.acpi.acline`; `-P`: also then),
the file `/var/run/pro1x-autosleep.inhibit`. A device interrupt ends the
freeze; after the power key or the RTC alarm (INTID 215) it stays up
30 s, after the modem's interrupts (100, 102) 10 s (a call then holds it
up by itself), after a wake-up from user space or the deadline 30 s
(`-a`), after anything else 2 s (`-s`); `-w "215:30 100:10 102:10"`.
Every suspend goes to the log with its end (`... ended by INTID 215 (a
device: the power key) [INTID 215 (gic0,s183: ...)]; up 30 s`), the
totals per interrupt to `/var/run/pro1x-autosleep.stats` (`service
pro1x_autosleep stats`, also on SIGUSR1). A cable or a charger raises no
interrupt that ends the freeze: a child of the daemon reads devd's
events (`/var/run/devd.seqpacket.pipe`; not a rule in a devd file: devd
runs only the first rule that matches an event, which would take
`pro1x_typec.conf`'s role switch and `pro1x.conf`'s status LED their
events) and on a Type-C attach, or the battery's change on external
power, ends the suspend through `hw.pro1x_pm.wake=1`, seen between the
epochs. Tested: the policy on the
PC (`cc -I freebsd/pm/tools/pro1x-autosleep -o /tmp/ast
freebsd/pm/test/autosleep_test.c
freebsd/pm/tools/pro1x-autosleep/policy.c && /tmp/ast`, 31 checks), the
daemon in the VM (`test/vm-autosleep.sh`, its test mode `-T`: the
display from files, a stand-in for `pro1x-suspend`: 11 checks: none
with the display on, the delay, the stays, a call, positions (a process
named `pro1x-gnss`) and the inhibit file holding it up, the statistics;
all passed on 2026-10-31 with the positions' check).

**The modem's interrupts and the freeze.** FreeBSD's GLINK edge to the
modem (SPI 68, INTID 100) and its SMP2P (SPI 70, INTID 102) are real
interrupts (`bus_setup_intr` in `../dsp/.../qcom_glink.c`,
`qcom_smp2p.c`). The freeze changes nothing in the GIC: no enable, no
priority mask; only the CPU's own interrupt mask is set while it waits,
which `wfi` ignores. So a message from the modem ends a suspend as the
power key does (the CPU the SPI goes to sees it pending and tells CPU 0).
vlow/vmin are another matter: neither interrupt is an MPM pin (the
device tree's `qcom,mpm-pin-map`: 5/296 and 24/79 SoundWire, 12/422 and
90, 91/260 USB, 86/183 the SPMI arbiter), and while the SoC sleeps the
GIC has no clock: only the MPM's pins and timer wake the apps processor.
Whether the modem, itself one of the RPM's masters and awake when it
sends, gets the apps processor's interrupt through anyway is not known.
Until device run 21 shows it, `hw.pro1x_pm.mpm_veto` ("100 102") skips
the hand-over while either interrupt is enabled: no vlow/vmin while the
modem is up (Linux's mainline MPM driver has no such check; Leste's
notify_rpm test asks the same). The MPM pins of the map now follow their
SPIs (`hw.pro1x_pm.mpm_spi=1`): the SoundWire lines are pins while armed.

**Wi-Fi's power save** (`patches/0037`): LinuxKPI's 802.11 never gave a
driver power save (`vif->cfg.ps` hard off, `IEEE80211_C_PMGT` not
offered), so ath10k switched the WCN3990's off as soon as its vdev
started (`WMI_STA_PS_MODE_DISABLED`): the receiver on between beacons.
Now `ifconfig wlan0 powersave` (taking effect at the next association)
reaches the firmware as mac80211 would (`vif->cfg.ps`, `BSS_CHANGED_PS`,
a dynamic timeout of 100 ms). Built, not tried with the chip (ath10k's
comment on some firmware's unstable STA power save applies).

**The switches:**

| | what | default |
| --- | --- | --- |
| `pro1x.power=on` (`freebsd/cmdline`) | all of the profile: in the kernel `kern.smp.disabled=0`, `hw.psci_idle.enable=1` (`patches/0036`, below the command line's own settings), in user space the rest of this table | off |
| `pro1x_power_enable="YES"` (`rc.conf`, above its profile block) | the user space half (the CPUs' idle states then without OS-initiated mode: the cores' only) | NO |
| `pro1x_autosleep_enable`, `pro1x_autosleep_flags` | the suspend while nobody uses the phone | NO (profile: YES) |
| `pro1x_audiod_jack` | the jack: `poll`, `irq`, `auto`, `clkstop` | `auto` (profile: `clkstop`) |
| `ifconfig_wlan0="WPA DHCP powersave"` | Wi-Fi's power save | off (profile: on) |
| `powerd_flags` | `-p 1000`: the load looked at once a second | `-p 500` (profile: 1000) |
| `pro1x.usbrole=1` (kenv) | the USB controller follows the cable (`pro1x-usbrole`, stage 4c): the gadget only with one; its events need `hw.pro1x_typec.enable=1` too (`freebsd/cmdline`; `rc.d/pro1x_power` then loads `pro1x_typec.ko`), after run 14 | off (profile: on) |
| `pro1x.standby_off=N` (kenv) | the panel off after N minutes of the stand-by screen (`pro1x-standby -O`, whole minutes: the stand-by screen comes 33 s after the last input), 0 at once, `never` the N9's stand-by screen for good | 2, with or without the profile (2026-10-31: the display off after 2 minutes on every system); `pro1x.standby=off`: the panel off 2 minutes after the last input |
| `hw.pro1x_pm.mpm_veto`, `mpm_spi` | above | "100 102", 1 |
| `hw.pro1x_pm.wake=1` | ends a suspend in progress (pro1x-autosleep's devd reader) | |
| `service pro1x_power status` | what is on, the last suspend, the RPM's counts | |

Not done: the page daemon's and IGMP/MLD's kernel timers; sensor
batching (the stand-by screen's proximity sensor is unwatched once the
panel goes off, `-O`); the display controller's own power-down with the
panel off (the DPU keeps scanning out; the panel's DCS off only).

**The energy audit of 2026-10-31** (PC only; what it changed, what the
device runs measure next):

- Changed: the GPS no longer runs on after its last app
  (`../hildon/patches/location-daemon/0002`, the hook's `stop`: gpsd,
  `pro1x-gnss` and the modem's GNSS engine ran until the next boot once
  any app had asked for a position); `pro1x-autosleep` stays up while
  `pro1x-gnss` runs; `../rootfs.sh` makes the packages' `@sample`
  configuration files as pkg does (PulseAudio found no `default.pa` and
  did not start at all, and with it went `module-suspend-on-idle`, which
  closes `/dev/dsp` 5 s after the last sound); `rc.d/pro1x_power` loads
  `pro1x_typec.ko` with `hw.pro1x_typec.enable=1` (the profile's role
  switch had no events); the panel off 2 minutes into the stand-by
  screen by default (`~/.xinitrc`; before: never, the profile 10
  minutes), X's screen saver after 2 minutes instead of 3.
- Checked, nothing to do: the kernel has no debug options (no
  `std.debug`: no INVARIANTS, WITNESS); idle is tickless
  (`kern.eventtimer.periodic=0`), so HZ 1000 costs wake-ups only while a
  CPU runs; no background scans (LinuxKPI 802.11 without
  `LKPI_80211_BGSCAN`), the keys in the Wi-Fi firmware; no moused, cron or
  NTP; `/var`, `/tmp` in memory; ZFS lz4, `atime=off`, a transaction group
  every 30 s; powerd's `dev.cpu.0.freq` sets both clusters (cpufreq sets
  every CPU's nearest level); the battery every 30 s, the keyboard scanned
  only while a key is held, UFS Hibern8 after 150 ms.
- For the device (runs 9, 14, 20, 22, 24): the USB block as Linux leaves
  it at the kexec (run 9's probe: the GDSC, the 66.67 MHz master clock and
  the QUSB2 PHY's PLL running with no FreeBSD driver on them would be
  waste for as long as FreeBSD runs; `dwc3dci`'s detach does not switch
  the PHY, the clocks, the GDSC or its RPM supplies off either, and of
  those supplies only L15A is the USB's alone: L4A and L12A also feed the
  UFS PHY and must stay); the DPU still scanning out with the panel off
  (it keeps the memory bus up: `vlow +0, vmin +0` in run 20 (c) with the
  modem off points at it); `pro1x_typec`'s interrupts (without them it
  polls 4 times a second); the epochs of the suspend (5 s, only for the
  watchdog: Linux's `qcom_wdt` stops the watchdog for its suspend, which
  would allow epochs of a minute once runs 6-8 are stable); ZFS's kernel
  threads that wake once a second while awake (`l2arc_feed` with no
  L2ARC, `arc_evict`, `arc_reap`, `dbuf_evict`, and `mmp` for a writable
  pool), frozen while suspended like every callout.
- Bluetooth: off. Nothing in FreeBSD brings it up (no `ng_ubt`, hcsecd,
  sdpd or bthidd); `pro1x-fmd` powers the WCN3950's Bluetooth side only
  while the FM receiver is on (its rails at the RPM, the UART) and off
  again with it. Not known: what Leste leaves of the chip at the kexec
  with its Bluetooth on (its rails L16A, L17A, L23A are the Wi-Fi's too,
  so no blind vote off; the RPM's counts of run 6 show a held resource).
- Double tap with the panel off: possible only through the touch
  controller's own gesture (doze) mode, which this port does not have
  yet. The GT9286's firmware (Goodix's vendor driver: 8 to 0x8046 and to
  the command register 0x8040, the gesture at 0x814B, 0xCC a double tap)
  scans slowly and interrupts on the gesture alone; its interrupt, GPIO 80,
  is MPM pin 21, so `hw.pro1x_pm.mpm_pins` could take it (`21:l`) and the
  SoC sleep through it. Needs a `goodix` sysctl for the mode, the
  gesture's read in its interrupt and a devd event for pro1x-standby,
  first tried on the phone (whether this firmware has the mode). Until
  then the panel off means the power key only, and the stand-by screen
  (touch awake, double tap) costs the touch controller's full scanning.

## Device runs (prepared, for 2026-10-31)

The kernel: `WORK=... bash freebsd/pm/build.sh phone` gives
`$WORK/pm-out/kernel.bin` (PRO1XPM: `freebsd/PRO1X` with `device
psci_idle`, the RAM root of `freebsd/mfsroot` with `pro1x-pmtest.sh`).
On the phone, from Leste: keep `/host/freebsd/kernel.bin` as
`kernel.bin.orig`, copy PRO1XPM's there, put the run's line into
`/host/freebsd/cmdline`, reboot and choose FreeBSD in the boot menu. Each
run ends with a reboot (or the watchdog's reset) into the boot menu;
then, in Leste, the RAM console (0x5ca01000, `README.md`) has the
report. Afterwards `cmdline` back to what it was.

Every run has `hint.qcom_wdt.0.lifetime=N`: the watchdog resets the
phone N seconds after the start whatever happens.

1. **SMP** (`smp`), about 2 minutes:

       kern.smp.disabled=0 pro1x.test=smp hint.qcom_wdt.0.lifetime=300

   Expected: `psci_idle: PSCI 1.x over smc, CPU_SUSPEND features 0x3
   (extended state ids, OSI there); 8 CPUs in the device tree, off at
   boot: 0xfe`, `CPU  0:` to `CPU  7:` (affinity 0-3 and 1 0-3),
   `Release APs...done`, `hw.ncpu: 8`, `pm: ... busy: cpu0 9x% ... cpu7
   9x%`, `dev.cpu.0.freq_levels` and `dev.cpu.4.freq_levels` (two
   domains), `worker N: ... 0 wrong` eight times, `=== test smp done`.
   Failure signs: `Failed to start CPU N` (CPU_ON refused: its return
   value is in the line), fewer CPUs, the console ending after
   `Release APs` (a CPU hangs the system: reset after 300 s), a driver
   misbehaving with its interrupt on another CPU (keys, touch: the
   console shows its messages). If it fails, `kern.smp.disabled=1`
   stays in `pro1x.env`.

2. **Idle on one CPU, the cores' state only** (`idle`), about 14 minutes:

       pro1x.test=idle hw.psci_idle.enable=1 hw.psci_idle.depth=1 pro1x.power_min=10 hint.qcom_wdt.0.lifetime=1200

   (`kern.smp.disabled=1` from `pro1x.env`: CPU 0 alone.) Expected:
   `psci_idle: OS-initiated mode, enabled; 1 CPUs, 1 clusters; broadcast
   timer: yes; resume at 0x...`, `arm_tmr_mem0: frame 0 at 0xf121000,
   virtual timer, 19200000 Hz, ...` (or physical), the states as in the
   table, then in the statistics `silver-rail-power-collapse N` growing,
   `resumed` equal to it (each a real power-down and warm boot),
   `failed 0`, the frame's interrupts growing, the work check without a
   wrong result, then the power lines (`power: s mA mV % status`, the
   panel off) to compare with a `pro1x.test=power` run (wfi only).
   Failure signs: the console ending at the first idle after the hook is
   set (right after the `psci_idle:` lines): the warm boot does not come
   back (the resume entry, the identity map, or the firmware wants
   something else, e.g. EL2) or the CPU never wakes (the frame); reset
   after 20 minutes. `failed N (last -2)`: the parameter refused
   (INVALID_PARAMETERS), `(last -3)`: DENIED. `broadcast timer: none`
   with `arm_tmr_mem0: frame 0: CNTACR 0, not usable`: the frame is not
   open to this side; then only wfi (stats: `wfi` only). Then the same
   with `hw.psci_idle.depth=0` for comparison is pointless (it is
   `wfi`); compare with `pro1x.test=power`.

3. **Idle on one CPU with the cluster's states** (OSI: CPU 0 is cluster
   0's only online CPU, so it is its last):

       pro1x.test=idle hw.psci_idle.enable=1 pro1x.power_min=10 hint.qcom_wdt.0.lifetime=1200

   Expected additionally `cluster-sleep-0-1 N` (the power collapse,
   0x41000044) growing for idles above 7.4 ms, `denied 0`. Failure
   signs: as 2, now in the cluster's way back (2.1 ms exit latency);
   `denied` growing with a single CPU: the firmware does not count the
   other three as down (they are off), which would be a firmware
   behaviour to note.

4. **Idle on all eight CPUs** (needs 1 and 3):

       kern.smp.disabled=0 pro1x.test=idle hw.psci_idle.enable=1 pro1x.power_min=10 hint.qcom_wdt.0.lifetime=1200

   Expected: every CPU's states, both clusters' power collapse, the
   frame's interrupts and IPIs, no wrong result, the standby current
   at most that of 3.

5. For a comparison of the current: `pro1x.test=power` (wfi, one CPU)
   the same 10 minutes, unplugged.

6. **The SoC's sleep and suspend** (`suspend`; needs 2, better 3),
   unplugged, about 16 minutes. First, from Leste:
   `mkdir -p /host/freebsd/pm` and `pm-out/modules/qcom_dsp.ko`,
   `pro1x_rpmsleep.ko` into it (the RAM root loads them from userdata,
   read only). Then:

       pro1x.test=suspend hw.psci_idle.enable=1 hw.pro1x_pm.debug=1 pro1x.power_min=10 hint.qcom_wdt.0.lifetime=1500

   (`hw.pro1x_pm.debug=1`: each freeze on the console, so that a stop
   shows which one it was; one of five QEMU runs stopped once, below.)

   Expected: `pro1x_pm: vMPM at 0x45f01b8 (96 pins), doorbell 0xf111008
   bit 1, RPM statistics yes; the hand-over off (hw.pro1x_pm.mpm=1), wake
   pins 86:h`, `hw.pro1x_pm.rpm_stats: vmin N (...), vlow N (...)`
   (Linux's counts so far, probably 0 and 0), `qcom_dsp: ...`,
   `qcom_rpm: message RAM 0x45f0000 ...`, `pro1x_rpmsleep: the sleep set:
   12 of 12 resources at 0; none refused`. A (60 s idle with the
   hand-over): `MPM hand-overs` growing, and the real question, `vlow`
   or `vmin` counting up. B: `hw.pro1x_pm.last: suspended 60.0xx s in 11
   epochs, ended by the deadline 0; ... at least 7 other CPUs frozen`
   (0 with one CPU), the RPM's counts `+N`. D: ten `power:` lines with
   the current read right after each minute's wake-up (the current
   while frozen cannot be read; the voltage and the capacity over the
   ten minutes are the better measure) and a `hw.pro1x_pm.last` line
   each. Failure signs: the console ending after `B: suspend 60 s` (the
   SoC does not wake from its sleep: no timer reached the RPM, or the
   frame does not wake CPU 0): the watchdog resets after 25 minutes
   (it keeps running while frozen; the epochs of 5 s keep it fed); `ended by INTID N` early in
   B: a device interrupt that keeps coming (N - 32 is its SPI: which
   driver's is in the device tree); `vlow +0, vmin +0` with hand-overs:
   another master or a vote holds the SoC up, as on Leste
   (`docs/power-management.md`); the next step is the subsystems'
   counts and the votes in the RPM's records.

7. **The power key** (with someone at the phone), as 6 plus
   `pro1x.suspend_key=1`: at `C: suspend until the power key` press the
   power key once. Expected: `ended by INTID 215 (a device: the power key)` (the SPMI
   arbiter's interrupt, SPI 183), a few seconds after the press at most.
   The press also reaches `qcom_pon` (its devctl/evdev event) as usual.

8. With all eight CPUs: 6 with `kern.smp.disabled=0` (needs 1 and 4).

9. The USB probe (`pro1x.test=usb`), 10. the gadget
   (`pro1x.test=usbnet`) and 11. the full system over the cable
   (`pro1x.usb=1`): see stage 4 above; 14 to 17 (the USB-C port, host
   mode, USB 3, the role switch): stage 4b/4c. (12 and 13 are the RTC
   alarm's.)

12. **The RTC alarm** (`rtcalarm`, stage 5; needs 6 to have woken by its
    deadline), unplugged, about 7 minutes, nobody needed at the phone.
    The kernel with `patches/0035` (PRO1XPM from `pm/build.sh phone`, as
    in 6; `$WORK/rtc-out/kernel.bin` of 2026-10-30), `qcom_dsp.ko` and
    `pro1x_rpmsleep.ko` in `/host/freebsd/pm/` as for 6. Then:

        pro1x.test=rtcalarm hw.psci_idle.enable=1 hw.pro1x_pm.debug=1 hint.qcom_wdt.0.lifetime=900

    Expected at the attach: `qcom_rtc0: counter N, offset M`, `qcom_rtc0:
    alarm at 0x6100 (type 0xb/3), irq K; off` (or `on, as the previous
    system left it: at counter X, in S s`: an alarm Leste set, which may
    end a suspend early), `dev.qcom_rtc.0.alarm: counter N, alarm off; 0
    alarms; interrupt set up`. Then A: `qcom_rtc0: alarm at counter N+20,
    in 20 s (counter N)`, 20 s later `qcom_rtc0: alarm: counter N+20, set
    for N+20`, `A: PASS: the alarm's interrupt while awake`; B: `suspend
    180 s`, the freezes (`hw.pro1x_pm.debug`), after 60 s
    `hw.pro1x_pm.last: suspended about 60 s in 11 epochs, ended by INTID 215
    (a device: the RTC alarm); ...`, `B: PASS: woken after 60 s, the RTC
    alarm`; C: `alarm off (counter ...)`, `ended by the deadline 0`, `C:
    PASS: 40 s, the deadline, no alarm`; D: the sleep set and the
    hand-over as in 6, `D: alarm in 120 s`, `ended by INTID 215 (a
    device: the RTC alarm)` within 2 s of the alarm, `RPM: vlow +N, vmin
    +N` if the SoC slept, `MPM hand-overs +N`, and in
    `hw.pro1x_pm.rpm_stats` the pins seen with pin 86 (`00000000 00000000
    00400000`). Failure signs: `alarm at 0x6100 not writable from this
    EE` or `no interrupt (not this EE's?)` at the attach (the arbiter's
    tables give the alarm peripheral to another EE: then only Linux can
    use it); `A: FAIL: no alarm interrupt` with `alarm on at counter ...
    (in -N s)` (the alarm's count reached, no interrupt: its QPNPINT
    enable or the arbiter's accumulator; the attach's irq line and
    `vmstat -i` on the console); A passes, B fails with `alarms` not
    grown and `ended by the deadline 0` after 180 s (a second alarm does
    not fire: the status not cleared; again with
    `hw.qcom_rtc.alarm_clear=1` on the line); B ends at once by INTID 215
    with another source or none (a Leste alarm left on: the attach line;
    or the power key); D woken up to 5 s late, without pin 86 in the pins
    seen (the MPM did not wake the SoC for the arbiter's line: the
    5 s epoch's timer did, and the pending interrupt was seen then: on
    the phone the alarm is then late by up to 5 s, no more); a reset
    (the watchdog: a freeze hung, `hw.pro1x_pm.debug` shows which).

13. **The alarm clock in the full system** (needs 12; the joint image
    with alarmd 1.1.23_2, `pro1x-suspend` and the kernel with
    `patches/0035`), someone at the phone, about 10 minutes. In
    `freebsd/cmdline` `hint.qcom_wdt.0.lifetime=900` for the run (out
    again afterwards), boot FreeBSD into Hildon, unplugged. In the clock
    an alarm 3 minutes ahead (whole minutes: at the minute after next
    plus one), then in X Terminal as root `pro1x-suspend`. Expected:
    `pro1x-suspend: the next alarm at HH:MM:00: the RTC alarm at
    HH:MM-1:00, the deadline in N s`, the display off; a minute before the
    alarm the display back on (`... ended by INTID 215 (a device: the RTC
    alarm); ...; the RTC alarm fired`, also in `/var/log/messages` and
    on the RAM console's `pro1x_pm:` line), at the alarm's minute the alarm
    dialog and its sound, Stop ends it. Failure signs: the display back
    only at the alarm's minute with `ended by the deadline 0` (the RTC did
    not wake it, the deadline did: run 12 shows why) and the alarm still
    ringing; `no RTC alarm` in the first line (`dev.qcom_rtc.0.wakealarm`
    missing: the attach line); no `the next alarm` line at all
    (`/var/run/alarmd-wakeup` missing or 0: alarmd 1.1.23_1, or the alarm
    not an `ACTDEAD` one); `an alarm in N s: not suspended` (set it
    further ahead); the dialog but no sound (the sound daemon decodes the tones
    90 s after the session's start, an alarm before is silent:
    `../hildon/README.md`; else the profile's alarm volume). The power key ends the suspend as before (`ended by INTID 215
    (a device: the power key)`); the RTC's alarm then stays set and fires
    while awake, which is harmless.

**Stage 6** (the full system: the joint image with this stage's parts,
the kernel with `patches/0036` and `0037`; unplugged unless said):

18. **The jack by interrupt** (needs the trace's `[jack-up]`/`[jack-down]`
    and run audio-jack of `../audio/README.md`): as audio-jack with
    `pro1x.audio_opts="-j irq"`. Expected: `/dev/qcom_swr0: master INTID
    328, wake-up INTID 111`, `swr0: device 3: SCP interrupts unmasked`,
    `jack: watched (...; irq)`; every plug, unplug and button as with
    polling, `pro1x-audioctl jack` with `polls=0` and `wcd_alerts` growing.
    Then `-j clkstop`: `clock_stops` +1 about 3 s after each event, a
    plug while stopped `wakeups` +1 and the detection as before;
    `sysctl dev.qcom_swr.0.stats` counts both lines. Failure signs: `jack:
    status ... latched without an alert` (in `auto` the fall-back to
    polling: the WCD did not alert; IntMask1, the master's level mode);
    no wake-up while stopped (again with `-W`: WakeUpEnable; else the
    wake-up line, SPI 79); a hang at the clock stop (the master read
    unclocked: the log's last `codec: [jack-down]`).
19. **Autosleep by hand** (with someone at the phone): FreeBSD into Hildon,
    `service pro1x_autosleep onestart`, the power key for the stand-by
    screen, 2 minutes. Expected in `/var/log/messages`: `pro1x-autosleep:
    on: ...`, after 30 s `suspended N s ..., ended by ...`; the power key
    brings the desktop back within a second (`ended by INTID 215 (a
    device: the power key)`), the stand-by screen's minute keeps going
    (at most 5 s late); a call or an SMS ends a suspend (`ended by INTID
    100`). `touch /var/run/pro1x-autosleep.inhibit`: no suspend.
20. **The drain** (needs 19): an hour each, unplugged, the display in the
    stand-by screen, the same Wi-Fi and modem state, `nohup pro1x-drain 60
    10 &` on the console or over ssh, the lines then in `/var/log/messages`:
    (a) as before 2026-10-31 (no `pro1x.power`, `pro1x.standby_off=never`:
    the stand-by screen the whole hour), (b) `pro1x.power=on
    pro1x.standby_off=never`, (c) `pro1x.power=on` (the panel off after 2
    minutes of the stand-by screen, the default now). Compare `% an
    hour`, `mV an hour` (OCV) and `asleep N of 3600 s`.
21. **vlow/vmin and the modem** (during 20 (c)): `hw.pro1x_pm.rpm_stats`
    before and after: `vetoed N (last by INTID 100)` while the modem is
    up. Then `sysctl hw.pro1x_pm.mpm_veto=""` and an SMS and a call to the
    phone while it is suspended: `ended by INTID 100` within seconds, and
    `vlow +N`/`vmin +N` meanwhile: the veto can go (`hw.pro1x_pm.mpm_veto=""`
    in the profile). The call only after up to 5 s (the next epoch) or
    lost: keep it. Without the modem (`pro1x_modem` off) `vetoed` stays.
22. **What wakes it** (after 20): `service pro1x_autosleep stats`: the
    suspends per interrupt. Frequent ones that are no wake-up source
    (Wi-Fi's copy engines, the ADSP's GLINK 309 for sensors, the battery's
    ADC) are the next targets. With `pro1x_steps_enable="YES"` walk a
    minute with the phone suspended: the ADSP reports every step
    (`tools/pro1x-steps`), so INTID 309 per step means the pedometer needs
    the sensor hub's batching (a report period) before it can stay on
    with autosleep. INTID 100 every few seconds with the phone lying
    still is the modem's indications (cell changes, the network's time):
    then fewer of them while the display is off.
23. **Wi-Fi's power save** (with Wi-Fi up, `pro1x.power=on` or by hand:
    `ifconfig wlan0 powersave` and a reconnect): `ifconfig wlan0` shows
    `powersavemode ON`; from the PC `ping` the phone: answers 100-300 ms
    late while it is quiet (the listen interval), the transfer as before
    (`fetch` of a large file); `dev.pro1x_battery.0.current` lower than
    with `-powersave`. Failure signs: disconnects (`wlan0: link state
    changed to DOWN`): `-powersave` again, a firmware with unstable STA
    power save.
24. **GPS off after its app** (the full system, `pro1x_modem` and the
    joint image with location-daemon 0002): an app that asks for a
    position (GPS Recorder, or liblocation's example as in
    `../modem/README.md`), then close it. Expected: while it runs
    `service pro1x_gps status` shows gpsd and pro1x-gnss (and with the
    display off `/var/log/debug.log` has `pro1x-autosleep: up:
    positions`); at most 30 s after it closed both are gone
    (`pro1x-gnss` sent LOC Stop on its SIGTERM; it does not log it) and
    `pro1x-autosleep` suspends again. With `pro1x_gps_enable="YES"` both
    stay (wanted). Failure signs: gpsd still there a minute later with
    location-daemon still running (a client still holds liblocation's
    lock), or with location-daemon gone (its stop did not run: the old
    location-daemon package; or the hook's stop failed:
    `/usr/local/libexec/location-daemon-gpsd stop` by hand says why).

Runs 1-4 (SMP, the CPUs' idle states) and 6-7 (the suspend, now also on
one CPU without the IPI panic) are the profile's kernel half;
`pro1x.power=on` turns on what they test.

The vMPM hand-over and the sleep set are the parts with the most
unknowns: they are copied from Linux and KolibriOS, neither of which
has shown vlow/vmin on this phone yet (Linux's `notify_rpm` test is on
2026-10-31 too: `docs/power-management.md`). If Linux's test shows how
the SoC gets there, the same settings apply here.

Risks: a hang in a power-down ends in the watchdog's reset (the RAM
console keeps what came before); the hook is set right after `Release
APs` (SI_SUB_SMP), so with `hw.psci_idle.enable=1` the boot only gets
further if the way back works. None of the runs writes to userdata.
