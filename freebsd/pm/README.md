# FreeBSD power management on the Pro1X: SMP, CPU idle, sleep, USB

FreeBSD on the phone ran on one CPU with `wfi` as its only idle state.
This directory adds, in stages:

| Stage | What | State |
| --- | --- | --- |
| 1 | SMP: all eight CPUs (PSCI CPU_ON) | FreeBSD's own code; checked against the device tree, tried in QEMU (8 CPUs); device run prepared |
| 2 | CPU idle: the PSCI idle states of the device tree, OS-initiated mode, the core's context across a power-down, the memory-mapped timer as the wake-up | built, tried in QEMU with every power-down played (`simulate`); device runs prepared |
| 3 | the SoC's sleep (RPM sleep set, MPM hand-over) and suspend-to-idle | built; the freeze tried in QEMU (no RPM there); device runs prepared |
| 4 | USB device mode (CDC ethernet to the PC) | dwc3dci (a DWC_usb3 device controller driver) and the SM6115 glue built, the encodings tested on the PC; a probe and the gadget run prepared |

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
| `test/dwc3dci_test.c` | the host test of `dwc3dci_hw.h` |
| `../patches/0032-arm64-cpu_idle-hook-psci-idle.patch` | `cpu_idle()` asks `arm64_cpu_idle_hook` first; the files for `device psci_idle` |
| `../mfsroot/etc/pro1x-pmtest.sh` | the device tests in the RAM root (`pro1x.test=smp`, `idle`), sourced by `mfsroot/etc/rc` |
| `build.sh` | the kernels: `qemu` (PRO1XQ) and `phone` (PRO1XPM = `freebsd/PRO1X` with `device psci_idle`) into `$WORK/pm-out` |
| `tools/pro1x-suspend` | the Pro1X's `zzz`: panel, touch and keyboard light off, the sleep set and the hand-over, suspend until the power key or N seconds, everything back (for the root image's `/usr/local/sbin`) |
| `test/qemu/` | PRO1XQ, its device tree (`mkdtb.py`: QEMU's virt with the SM6115's idle topology), its rc, `run.sh` |

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
tells CPU 0 by an IPI. The suspend runs in epochs of
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

Not done: the PMIC's RTC alarm as a wake-up (`qcom_rtc` has no alarm:
the alarm registers at 0x6140 and their interrupt through the arbiter,
Linux's `rtc-pm8xxx.c`); the suspend's deadline is the timer's (the
frame, the vMPM's timer), which wakes the same way for a test.
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
- **Not done:** the serial console (`hw.usb.template=8`, CDC Ethernet and
  a serial port, with `umodem.ko` and a getty on `/dev/ttyU0`: the next
  step, cheap once the Ethernet works), the full system's side
  (`rc.conf`: load the modules, `ifconfig_ue0="inet 172.16.42.1/24"`,
  sshd), the HSTX trim, USB3, the PMI632's VBUS and Type-C detection
  (VBUS is forced valid).

**Device run:**

9. **USB probe** (`usb`), seconds, read only:

       pro1x.test=usb hint.qcom_wdt.0.lifetime=120

   Expected: `hw.pro1x_pm.usb_probe: USB30_PRIM_BCR ..., USB30_PRIM_GDSCR
   ..., ...; GDSC on|off, master clock on|off, its bus clock on|off,
   reset asserted|released` and either the DWC3's registers (GSNPSID
   0x5533xxxx for a DWC_usb3) or `not read (not clocked)`. That is the
   input for the next step; nothing is changed.

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
   power key once. Expected: `ended by INTID 215 (a device)` (the SPMI
   arbiter's interrupt, SPI 183), a few seconds after the press at most.
   The press also reaches `qcom_pon` (its devctl/evdev event) as usual.

8. With all eight CPUs: 6 with `kern.smp.disabled=0` (needs 1 and 4).

9. The USB probe (`pro1x.test=usb`) and 10. the gadget
   (`pro1x.test=usbnet`): see stage 4 above.

The vMPM hand-over and the sleep set are the parts with the most
unknowns: they are copied from Linux and KolibriOS, neither of which
has shown vlow/vmin on this phone yet (Linux's `notify_rpm` test is on
2026-10-31 too: `docs/power-management.md`). If Linux's test shows how
the SoC gets there, the same settings apply here.

Risks: a hang in a power-down ends in the watchdog's reset (the RAM
console keeps what came before); the hook is set right after `Release
APs` (SI_SUB_SMP), so with `hw.psci_idle.enable=1` the boot only gets
further if the way back works. None of the runs writes to userdata.
