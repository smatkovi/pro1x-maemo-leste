# FreeBSD power management on the Pro1X: SMP, CPU idle, sleep, USB

FreeBSD on the phone ran on one CPU with `wfi` as its only idle state.
This directory adds, in stages:

| Stage | What | State |
| --- | --- | --- |
| 1 | SMP: all eight CPUs (PSCI CPU_ON) | FreeBSD's own code; checked against the device tree, tried in QEMU (8 CPUs); device run prepared |
| 2 | CPU idle: the PSCI idle states of the device tree, OS-initiated mode, the core's context across a power-down, the memory-mapped timer as the wake-up | built, tried in QEMU with every power-down played (`simulate`); device runs prepared |
| 3 | the SoC's sleep (RPM sleep set, MPM hand-over) and suspend-to-idle | see below |
| 4 | USB device mode (CDC ethernet to the PC) | see below |

Nothing here has run on the phone yet (it is unreachable until
2026-10-31). Everything is off by default: the phone's normal start is
unchanged until the device runs below pass.

## Files

| File | |
| --- | --- |
| `sys/arm64/qualcomm/pm/psci_idle.c` | the idle states from the device tree, the choice per idle, OS-initiated mode, statistics (`hw.psci_idle.*`) |
| `sys/arm64/qualcomm/pm/psci_idle_asm.S` | the core's context across CPU_SUSPEND, the warm boot entry `psci_idle_resume` |
| `sys/arm64/qualcomm/pm/arm_tmr_mem.c` | `arm,armv7-timer-mem`: the frame that wakes CPUs whose own timer stops |
| `../patches/0032-arm64-cpu_idle-hook-psci-idle.patch` | `cpu_idle()` asks `arm64_cpu_idle_hook` first; the files for `device psci_idle` |
| `../mfsroot/etc/pro1x-pmtest.sh` | the device tests in the RAM root (`pro1x.test=smp`, `idle`), sourced by `mfsroot/etc/rc` |
| `build.sh` | the kernels: `qemu` (PRO1XQ) and `phone` (PRO1XPM = `freebsd/PRO1X` with `device psci_idle`) into `$WORK/pm-out` |
| `test/qemu/` | PRO1XQ, its device tree (`mkdtb.py`: QEMU's virt with the SM6115's idle topology), its rc, `run.sh` |

`freebsd/build.sh` copies `sys/` into the sources like `dsp/` and `fm/`;
`freebsd/PRO1X` has `device psci_idle` (inert while
`hw.psci_idle.enable=0`).

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

Risks: a hang in a power-down ends in the watchdog's reset (the RAM
console keeps what came before); the hook is set right after `Release
APs` (SI_SUB_SMP), so with `hw.psci_idle.enable=1` the boot only gets
further if the way back works. None of the runs writes to userdata.
