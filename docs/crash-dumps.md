# Crash dumps of the Pro1X

When the phone ends in Qualcomm's crash-dump mode (USB `05c6:900e`,
"QUSB_BULK"), `qdl ramdump` can pull what the bootloader offers, and taking
it resets the phone. This page says how to take the small part of it, how to
decode it (`tools/devday/crashdump.py`), and what the three dumps of kernel
7.1.0-pro1x.12 on 2026-10-31 say. It ends with the test method for the
candidates that followed (`tools/devday/kexec-test.sh`).

## Taking a dump

```sh
sudo -n qdl ramdump -o ~/ps/pro1x-runs/<day>/ramdump-<time> \
    'OCIMEM*,CODERAM*,DATARAM*,MSGRAM*,PMIC*,PMON*,RST*,FSM*,*.cmm'
python3 -I tools/devday/crashdump.py ~/ps/pro1x-runs/<day>/ramdump-<time>
```

The phone resets a few seconds after the dump and starts boot_b. The RAM
survives that warm reset, so the next Linux start shows the crashed
kernel's console in `/sys/fs/pstore` (collect.sh saves it). It is lost if
another system starts in between: Sailfish OS in slot a clears it.

`load.cmm` lists every region the bootloader offers. The small ones:

| File | Address | What |
|---|---|---|
| `OCIMEM.BIN` | 0x0c100000, 168 KiB | on-chip RAM: TZ's diagnostic area (0x0c116000), the shared IMEM (0x0c125000); the rest is the dump program's own code |
| `CODERAM.BIN`, `DATARAM.BIN` | 0x04600000, 0x04690000 | the RPM's code and data RAM; DATARAM holds the RPM's sleep statistics (`qcom,rpm-stats`, vlow/vmin counts) |
| `MSGRAM.BIN` | 0x045f0000, 28 KiB | the RPM's message RAM: the request FIFOs, the MPM's area at 0x1b8 |
| `PMIC_PON.BIN` | (0x45e97000) | 8 bytes, the PMIC's power-on and reset reasons |
| `PMON_HIS.BIN`, `FSM_*.BIN` | (0x45e97008-dc) | the PMIC's power-on history and state machine (not taken before 2026-11-01) |
| `RST_STAT.BIN` | (0x45e970d4) | 4 bytes, a reset status word the bootloader saved |

The big ones (`DDRCS*`, the whole RAM, gigabytes) are not worth the time
over this USB link: the pstore has the kernel's console.

## Decoding

Everything below is read from data, with layouts from sources that are in
this tree or on this PC. No Qualcomm binary was looked into.

### PMIC_PON: the PM6125's PON reasons

The 8 bytes are the PON peripheral's registers 0x8C0-0x8C7 (pmic@0,
`pon@800`, "qcom,pm8998-pon", subtype GEN2). The register map and bit names
are those of the vendor kernels' `drivers/input/misc/qpnp-power-on.c`
(`~/ps/kernel/sony/msm-4.4/kernel/`, GPL) and of kernel patch 0028:

| Byte | Register | Bits |
|---|---|---|
| 0 | 0xC0 PON_REASON1 | 0 hard reset, 1 SMPL, 2 RTC, 3 DC, 4 USB, 5 PON1, 6 cable power, 7 power key |
| 2 | 0xC2 WARM_RESET_REASON1 | as POFF_REASON1 |
| 4 | 0xC4 ON_REASON | bit 6 warm sequence, bit 7 power-on sequence (not in the trees here) |
| 5 | 0xC5 POFF_REASON1 | 0 software, 1 PS_HOLD, 2 PMIC watchdog, 3/4 keypad resets, 5 power key and reset line, 6 reset line, 7 long power key |
| 6 | 0xC6 POFF_REASON2 | 3 charger, 4 thermal fault, 5 UVLO, 6 overtemperature, 7 stage 3 |
| 7 | 0xC7 OFF_REASON | 7 POFF sequence, 6 FAULT sequence, 5 S3 reset sequence |

PS_HOLD means the SoC itself dropped its "keep me powered" line: a reset
that Linux, TZ or the hardware watchdog logic asked for. A PMIC watchdog, a
held power key, an undervoltage or heat would show other bits.

### RST_STAT

A copy of a reset status register that the bootloader saved. Its bit
meanings are in no source here; compare dumps with each other only.

### OCIMEM: TZ's diagnostic area (0x0c116000)

It starts with the magic "tzda" (0x747a6461) and has the layout of
`struct tzdbg_t` in the vendor kernels' `drivers/firmware/qcom/tz_log.c`
(same tree): version 0x00090003, so the TZ >= 4.x variant with 32-bit
interrupt counters (`struct tzdbg_int_t_tz40`, 56 bytes each, 64 slots).

- **Per CPU:** warm boots, power collapses and PSCI calls (in/out), and TZ's
  reset record (type, count). The type numbers are not documented here.
- **Interrupts TZ took**, with TZ's own names, per CPU. The line numbers
  are GIC numbers (SPI + 32). From the SM6115 device tree: `NSWDogBite`
  (GIC 36 = SPI 4) is the bite of the APSS watchdog at 0x0f017000, the one
  Linux's qcom_wdt drives; its bark is SPI 3, which goes to Linux.
  `SWDogBark` is TZ's own (secure) watchdog, `RPMWDogBite` the RPM's,
  `NOCERR` the interconnect's error interrupts.
- **TZ's log**, a text ring (`[counter](event ...)` and plain lines). The
  counter runs at 19.2 MHz from power-on. TZ writes `(700125)` when it
  starts handling a crash, then lines starting with `S:` (the dump
  program), the SMMU TLB dumps, and the error loggers of the NoCs it finds
  set.
- **NoC error loggers** (TZ prints `ERRLOG0..3`): Qualcomm's layout is not
  public. crashdump.py reads ErrLog0 with the Arteris FlexNoC layout that
  `drivers/soc/tegra/cbb/tegra194-cbb.c` decodes (bit 0 lock, 4:1 opcode,
  10:8 error code, 27:16 length); ERRLOG2 is the address, ERRLOG1 the
  route, ERRLOG3 user bits that name the initiator in a table we do not
  have.

### OCIMEM: the shared IMEM (0x0c125000)

Downstream's `qcom,msm-imem` block: the serial number at 0xcc (in
`diag_dload@c8`) marks it. The restart reason at 0x65c is what a vendor
kernel writes before a reboot; mainline does not write it, so it holds
random SRAM content.

## The pro1x.12 crash of 2026-10-31

Three trial starts of 7.1.0-pro1x.12 (22:56-23:05), dumps
`~/ps/pro1x-runs/2026-10-31/ramdump-2300`, `-2303`, `-2305`. All three say
the same:

- **PMIC:** PON_REASON1 0x41 (hard reset, cable power), WARM_RESET_REASON1
  0x02 (PS_HOLD), ON_REASON 0x40 (warm), POFF_REASON1 0x02 (PS_HOLD),
  OFF_REASON 0x80 (a POFF sequence). A warm reset the SoC asked for; no
  PMIC watchdog, no key, no undervoltage, no heat.
- **RST_STAT** 0x00000003 each time.
- **IMEM:** no restart reason written (as always with mainline).
- **TZ** started its crash handling at 46.66 s, 46.96 s and 46.32 s after
  power-on (TZ's counter): one run and the next differ by less than a
  second. It took these interrupts, the same in all three:

  | Interrupt | CPUs | Meaning |
  |---|---|---|
  | NSWDogBite (SPI 4) | 2 and 4 | **the APSS watchdog bit**: Linux's watchdog went unfed |
  | SWDogBark | 2 (1 in -2303) | TZ's own watchdog barked |
  | RPMWDogBite | 2 | the RPM's watchdog bit |
  | NOCERR (GIC 282, 281) | 2, 4 | one error of the config NoC, one of the system NoC |
  | SGIFtlErr, SGIReset | 0 / 0, 4 | TZ's own fatal-error and reset signals between the CPUs |

  CPU 2 has TZ's reset record (type 1, count 1). No SMMU fault, no XPU or
  VMIDMT violation, no RPM error indication was counted.
- **TZ's log:** the crash entry, the dump program (`S:Debug reset main
  enter` ... `S:-DEAD END-`), the TLB dumps, then one error in each NoC:
  - CONFIG_NOC: ErrLog0 0x00130019, route 0xe00e_00000034, address
    0x00080040, user 0x29
  - SYSTEM_NOC: ErrLog0 0x00010019, route 0xe00e_000000a6, address
    **0x06080040**, user 0x29

  0x06080000 is the modem subsystem's register block (`remoteproc@6080000`,
  the MPSS). Linux maps nothing there (`qcom_q6v5_pas` uses only the
  memory regions); TZ does, when it starts or stops the modem
  (`qcom_scm_pas_auth_and_reset`, `qcom_scm_pas_shutdown`) and when its
  dump program looks at the modem ("Skip encryption as NS device: MPSS").
  With the FlexNoC reading the error code is 0 (SLV, the target answered
  with an error).
- **Earlier in TZ's log:** the bootloaders' image loads at 1.8-2.1 s, and
  three more at 21.2-21.7 s, by their count and time most likely Linux
  starting the three DSPs (ADSP, CDSP, modem) through TZ (TZ does not name
  them). Nothing else reached TZ's log until the crash.

### What follows from it

- **The reset was the APSS watchdog the trial armed** (the boot menu arms
  it, qcom_wdt takes it, dsme feeds it in Leste). TZ handles that bite as
  a crash: it dumps and resets into the crash-dump mode. That is why the
  phone stopped in 900e instead of rebooting.
- **The watchdog's last feed** was one timeout before the bite. Leste's
  dsme sets 14 s on `/dev/watchdog0` (qcom_wdt there, `maemo-leste/dsme`,
  `dsme-wdd-wd.c`) and feeds it every 12 s; before dsme opens it, the
  kernel feeds it every 15 s with 30 s. So the last feed was at about
  32.5 s (dsme) or 16.5 s (kernel), and Linux was still running at 21.7 s
  (the DSP starts). Whatever stopped the feeding started between about 6
  and 29 s after the USB gadget came up (which was at about 15.5 s), and
  it stopped the kernel or dsme for good.
- **More than a stuck task:** TZ's own watchdog barked and the RPM's bit,
  which Linux cannot cause by hanging in a task. Both point to a CPU or the
  bus being stuck in a transaction that does not complete (an access to a
  block whose clock or power is off), or to TZ busy with the modem; the
  NoC errors at the modem's registers fit TZ stopping or starting the
  modem with the modem's side not answering. TZ's counters do not say
  which came first: the NoC errors may also be the dump program's own
  look at a modem that was already down.
- **The same end once before:** pro1x.5 on 2026-10-31 18:56 (no dump
  taken; `pstore-1856.txt`): the modem reported a fatal error (WLAN
  process, "Task starvation"), "crash detected in modem", and the kernel
  log stopped there; then the trial's watchdog and 900e. Mainline's
  `qcom_pas_stop()` calls `qcom_scm_pas_shutdown()` without the proxy votes
  (XO, CX) that the modem dropped at its handover; downstream votes them
  before the shutdown. That is a candidate for "a modem restart hangs the
  phone" in every kernel here, pro1x.5 too, and would make a modem crash
  early in pro1x.12's boot end exactly like this.
- **What the dumps cannot say:** what Linux was doing. The pstore of these
  starts was lost (Sailfish started in between). The next one is the
  thing to get.

## Suspects for pro1x.12

pro1x.12 = 0001-0035, 0038-0040; the phone ran pro1x.5 = 0001-0017 (0017,
the FM debug patch, was applied before pro1x.5's build; its radio module
has the patch's strings). pro1x.6-.11 never ran on the phone. 0040 acts
only at a kexec (`kexec_in_progress`), so at boot pro1x.12 is pro1x.11.
The boot image of 10-31 also carries a newer initramfs (the boot menu
reads PMIC registers for the alarm check through debugfs; the file-system
options were switched back with `/host/pro1x-fs-defaults` for the trial).

In order:

1. **0023, 0024, 0029, 0030: the Bluetooth UART's power.** The screen
   showed `Bluetooth: ... command tx timeout` before the end, and this is
   the new code that runs at every boot in that window: the kernel sets
   the WCN3950 up when hci_uart probes and, unless BlueZ powers the
   adapter (AutoEnable=false since aa36b18, if 71-power-userspace.sh has
   run in Leste), powers it down again 2 s later (HCI auto-off). With
   0029 that power-down now sends the chip's power-off pulse and drops
   Bluetooth's regulators, which the chip's Wi-Fi part shares; the WCN3950's
   Wi-Fi is run by the modem's WLAN process. With 0023/0024 the GENI UART
   is switched off (clock, CX and interconnect votes) while the port is
   open, and every register access in that state stalls the CPU on the bus
   (which also fits TZ's watchdog bark). 0030 moves the RX wake-up
   interrupt to the port's open and close.
2. **0018-0021: SLIMbus.** 0021 enables new hardware, the LPASS SLIMbus
   NGD (0x0a5c0000) and its BAM DMA, with no IOMMU stream known, plus the
   WCN3950's SLIMbus devices; it comes up with the ADSP, which started at
   21.2-21.7 s.
3. **0038/0039: the ADSP's sleepstate entry** (a new outbound SMP2P entry
   to the ADSP, set at probe).
4. **0022: glink endpoint destroy** (only on a remove, which a DSP restart
   does).
5. The rest acts later or elsewhere: 0033 (only while the touchscreen is
   inhibited), 0031 (only at a freeze), 0032 (only at a resume), 0025
   (battery polling pace), 0026 (amplifier order), 0027/0034 (device-tree
   audio), 0028 (PON reasons read at probe), 0035 (period size).
6. The initramfs of 10-31 (see above); unlikely, and every candidate
   below boots with it too.

## Testing a kernel without flashing it

The Pro1X's ABL has no `fastboot boot` (docs/HANDOFF.md), so the only way
to start a kernel without writing boot_b is kexec from a running Linux:
`tools/devday/kexec-test.sh` (from the PC; `check`, `load`, `go`, `after`,
`unload`, see its header). The candidate starts with its own device tree
(with the live memory banks; the script refuses a ramoops node that
differs from the running one), its initramfs and the running command line
plus `rcupdate.rcu_cpu_stall_timeout=5` and `ignore_loglevel`. Before the
jump it sets a one-shot "leste", so the candidate's boot menu arms the
watchdog and loads qcom_wdt as on trial, and dsme takes it over: the same
watchdog that ended pro1x.12's starts. A hang ends in the watchdog's reset
into boot_b (pro1x.5, Leste) or in 900e, which `after` (OK=yes) leaves with
the dump; either way the next Leste has the candidate's console in the
pstore. The watchdog's bark (1 s before its bite) prints `watchdog0:
pretimeout event` while Linux still takes interrupts; a pstore that ends
without it means Linux was frozen.

Limits:

- On 2026-10-28 every kernel kexec'd from a running Leste stopped about
  0.23 s into its boot, right after the I/O schedulers
  (docs/kernel-update.md), the same kernel into itself too; the next line
  of a cold boot is the MPM's probe. So run 0 is a calibration: K=pro1x.5
  into itself with `NOSERIAL=1 EXTRA="ignore_loglevel initcall_debug"`,
  which names the initcall it stops in (NOSERIAL drops the 115200-baud
  serial console, whose thousands of lines could otherwise outlast the
  watchdog; runs 1-4 keep it as the crashed starts had it). `after` tells
  the test's start from boot_b's by the marker `pro1x.kexectest=` on the
  command line. A candidate tested before a run gets past that point says
  nothing about the candidate; if run 0 stops there again, the only test
  left is a flash with a trial.
- The running system's state carries over: the DSPs keep running, the
  display and UFS stay powered (0004/0005), and pro1x.5's SMMU shutdown
  sets sCR0.CLIENTPD (no translation for any stream, unless the hypervisor
  steps in) while the DSPs may still do DMA. A crash under kexec may come
  from that; a clean run is good, not perfect, evidence.

### The candidates (built 2026-11-01, `$WORK/out-<name>`)

Built with `scripts/30-build-kernel.sh` in a reflink copy of the kernel
tree (`$WORK/kbuild-bisect/build/linux`, pro1x.12's commit plus `git
revert`s; `reverted.txt` in each output lists them). No warnings. Each
still needs `65-kernel-update.sh prepare` (modules to the phone, the
initramfs made in Leste; no partition written).

| Name | pro1x.12 without |
|---|---|
| `pro1x.12nobt` | 0023, 0024, 0029, 0030 |
| `pro1x.12no29` | 0029 |
| `pro1x.12nobtslim` | 0023, 0024, 0029, 0030, 0021 (no SLIMbus nodes) |

### Run order

| Run | Kernel | If it stays up (5 min) | If it goes down |
|---|---|---|---|
| 0 | pro1x.5 into itself, `NOSERIAL=1`, `initcall_debug` | kexec works: go on | kexec from Leste does not work here (the console names the initcall); stop, the candidates need another way |
| 1 | pro1x.12 | the crash needs a cold start (or the trial's boot): read run 1's logs anyway | the pstore shows what happened: often that alone names the patch |
| 2 | pro1x.12nobt | the Bluetooth set is it: run 3 | not the Bluetooth set (alone): run 4 |
| 3 | pro1x.12no29 | 0029 (the WCN3950 power-off) | 0023/0024/0030 (the UART switched off while open) |
| 4 | pro1x.12nobtslim | SLIMbus (0021), or the two together | neither: 0038/0039 next, then the initramfs |

Every run is [OK]: `OK=yes kexec-test.sh go <name>`. Stop at the first run
whose pstore names the cause.
