# Alarms that switch the phone on

An alarm set in Maemo Leste's (or FreeBSD's) clock rings even when the
phone was switched off: the PMIC's RTC alarm powers the phone on, the boot
menu sees that and starts the system that set the alarm at once, without
the menu, and that system rings. Once the alarm is stopped it asks whether
to switch the phone off again, as the N900 asked whether to switch it on
in its "acting dead" mode.

Status (2026-10-30): built and tested on the PC (below); the power-on
itself is untested on the phone. To be flashed with kernel pro1x.10.

## Is it possible on this hardware?

Yes, as far as the sources show; one register is to be checked on the
phone (step 1 of the test).

| Question | Answer | Source |
| --- | --- | --- |
| Which PMIC has the power-on (PON) block and the RTC? | PM6125, SPMI slave 0: PON at 0x800 (`qcom,pm8998-pon`, a GEN2 PON), RTC counter at 0x6000, RTC alarm at 0x6100 (`qcom,pm8941-rtc`) | `arch/arm64/boot/dts/qcom/pm6125.dtsi` (mainline 7.1); the vendor kernel's `pm6125.dtsi` (LineageOS `android_kernel_fxtec_sm6115`) has the same addresses |
| Is the RTC alarm a power-on source? | `PON_TRIGGER_EN` (0x880), bit 2 (`PON_RTC` in the vendor kernel's `enum pon_trigger_source`). Neither the vendor kernel (`qpnp_pon_trigger_config()` has no caller) nor mainline writes it: it is as the PMIC and the boot loaders leave it. Qualcomm's own RTC driver of the same PMIC generation (`qpnp-rtc.c`, sm8150) switches the alarm off at shutdown unless a power-off alarm is wanted (`qcom,qpnp-rtc-alarm-pwrup`, `poweron_alarm`): an alarm left on powers the device on, with nothing else configured. The Pro1X's vendor kernel uses `rtc-pm8xxx`, which leaves it on. | vendor sources; `pro1x-boot pmic` shows the bit on the phone ("power-on sources") |
| Does the alarm survive the power off? | Yes: the RTC is in the PMIC's always-on part (it keeps counting while off; `pro1x-rtc` relies on that). Linux's `rtc-pm8xxx` shutdown writes only an nvmem/UEFI offset (none here); the RTC core keeps a set wake alarm; Leste's `hwclock --systohc` at shutdown gets -ENODEV (no `allow-set-time`) and touches nothing. FreeBSD's `qcom_rtc` leaves it on too ("as Linux keeps it", `freebsd/pm/README.md`); FreeBSD's alarmd switches it off only when no alarm needs it (`patches/alarmd/0006`). | `drivers/rtc/rtc-pm8xxx.c`, `drivers/rtc/interface.c` |
| How is "powered on by the RTC" seen? | `PON_REASON1`, bit 2 (GEN2 at 0x8c0, GEN1 at 0x808); `WARM_RESET_REASON1` (0x8c2) 0 means a cold start. The vendor kernel reads them at probe, after XBL and ABL, and logs "Power-on reason: Triggered from RTC (RTC Alarm Expiry)": they are still valid then. ABL reads them too (`GetPonReason`) but clears only the reboot reason (`ClearResetReason`, the soft reset spare register), not these. | vendor `qpnp-power-on.c` (`PON_OFFSET`, `qpnp_pon_reason[]`); Qualcomm ABL `LinuxLoader.c` |
| What does ABL boot after an RTC power-on? | The active slot's boot partition, as for any power-on: boot_b, the menu. ABL's "alarm boot" (`androidboot.alarmboot=true`) is a different thing, a reboot reason (0x03) Android writes before a reboot. The only special case of a cold start is off-mode charging (`androidboot.mode=charger` when the charger caused it), which loads the same boot image; this kernel ignores `androidboot.*`. The start is one of the slot's tries, as any start (the trial below is not changed). | Qualcomm ABL (`QcomModulePkg`: `UpdateCmdLine.c` `TargetPauseForBatteryCharge`, `LinuxLoader.c`); the Pro1X's own ABL is F(x)tec's build of it, not public |

## How it works

1. **The alarm is set.** clock-ui's alarms have `ALARM_EVENT_ACTDEAD`;
   alarmd puts the RTC alarm 60 s before the next such alarm. On Leste
   through `/dev/rtc0` and `librtc-realtime` (`tools/pro1x-rtc`), on
   FreeBSD through `dev.qcom_rtc.0.wakealarm`.
2. **Power off.** Nothing to do. (Leste's `/etc/init.d/pro1x-rtc` used to
   set a one-shot `pro1x-boot next` for the running system at a shutdown
   with an alarm; that also sent starts by the power key there, and is
   gone.)
3. **The RTC powers the phone on.** XBL, ABL, boot_b's kernel, the
   initramfs.
4. **The boot menu** (`initramfs/local-top/pro1x-loop-root`) asks
   `pro1x-boot alarm-check` first. It reads the PMIC through its regmap in
   debugfs (`/sys/kernel/debug/regmap/0-00/registers`, a 9-byte line per
   register; no module and no kernel patch needed, the menu runs before
   `qcom-pon` and `rtc-pm8xxx` load; only reads): the PON subtype, the
   reasons, the power-on sources, the RTC counter and the alarm.
   - **Whose alarm?** The RTC has one alarm, and the system that set it
     owns it. `/host/pro1x-boot/rtc-alarm` notes the alarm's count at each
     start (`count owner [used]`). A count that changed since the
     previous start was set by the system that ran in between
     (`/host/pro1x-boot/current`, which the menu writes for every start,
     FreeBSD's and KolibriOS's too). So FreeBSD, which never writes
     userdata, needs nothing at its shutdown, and a system that only left
     another one's alarm alone does not take it over.
   - **The alarm's start:** powered on by the RTC (`PON_REASON1` bit 2),
     the counter at most 10 minutes past the alarm's count, the owner
     installed, this alarm not used for a start before. Then the owner
     starts at once (no menu, a snapshot owner `arch@N` as its snapshot),
     with the watchdog of a one-shot choice; a pending `pro1x-boot next`
     stays for the next start (unless it names the same system).
   - It notes `/host/pro1x-boot/alarmboot` (`system=`, `rtc_alarm=`,
     `rtc_now=`, `due=`: the alarm's real time, count + the shared RTC
     offset + 60 s), removed again at every other start. FreeBSD gets
     `pro1x.alarmboot=1 pro1x.alarmdue=<time>` in its kernel environment.
   - Anything else (power key, charger, reboot, PMIC unreadable): the menu
     as before. Every start logs one line to `/host/pro1x-boot/last-boot`,
     e.g. `alarm-check: none not-by-the-rtc owner=leste late=-3500 pon=80
     warm=00 poff=.. sources=86 rtc=... alarm=... on=1 last=leste prev=...`
     or `alarm-check: leste late=14 due=... pon=04 ...`.
5. **The system rings.**
   - **Leste:** alarmd and the system UI ring as always (a clock-ui alarm
     has `ALARM_EVENT_POSTPONE_DELAYED`: even one that comes late after a
     slow start rings, up to a day late). `pro1x-alarmboot` (package
     `pro1x-alarmboot`, started by `Xsession.post/48pro1x-alarmboot`)
     watches the alarm dialog (`alarm_dialog_status` on the system bus).
     Should none come within 2 minutes of the alarm's time while alarmd
     still has an alarm then (or does not answer), it rings by itself: a
     dialog and a tone through PulseAudio, until Stop or for 5 minutes.
     Once the alarm dialog is gone (Stop, or Snooze, which sets the RTC
     for the snooze and switches the phone on again), it asks "Switch it
     off again?"; no answer in 60 s keeps the phone on. An alarm that was
     deleted after the RTC was set (Leste's alarmd leaves the RTC alarm
     then) does not ring; the question comes all the same.
   - **FreeBSD:** `rc.d/pro1x_alarmboot` (package `pro1x-alarmboot`)
     writes the note `/var/run/pro1x-alarmboot` and starts the Hildon
     session (`startx hildon`, as root, as on the console). alarmd and the
     system UI ring there; alarmd's queue is kept only on the writable
     pool (`pro1x.rootrw`), with the read-only root `pro1x-alarmboot` rings
     by itself after 2 minutes, then asks; "Switch off" is
     `shutdown -p now`.
   - **MeeGo** (Harmattan's session): timed rings in its own session (its
     wake alarms go through `librtc-realtime` too); no switch-off
     question.
   - **Arch, Kali, LuneOS** set no RTC alarms (Arch builds libalarm only
     for its calendar). If something sets one there (`rtcwake`), the menu
     starts that system all the same.

Kernel patch 0028 (`device/kernel/patches/0028-power-reset-qcom-pon-...`,
in pro1x.10) adds what the running systems may want: `qcom-pon` logs the
reasons at boot (`dmesg | grep "power-on reason"`) and shows them in
`/sys/bus/platform/drivers/qcom-pon/*/` as `pon_reason`,
`warm_reset_reason`, `poff_reason`; `rtc_power_on` reads the RTC bit of
`PON_TRIGGER_EN`, and writing 1 or 0 changes only that bit (a
read-modify-write; no power key, reset, S2 or PS_HOLD setting). The boot
menu does not need it.

`pro1x-boot pmic` (root, any Linux system with debugfs) prints all of it:

```
power-on reason: 0x04 (RTC alarm), cold boot (warm reset reason 0x00), power-off reason 0x.. (PON subtype 04)
power-on sources: 0x86 (SMPL, RTC alarm, power key)
RTC: counter 50000014, alarm at 50000000 (on, in -14 s)
the alarm's owner (count owner [used]): 50000000 leste used
this start by the alarm: system=leste rtc_alarm=50000000 rtc_now=50000014 due=... 
```

## On the phone, 2026-10-31

What goes onto the phone: kernel pro1x.10 (with 0028; built together with
the power patches 0029 and up), the boot image `65-kernel-update.sh
prepare` makes from this repository (the initramfs scripts and
`pro1x-boot` above), Leste's `pro1x-rtc` service (prepare installs it with
`pro1x-boot`, `66-boot-trial-install.sh`) and the `pro1x-alarmboot`
package. **Writing boot_b needs the owner's OK on the day.**

0. Optional, before flashing, with pro1x.9 running: the register reading
   alone. Copy `tools/pro1x-boot/pro1x-boot` to `/tmp` and run
   `sh /tmp/pro1x-boot pmic` as root: a start by the power key shows
   `power-on reason: 0x80 (power key)`; "power-on sources" must name
   "RTC alarm".
1. Build and prepare (PC, Leste running):
   ```sh
   KLOCAL=-pro1x.10 OUT=$WORK/out-pro1x.10 WORK=$WORK bash scripts/30-build-kernel.sh
   PHONE="ssh root@172.16.42.1" OUT=$WORK/out-pro1x.10 WORK=$WORK \
       bash scripts/65-kernel-update.sh prepare
   ```
2. With the OK: `... 65-kernel-update.sh flash` (saves boot_b as
   `/host/boot_b-before-7.1.0-pro1x.10.img`, writes, puts slot b on trial
   with 3 tries), then reboot. The trial and the way back are those of
   [kernel-update.md](kernel-update.md): a start that does not come up
   ends in Sailfish after three tries; back with
   `TARGET=b IMG=/host/boot_b-before-7.1.0-pro1x.10.img VBMETA= sh scripts/60-flash-slot-b.sh flash`
   or `fastboot flash boot_b $OUT/boot_b-before-7.1.0-pro1x.10.img`.
3. The package, from the pre-release `leste-alarmboot-2026.10.30`
   (built by `scripts/mkdeb-tools.sh`, `ONLY=pro1x-alarmboot`):
   ```sh
   gh release download leste-alarmboot-2026.10.30 -R smatkovi/pro1x-maemo-leste -D /tmp/alarmboot
   (cd /tmp/alarmboot && sha256sum -c SHA256SUMS)
   scp /tmp/alarmboot/pro1x-alarmboot_1.0_arm64.deb root@172.16.42.1:/root/
   # in Leste, as root
   apt install /root/pro1x-alarmboot_1.0_arm64.deb
   ```
   then log out of Hildon or reboot. FreeBSD gets `pro1x-alarmboot` and
   the new `hildon-session` with the next FreeBSD Hildon release.

## The test on the phone

1. `pro1x-boot pmic` (root): "power-on sources" names "RTC alarm". If
   not: `echo 1 > /sys/bus/platform/drivers/qcom-pon/*/rtc_power_on`, and
   after the next power off and on, `pro1x-boot pmic` again: whether the
   PMIC kept it (if not, the rc scripts would have to set it at every
   start; FreeBSD has no such switch yet).
2. **Leste:** clock, an alarm 5 minutes ahead. `cat
   /sys/class/rtc/rtc0/wakealarm` is a count (the RTC's own, not the real
   time) and `pro1x-boot pmic` says `alarm at ... (on, in ~240 s)`.
   Switch off with the power key menu. Expected: about 60 s before the
   alarm the phone switches itself on, no boot menu, Leste starts, the
   alarm dialog rings at the alarm's time; Stop, then "Switch it off
   again?": Switch off, and the phone is off.
3. **FreeBSD:** the same from FreeBSD's Hildon (`startx hildon`; with
   the writable pool alarmd keeps the alarm, else `pro1x-alarmboot`
   rings 2 minutes after its time), then `shutdown -p now`. Expected: the
   phone switches on, FreeBSD starts at once (kexec), Hildon comes up by
   itself, it rings, then the question.
4. **Counter-checks:** switched on by the power key before an alarm: the
   menu as always (`last-boot`: `alarm-check: none not-by-the-rtc`). A
   reboot right after an alarm's start: the menu (`alarm-used-already`).

If it does not work:

| What happens | Where to look |
| --- | --- |
| The phone stays off | `pro1x-boot pmic` before the power off: the alarm `on`, in the future? "power-on sources" with "RTC alarm"? (step 1) |
| It comes on, but the menu shows | `/host/pro1x-boot/last-boot`, the `alarm-check:` line: `pmic-unreadable` (no debugfs, no `0-00` regmap), `not-by-the-rtc` (the `pon=` value: another reason bit), `not-this-alarm` (`late=` beyond 600 s or negative), `owner-not-installed`, `alarm-used-already`; `/host/pro1x-boot/rtc-alarm` (who owns it) |
| The system starts, nothing rings | Leste: `/host/pro1x-boot/alarmboot` there? `pro1x-alarmboot -v` by hand in the session's terminal; alarmd's queue (`alarmclient -i`, package alarmclient); FreeBSD: `kenv pro1x.alarmboot`, `/var/run/pro1x-alarmboot`, `/var/log/startx.log` |
| The question to switch off does not come | `pro1x-alarmboot -v`: it waits for the alarm dialog to show and go (or for its own ringing to stop) |

## Tested on the PC

`tests/alarm-boot/run.sh` (2026-10-30, 92 checks, 0 failures):

- `pro1x-boot alarm-check` on faked PMIC registers (a 65536-line file of
  the regmap's form, read with `dd` at 9 * register as on the phone),
  under the PC's sh and under the initramfs's own busybox 1.35 (aarch64,
  through qemu-user): an alarm set in Leste, the same alarm a second time,
  a start by the power key, FreeBSD's alarm left alone by Leste
  afterwards, an Arch snapshot as the owner and one that is gone, an
  owner that is not installed, 20 minutes late, before the alarm, no
  alarm, the GEN1 layout and a count above 2^31, the PMIC unreadable or
  of another form; `pro1x-boot pmic`'s report.
- The addresses against Linux 7.1 (`rtc-pm8xxx`'s `pm8941_regs`,
  `pm6125.dtsi`) and kernel patch 0028's.
- `initramfs/local-top/pro1x-loop-root` itself, in the initramfs (its
  busybox, its `/scripts/functions`) in a bubblewrap sandbox with
  `blkid`, `mount`, `losetup`, `kexec` and the menu stubbed: the menu after
  a power-key start; Arch at once after its alarm, with the watchdog, the
  one-shot choice kept; that choice (FreeBSD) at the next start by kexec
  without the alarm words; FreeBSD's alarm with `pro1x.alarmboot=1
  pro1x.alarmdue=...`; a snapshot's alarm; an old `next` for the alarm's
  system taken.
- Kernel 0028 built with `W=1` without warnings (test kernel
  7.1.0-pro1x.10-alarm, pro1x.9 + 0028); `pro1x-alarmboot` built for
  FreeBSD against the Hildon sysroot and for Leste in its chroot.

```sh
INITRD=$WORK/out-pro1x.5/initrd.img-7.1.0-pro1x.5 LINUX=$WORK/build/linux \
    bash tests/alarm-boot/run.sh
```

Not tested: anything on the phone (the PMIC registers through debugfs,
the power-on itself, ABL's behaviour after it, the ringing and the
switch-off question in a real session), and FreeBSD's side in the VM
(the VM was in use).

## Limits

- One RTC alarm for all systems: the system that set it last owns it.
  Sailfish OS (slot a) keeps its own alarms; an alarm while slot a is
  active starts Sailfish.
- `alarm-check` does not look at the warm reset reason: should a later
  restart within 10 minutes of an alarm still show the RTC bit in
  `PON_REASON1` (whether the PMIC keeps it over a reset is to be seen on
  the phone), and that alarm's count be a new one (an alarm that rang
  while the phone was on), that start goes straight to the system that
  ran anyway, and `pro1x-alarmboot` there asks to switch off. Each count
  starts a system only once.
- Leste's alarmd leaves the RTC alarm of a deleted alarm set: the phone
  then switches on for nothing, `pro1x-alarmboot` does not ring (alarmd
  has no such alarm) and only asks to switch off again.
