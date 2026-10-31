#!/usr/bin/env python3
"""Decode the small regions of a Qualcomm crash dump of the F(x)tec Pro1X.

  python3 -I tools/devday/crashdump.py ~/ps/pro1x-runs/<day>/ramdump-<time> [...]

A dump is what `qdl ramdump` pulls from the crash-dump mode (USB 05c6:900e)
with the filter of docs/device-day-2026-10-31.md (ways back 4). This reads
the files only; it decodes data, no firmware. docs/crash-dumps.md says
where each layout comes from and what is known and what is not:

  PMIC_PON.BIN  8 bytes, the PM6125 PON block's reason registers 0x8C0-0x8C7
                (GEN2 layout, as qpnp-power-on and kernel patch 0028 read it)
  RST_STAT.BIN  4 bytes, a reset status word the bootloader saved (raw only)
  OCIMEM.BIN    0x0c100000-0x0c129fff: TZ's diagnostic area at 0x0c116000
                (struct tzdbg_t of the vendor kernels' tz_log.c: per-CPU boot
                and reset counters, interrupt counters, TZ's log ring), and
                the shared IMEM at 0x0c125000 (restart reason at 0x65c)
"""
import os
import re
import struct
import sys

OCIMEM_BASE = 0x0C100000
TZ_DIAG = 0x0C116000          # found by its magic "tzda" (0x747a6461)
SHARED_IMEM = 0x0C125000      # diag_dload@c8 holds the serial number there
TIMER_HZ = 19200000           # the timestamps of TZ's log: the 19.2 MHz counter

# PM6125 PON (pmic@0 pon@800, "qcom,pm8998-pon", GEN2 subtype)
PON_REASON = ["hard reset", "SMPL", "RTC alarm", "DC charger", "USB charger",
              "PON1", "cable power (CBL)", "power key (KPD)"]
# POFF_REASON1 and WARM_RESET_REASON1 share these bits (qpnp-power-on.c)
POFF_REASON = ["software (SOFT)", "PS_HOLD (the SoC reset itself)",
               "PMIC watchdog", "keypad reset 1 (GP1)", "keypad reset 2 (GP2)",
               "power key and reset line", "reset line (RESIN, volume down)",
               "long power key hold (KPDPWR)"]
POFF_REASON2 = {3: "charger", 4: "thermal fault (TFT)", 5: "undervoltage (UVLO)",
                6: "overtemperature (OTST3)", 7: "stage 3 reset"}
OFF_REASON = {7: "POFF sequence (reason in POFF_REASON1/2)",
              6: "FAULT sequence (FAULT_REASON1/2)",
              5: "S3 reset sequence (S3_RESET_REASON)"}

# TZ's names for the interrupts it counts, with what the SM6115 device tree
# says about the line (GIC interrupt number = SPI + 32)
TZ_IRQ_NOTE = {
    36: "SPI 4: bite of the APSS watchdog at 0x0f017000 (Linux's qcom_wdt; its bark is SPI 3)",
    306: "TZ's own (secure) watchdog",
    225: "the RPM's watchdog",
    281: "NoC error", 282: "NoC error",
}


def bits(value, names):
    return [names[i] for i in range(len(names)) if value & (1 << i)] or ["none"]


def pmic(d):
    p = open(os.path.join(d, "PMIC_PON.BIN"), "rb").read()
    print(f"PMIC_PON  {p.hex(' ')}  (PON 0x8C0-0x8C7 of the PM6125, GEN2)")
    print(f"  PON_REASON1        0x{p[0]:02x}: {', '.join(bits(p[0], PON_REASON))}")
    print(f"  WARM_RESET_REASON1 0x{p[2]:02x}: {', '.join(bits(p[2], POFF_REASON))}"
          + ("" if p[2] else " (0 = a power-on from off, no warm reset)"))
    print(f"  ON_REASON (0xC4)   0x{p[4]:02x}: bit 6 = warm sequence, bit 7 = power-on sequence"
          " (layout not in the trees here)")
    print(f"  POFF_REASON1       0x{p[5]:02x}: {', '.join(bits(p[5], POFF_REASON))}")
    print(f"  POFF_REASON2       0x{p[6]:02x}: "
          + (", ".join(v for k, v in POFF_REASON2.items() if p[6] & (1 << k)) or "none"))
    print(f"  OFF_REASON         0x{p[7]:02x}: "
          + (", ".join(v for k, v in OFF_REASON.items() if p[7] & (1 << k)) or "none"))


def rst_stat(d):
    v = struct.unpack("<I", open(os.path.join(d, "RST_STAT.BIN"), "rb").read(4))[0]
    print(f"RST_STAT  0x{v:08x}  (the bootloader's copy of a reset status register;"
          " its bits are documented nowhere here: compare dumps only)")


def flexnoc(name, regs):
    """The NoC error logger lines TZ printed, read with the FlexNoC ErrLog
    layout of drivers/soc/tegra/cbb/tegra194-cbb.c (ErrLog0: bit 0 lock,
    4:1 opcode, 10:8 error code, 27:16 length-1). Qualcomm's own layout is
    not public; the address and route fields are what TZ printed."""
    e0 = regs.get("ERRLOG0_LOW")
    if e0 is None:
        return
    code = ["SLV (target error)", "DEC (no target at the address)", "UNS (unsupported)",
            "DISC (target disconnected/powered down)", "SEC (security)", "HIDE",
            "TMO (timeout)", "?"][(e0 >> 8) & 7]
    print(f"  {name}: ErrLog0 0x{e0:08x} (FlexNoC reading: error code {code},"
          f" opcode {(e0 >> 1) & 0xf}, length-1 {(e0 >> 16) & 0xfff}),"
          f" route 0x{regs.get('ERRLOG1_HIGH', 0):x}_{regs.get('ERRLOG1_LOW', 0):08x},"
          f" address 0x{regs.get('ERRLOG2_LOW', 0):08x}, user 0x{regs.get('ERRLOG3_LOW', 0):x}")


def tz(d):
    o = open(os.path.join(d, "OCIMEM.BIN"), "rb").read()
    b = TZ_DIAG - OCIMEM_BASE
    magic, ver, cpus, vmid, boot, rst, intoff, ring, rlen, wake = struct.unpack_from("<10I", o, b)
    if magic != 0x747A6461:
        print(f"TZ diag: no magic at 0x{TZ_DIAG:08x} (0x{magic:08x})")
        return
    print(f"TZ diag at 0x{TZ_DIAG:08x}: version 0x{ver:x}, {cpus} CPUs")
    print("  per CPU: warm boots, power collapses in/out, PSCI calls in/out; TZ's reset record")
    for c in range(cpus):
        wb_in, wb_out, pc_in, pc_out, psci_in, psci_out = struct.unpack_from("<6I", o, b + boot + 40 * c)
        rtype, rcnt = struct.unpack_from("<2I", o, b + rst + 8 * c)
        print(f"    cpu{c}: wb {wb_in}/{wb_out} pc {pc_in}/{pc_out} psci {psci_in}/{psci_out}"
              + (f"   reset type {rtype}, count {rcnt}" if rcnt or rtype else ""))
    n = struct.unpack_from("<I", o, b + intoff - 4)[0]
    print("  interrupts TZ took (count per CPU 0-7), the nonzero ones:")
    for i in range(n):
        e = b + intoff + 56 * i
        num = struct.unpack_from("<I", o, e + 4)[0]
        desc = o[e + 8:e + 24].split(b"\0")[0].decode("ascii", "replace").strip()
        cnt = struct.unpack_from("<8I", o, e + 24)
        if desc and any(cnt):
            note = TZ_IRQ_NOTE.get(num, "")
            print(f"    {desc:15} GIC {num:3} {list(cnt)}  {note}")
    wrap, off = struct.unpack_from("<2I", o, b + ring - 8)
    log = o[b + ring:b + ring + (rlen if wrap else min(off, rlen))]
    lines = log.decode("ascii", "replace").replace("\r", "").split("\n")
    crash = None
    noc = {}
    print("  TZ log from the crash on (time = TZ's counter / 19.2 MHz, from power-on):")
    for ln in lines:
        m = re.match(r"\[([0-9a-f]+)\](.*)", ln)
        if not m:
            continue
        t = int(m.group(1), 16) / TIMER_HZ
        text = m.group(2)
        if crash is None and text.startswith("(700125"):
            crash = t
        if crash is not None and not re.match(r"\((70022d|700389|70023b|700126)", text):
            print(f"    {t:9.3f} s  {text}")
        n = re.match(r"(\w+_NOC) ERROR: (\w+) = 0x([0-9a-f]+)", text)
        if n:
            noc.setdefault(n.group(1), {})[n.group(2)] = int(n.group(3), 16)
    pas = [int(m.group(1), 16) / TIMER_HZ for m in
           (re.match(r"\[([0-9a-f]+)\]\(400160 ", ln) for ln in lines) if m]
    if pas:
        print("  earlier: TZ's (400160 ...) lines (image loads, 3 per boot from Linux: the DSPs) at "
              + ", ".join(f"{t:.2f}" for t in pas) + " s")
    for name, regs in noc.items():
        flexnoc(name, regs)
    if crash is None:
        print("  no crash entry (700125) in TZ's log")


def imem(d):
    o = open(os.path.join(d, "OCIMEM.BIN"), "rb").read()
    b = SHARED_IMEM - OCIMEM_BASE
    serial = o[b + 0xCC:b + 0xD4].split(b"\0")[0].decode("ascii", "replace")
    rr = struct.unpack_from("<I", o, b + 0x65C)[0]
    known = {0x77665500: "bootloader", 0x77665501: "normal reboot", 0x77665502: "recovery",
             0x77665503: "rtc", 0x7766550A: "dm-verity", 0x77665510: "kernel panic"}
    what = known.get(rr) or ("oem" if rr >> 8 == 0x6F656D else "none written (random SRAM)")
    print(f"shared IMEM 0x{SHARED_IMEM:08x}: serial '{serial}', restart reason (0x65c)"
          f" 0x{rr:08x}: {what}")


for d in sys.argv[1:] or ["."]:
    print(f"=== {d}")
    for f in (pmic, rst_stat, imem, tz):
        try:
            f(d)
        except FileNotFoundError as e:
            print(f"  ({e.filename} missing)")
