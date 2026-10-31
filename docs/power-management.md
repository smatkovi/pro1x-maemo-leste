# Power management on the mainline kernel

Measurements and changes for standby time in Maemo Leste and the MeeGo
session (both on the mainline kernel 7.1), and the protocol for the tests
that need someone at the device.

## Measured (2026-10-25, MeeGo session, display off, on USB)

| What | Before | After |
|---|---|---|
| SoC sleep states (`/sys/kernel/debug/qcom_stats/vlow`, `vmin`) | 0 and 0 after 6 h | still 0: nothing suspends yet (see below) |
| pro1x-steps CPU time | one core at 100 % all day | 0 s (libssc fixes, `f01565a`) |
| Harmattan processes | six of them at 14.6 wake-ups/s each, n9-sensors 28/s | each below 1/s (`313d828`) |
| ADSP `q6v5 handover` interrupt | 4/s, every one logged to the flash | only while the accelerometer streams, not logged |
| tonegend | refused by PulseAudio every 10 s | connected |
| Interrupts in all | about 250/s | about 170/s, mostly timers and IPIs |

Wake-ups per process and interrupt can be measured with this (as root,
display off; it prints the top 20 processes and top 15 interrupts):

```sh
dbus-send --system --type=method_call --dest=com.nokia.mce \
  /com/nokia/mce/request com.nokia.mce.request.req_display_state_off
tools/pro1x-autosuspend/wakemeas.sh 60
```

## What keeps the SoC awake

- **Nothing suspends the system.** The kernel offers s2idle only
  (`/sys/power/mem_sleep`). Android suspends whenever nothing holds a wake
  lock; Leste and the MeeGo session never do. `tools/pro1x-autosuspend`
  does that now, but it is not enabled until the tests below pass.
- **Bluetooth holds the CX rail at "nominal"** as long as hci0 is up: the
  Bluetooth UART (`4a8c000.serial`) is the only CX performance vote left
  with the display off (`/sys/kernel/debug/pm_genpd/pm_genpd_summary`);
  `hciconfig hci0 down` drops it to 0. Android switches the UART clock off
  while the Bluetooth chip sleeps (in-band sleep); in mainline `hci_qca` that
  hook (`__serial_clock_off`) is an empty TODO, and qcom_geni_serial only
  has its wake-up interrupt for system suspend. It even stayed powered in a
  suspend: the open serdev port keeps it up as a runtime PM child. Patches
  0023 and 0024 (7.1.0-pro1x.7) switch it off between the in-band sleeps
  and for a suspend, see "The Bluetooth UART between in-band sleeps".
- **Too many wake-ups for cluster sleep.** The CPU clusters' deeper idle
  state was rejected in over 99 % of the attempts (`Rejected` equal to
  `Above` in `/sys/kernel/debug/pm_genpd/power-domain-cpu-cluster*/idle_states`):
  the next interrupt always came too soon.
- **upowerd read the battery every 5 s** (each `voltage_now` is an ADC
  conversion of about 60 ms) and broadcast `PropertiesChanged` each time,
  waking every subscriber. It now runs with `NoPollBatteries=true`; on USB at
  99 % it still refreshes every 5 s (an unknown discharge rate makes upower
  poll), which has to be checked again on battery.
- **Modem messages did not wake the system.** The glink-smem interrupt is
  `IRQF_NO_SUSPEND` but no wake-up source, so a call or an SMS would have
  waited for the next wake-up. Kernel patch 0003 makes the modem edge
  (`wakeup_edges=mpss`) a wake-up source; the module is installed in Leste,
  the MeeGo copy and Kali and loads at the next boot.

## Test protocol (with someone at the device)

First the current `pro1x-autosuspend` (see "Changes after the first
suspends") compiled in Leste and in the MeeGo copy: the `gcc` line of
`scripts/46-sensor-stack.sh`. An older `/etc/pro1x-autosuspend.conf` may
stay: the new settings have their defaults in the program.

Over Wi-Fi (USB gets disconnected when unplugged), display off:

1. After the reboot that loads the new `qcom_glink_smem`:
   `cat /sys/kernel/debug/wakeup_sources | grep glink` lists the modem edge.
2. One suspend with an RTC wake-up, unplugged (USB does not come back
   from a suspend yet): `pro1x-autosuspend --test 20`. Expected: "resumed
   after ~20 s asleep", wake-up interrupt of the RTC alarm, no
   `sensor_process` crash in `dmesg`. If it does not come back, the power
   key has to wake it. Afterwards the sensors must still work:
   `rc-service pro1x-hexagonrpcd status` is started again,
   `qrtr-lookup | grep ' 400 '` lists the sensor service, and the step
   counter still counts (`dbus-send --system --print-reply --dest=org.pro1x.Steps
   /org/pro1x/Steps org.pro1x.Steps.GetSteps` before and after a few
   steps).
3. Whether the SoC sleeps: the same line reports `vlow +N, vmin +N`. If both
   stay 0, repeat with `hciconfig hci0 down` to find what still holds it.
   With the MPM device tree (below) first check that the rerouted
   interrupts still work: power key, volume keys, keyboard slide, touch,
   and `grep mpm /proc/interrupts` lists the SPMI arbiter and the GPIOs.
   Then: `vlow`/`vmin` going up proves the MPM hand-over to the RPM;
   +0 after a good resume means another master (modem, ADSP, CDSP) or an
   RPM vote (a clock, a regulator, an interconnect) still holds the SoC
   up, the next thing to find (`/sys/kernel/debug/qcom_stats/` per
   subsystem: `modem`, `adsp`, `cdsp` sleep counts).
4. Power key: `pro1x-autosuspend --test 0`, then press the power key.
5. Incoming call and SMS during a `--test 0` suspend: the modem edge must
   wake the system (interrupt "glink-smem").
6. Standby current: unplugged, display off, the daemon running
   (`rc-service pro1x-autosuspend start`), for 15 minutes; then
   `grep pro1x-autosuspend /var/log/syslog` for the sleeps and their wake-ups
   and the battery's `current_now` before and after.
7. If all of this works: `rc-update add pro1x-autosuspend default`.

Open after that: Wi-Fi WoWLAN (the chip supports magic packet and pattern
wake-ups; without it mac80211 disconnects at every suspend, hence
`StayAwake=45`), the Bluetooth UART clock while awake, and sensor batching
for the step counter during suspend (done 2026-10-31, untested on the
phone: [sensors.md](sensors.md)).

## First suspends (2026-10-25)

`pro1x-autosuspend --test N` on the device, on USB:

1. Within the first minutes after boot every suspend was aborted by the
   modem's glink edge: oFono's start-up talks to the modem. Quiet after
   about 4 minutes (0 messages a minute).
2. Suspending itself made the modem talk too (Wi-Fi firmware and IPA go
   down over QMI), which aborted every suspend: the glink edge now raises
   wake-up events only from the noirq phase on (patch 0003).
3. The next suspend succeeded, but:
   - the Bluetooth UART's wake-up interrupt (`4a8c000.serial:wakeup`)
     woke the system at once (0.0 s asleep, vlow/vmin +0);
   - the ADSP's `sensor_process` crashed on the way (fatal error over
     FastRPC, remoteproc restarted the ADSP) and the audio services were
     left timing out (q6afe/q6asm -110) until the next boot; probably the
     ADSP calling into the frozen hexagonrpcd;
   - the USB gadget did not come back ("HS-PHY not in L2" from
     dwc3-qcom-legacy, UDC "not attached"; neither rebinding the gadget nor
     toggling the USB role helped). Only a reboot brings USB back; Wi-Fi
     came back by itself.

So pro1x-autosuspend stays disabled until the sensor stack survives a
suspend, Bluetooth is no longer an immediate wake-up and USB resumes.
The causes of all three, found later in that log and the kernel's code,
and their fixes for pro1x.10: "Power fixes for pro1x.10" below.

## The MPM (2026-10-26, built, untested)

On RPM-based Qualcomm SoCs the SoC's system sleep states (XO shutdown,
vlow; vmin) need the MPM: while the SoC sleeps the GIC is off, and the
MPM, a small interrupt controller in the always-on domain, watches the
wake-up interrupts and hands them over to the RPM. The last CPU going
down gives the MPM its list of enabled wake-up interrupts and signals the
RPM. Mainline's `sm6115.dtsi` has no MPM node (the pinctrl driver has the
SM6115's GPIO map since 2023, the device tree was never hooked up), so the
RPM never puts the SoC to sleep, which matches vlow and vmin staying 0.
Android's kernel (Sailfish on this phone) has it.

`device/kernel/patches/0006-sm6115-hook-up-mpm.patch` adds it as on the
sister SoCs QCM2290 and SM6375, with the pin map of Qualcomm's own
`qcom-mpm-bengal.c`: the SPMI arbiter (power key, RTC alarm, charger) and
the GPIOs go through the MPM, which is the parent power
domain of both CPU clusters. The same patch is meant for upstream
Linux (written against linux-next). It concerns every system on the mainline
kernel (Leste, the MeeGo copy, Kali, LuneOS). Built into `out-mpm/` (`OUT=`
of `30-build-kernel.sh`), so that `out/` and the boot image on the phone
stay as they are until it is tried: a wrong interrupt route costs the power
key or touch, at worst the boot (slot B, rollback image as before).

The MPM makes the system sleep states possible, it does not make the
phone use them while it is awake: with about 170 interrupts a second the
clusters rarely get down. They pay off together with autosuspend (s2idle,
where all CPUs stay down until a wake-up).

### On the phone (2026-10-28/29)

With 0006 the CPU clusters' power domains were below the MPM, which probes
late (it waits for its mailbox and SRAM): psci-cpuidle, which probes once,
gave up, and the phone ran without cpuidle at all (only WFI). Patch 0009
takes the clusters from below the MPM again. Patch 0011 hands the vMPM to
the RPM another way: from CPU PM notifications when the last online CPU
goes down (as rpmh-rsc does), and it writes the vMPM's TIMER0/TIMER1, the
earliest timer of the online CPUs as an arch timer count. Mainline never
wrote them, so the RPM had no timer to wake the SoC for; Qualcomm's series
"Register MPM under CPU cluster power domain" (July 2026, v1, for Agatti,
SM6375 and Shikra) does the same from a cluster domain, which the SM6115's
two clusters do not have in common. It is off by default
(`/sys/module/irq_qcom_mpm/parameters/notify_rpm`) and on only until the
next boot: a SoC that does not wake up resets through the watchdog into a
normal start.

## Changes after the first suspends (2026-10-26, untested)

`pro1x-autosuspend` now does this around every suspend (also with
`--test`), each a setting in `/etc/pro1x-autosuspend.conf`:

- `StopServices=pro1x-hexagonrpcd`: stopped before the suspend, started
  again after it (since 2026-10-30 only on kernels without patch 0031, see
  "Power fixes for pro1x.10"). With hexagonrpcd running, a suspend crashes the ADSP's
  sensor process on other Qualcomm phones too; postmarketOS and Debian
  stop it around a suspend
  ([pmaports MR 5400](https://gitlab.postmarketos.org/postmarketOS/pmaports/-/merge_requests/5400),
  [Debian's README](https://metadata.ftp-master.debian.org/changelogs/main/h/hexagonrpc/stable_README.Debian)).
  hexagonrpcd only serves the ADSP files (configuration, registry), which
  it asks for when the sensors start; libssc and iio-sensor-proxy talk to
  the ADSP over QRTR without it.
- `NoWakeup=4a8c000.serial`: the Bluetooth UART cannot wake the system
  (since 2026-10-30 left alone while Bluetooth's own wake-up switch is on;
  pro1x.10's patch 0030 does the rest in the kernel).
  Its wake-up interrupt fired at once. The price: Bluetooth cannot wake the
  phone (an incoming connection, a headset button), and what the chip sends
  during the suspend is lost; it reconnects after the next wake-up.
- `SuspendOnUSB=false`: no suspend while a USB cable is in, by the
  gadget's state or a USB or mains power supply online (a charger without
  a host), until the USB controller comes back from a suspend ("HS-PHY not
  in L2"). On battery, which is what standby time is about, this changes
  nothing.

## The panel's low power modes (2026-10-26, built, untested)

The Nokia N9 ran its stand-by screen with the AMOLED panel in a low power
mode (panel-nokia-dsi's `lpm`: DCS idle mode, 8 colours at a quarter of the
refresh, and partial mode, only the panel's partial area driven). The
Pro1X's panel (BOE BF060Y8M-AJ0) takes the same standard DCS commands:

- Linux: `device/kernel/patches/0007-panel-boe-bf060y8m-aj0-low-power-modes.patch`
  gives the panel driver `lpm` (0/1: idle mode) and `partial` ("first last"
  rows, 0: all) next to its backlight, `/sys/class/backlight/*/device/`,
  as the N9 had `/sys/class/backlight/display0/device/lpm`; both are kept
  across a power cycle of the panel and sent again on power-up.
- FreeBSD: `dev.qcom_dpu.0.lpm` and `dev.qcom_dpu.0.partial`
  (`freebsd/patches/0022`).
- `tools/pro1x-standby` (the N9's stand-by screen for an X session) uses
  them: idle mode, only the rows around the clock and icons driven (about
  a fifth of the panel), the brightness low, the panel off while the
  proximity sensor is covered. On FreeBSD it runs in the X session;
  on Leste mce blanks the display itself, so there it needs mce's side
  (not done yet).

Not possible on this panel: the N9's link power-down between updates
(ULPS), which needs a command-mode panel with its own frame memory; this
one runs in video mode, so the display controller keeps sending 60 frames
a second. A lower refresh in the stand-by screen would be the next step.

## Every driver checked against the N9 (2026-10-26, built, untested)

An audit of the Linux side with the Nokia N9's kernel, mce and dsme as the
model. The N9 never suspended: cpuidle took the SoC to OFF whenever
nothing held it (`/etc/pmconfig`: `enable_off_mode 1`, `sleep_while_idle
1`). The Pro1X's equivalent is cluster power-collapse -> MPM -> RPM vmin
from idle (patch 0006), which works only when nothing votes in the RPM
sleep set; so the votes come first.

Done (`device/kernel/patches/0008-power-checked-against-the-n9.patch`,
`device/kernel/leste.fragment`, `scripts/`):

- `pro1x-adc-battery`: every 30 s on the battery (`poll_battery_ms`), 5 s
  on a charger, on the freezable power efficient workqueue, the filters
  scaled by time; `VOLTAGE_NOW` from the last reading instead of a new
  burst of eight conversions per upower read. (The N9 read its gauge every
  360 s.)
- `pro1x-charger-icl`: every 20 s without a charger (2 s with one), power
  efficient workqueue.
- `aw86224`: the chip in standby between effects.
- Device tree: the touchscreen's bit-banged I2C at 2 us half-periods
  instead of 5 (each touch report is busy-waited); Bluetooth at 3 Mbaud
  instead of 3.2, whose 102.4 MHz UART clock needed the CX rail at its
  nominal corner.
- Kernel config: lazy RCU on offloaded CPUs, transparent huge pages only on
  request (no khugepaged scans), the LED pattern trigger.
- `fw_devlink.sync_state=timeout` on the command line: providers no longer
  keep the boot-time maximum (DDR, NoCs) for ever when a consumer never
  probes.
- No getty on `ttyMSM0` (the debug UART is disabled; init respawned it).

To try on the phone (with someone there):

- `dmesg | grep sync_state` before and after the new command line; the
  interconnect summary in debugfs.
- USB runtime PM: `rootfs-overlay/usr/share/pro1x/60-usb-runtime-pm.rules`
  into `/etc/udev/rules.d/` (dwc3 forbids it, so the controller, both PHYs
  and a GB/s DDR vote stay on without a cable, in the sleep set too; the
  N9's musb suspended on cable removal). Check that the USB network comes
  back after replugging.
- Bluetooth at 3 Mbaud: throughput and that the controller still comes up.
- The audio card suspends after 300 s of silence because of the
  loudspeaker pop (`50-pro1x.pa`); the N9 used 1-5 s. Patch 0026
  (7.1.0-pro1x.8) changes the aw87339 sequencing; its test and the switch
  to 3 s: `docs/audio.md`.
- MPM: GPIOs 106, 107 (keyboard) and 112 are not in the MPM map, so the
  keyboard cannot wake the SoC from vmin; the MPM's timer wake-up is not
  programmed by mainline.

Larger, not done: the touchscreen asleep while dt2w is off or the phone is
in a pocket (needs mce's proximity), the LED patterns in the LPG instead of
mce's timer, sensor batching in the ADSP requests (libssc asked for every
sample as a wake-up; done 2026-10-31, [sensors.md](sensors.md)), the
panel's ELVDD/ELVSS enable (gpio65) as a
regulator. (The Bluetooth UART clock off between transfers: patches 0023
and 0024, below.)

## The Bluetooth UART between in-band sleeps (2026-10-29, 7.1.0-pro1x.7, untested)

With Bluetooth up, the UART (`4a8c000.serial`) kept its clock, its CX
performance vote and its interconnect votes for good, in the RPM's sleep
set too, so the SoC could not reach vlow or vmin, awake or in a suspend.
Qualcomm's in-band sleep (HCI_IBS) has both sides tell when they sleep
(`HCI_IBS_SLEEP_IND`) and wake the other one before they send
(`HCI_IBS_WAKE_IND`, answered by `HCI_IBS_WAKE_ACK`); `hci_qca` tracked
that in a TX and an RX vote, but its clock hooks were empty.

- `0023-serdev-qcom-geni-uart-powered-down-while-open.patch`: serdev gets
  `serdev_device_pm_put()` and `serdev_device_pm_get()` (drop and take back
  the controller's runtime PM reference that `serdev_device_open()` took);
  a GENI UART owned by a serdev controller is held by that controller, not
  by a reference of its own, so it powers down (clocks, OPP/CX vote,
  interconnect, pins in their sleep state) once the client lets go and the
  serial core's port device has nothing left to send. While it is down its
  interrupt is off and its wake-up interrupt (GPIO 11, RX, an MPM pin)
  resumes it; it stays up 200 ms after a wake-up or the last received byte
  (`power/autosuspend_delay_ms`). For a suspend it is now forced off also
  while the port stays open.
- `0024-bluetooth-hci_qca-uart-off-while-both-sides-sleep.patch`: both
  votes off, the UART may power down; a vote on (the host waking the chip
  for TX, the chip's `WAKE_IND` for RX) brings it up first. The chip's
  first `WAKE_IND` byte is lost while the UART comes up; the chip repeats
  it until it gets the `WAKE_ACK`. Not while IBS is off (patch download,
  memory dump, power off, SSR), and after the download the RX vote starts
  on, until the chip's first `SLEEP_IND`. The FM receiver's commands go
  through the same wake-up (`qca_enqueue()`).

To check on the phone: `cat /sys/kernel/debug/bluetooth/hci0/ibs/*`
(`uart_idle` 1 while both sleep, `votes_off`/`votes_on` counting up,
`ibs_recv_wakes` and `ibs_sent_wacks` too),
`cat /sys/devices/platform/soc@0/4ac0000.geniqup/4a8c000.serial/power/runtime_status`
(`suspended` between the transfers), CX in `pm_genpd_summary` at 0 with
Bluetooth up, and that Bluetooth and the FM radio still work: a scan
(`hcitool lescan`, `bluetoothctl scan on`), a connection, an FM station
with RDS. If something breaks: `echo on > .../4a8c000.serial/power/control`
forbids the UART's runtime PM, which is the old behaviour (until the next
boot); `echo auto` allows it again. Lost wake-ups (the chip's events
arriving late or not at all, `Frame reassembly failed` in `dmesg`) would
mean the chip does not repeat its `WAKE_IND` fast enough: then a longer
`autosuspend_delay_ms` first.

## Wake-up audit (2026-10-29)

From the logs of 2026-10-25 (per interrupt, MeeGo, display off, before
patch 0008) and 2026-10-28 (`~/ps/pro1x-power/2026-10-28`, totals only,
Leste and MeeGo on 7.1.0-pro1x.1, display off, on the battery):

- **The first five minutes after a start cost about 100 mA:** Leste at
  -237 to -257 mA and about 400 interrupts a second until 290 s into the
  run, then -137 to -171 mA (mean -145 mA after the first 5 minutes,
  against -160 mA over the whole run) and 27-48 interrupts a second; MeeGo
  the same until 165 s (-160 mA after it). The step comes about 300 s
  after the system's start, which is PulseAudio's suspend-on-idle timeout
  (`timeout=300` in `rootfs-overlay/etc/pulse/default.pa.d/50-pro1x.pa`,
  set because every resume of the card pops in the loudspeaker): the sound
  card and the ADSP's audio path running silence. Not proven yet; the
  logger's new `pcm` column (ALSA substreams running) shows it in the next
  runs. If so, every sound costs 5 minutes at +100 mA, and it keeps
  `pro1x-autosuspend` (which waits for running substreams) from suspending
  meanwhile: then the pop has to be fixed (aw87339 sequencing) so that the
  timeout can go down to the N9's 1-5 s (patch 0026, `docs/audio.md`).
- **Steady state, 27-48 interrupts a second on Leste (41-64 on MeeGo).**
  On 2026-10-25 (MeeGo, about 170 a second then) they were mostly timers
  and IPIs: function-call IPIs 57/s (remote task wake-ups go through
  them), the arch timer 44/s, the broadcast timer (`arch_mem_timer`) 34/s
  and its IPIs 14/s for CPUs in their deep idle state, rescheduling IPIs
  12/s; then the battery ADC 3.8/s (on USB), UFS 3.2/s, the RPM's glink
  1.8/s, the Bluetooth UART 1.7/s, Wi-Fi 0.7/s. By process: `rcu_preempt`
  13/s (lazy RCU since 0008), kworkers around 25/s, `upowerd` 3.5/s (fixed
  by the `NoPollBatteries` override), UFS clock gating and scaling 4/s,
  `jbd2` 1.4/s. The 2026-10-28 logs have no breakdown.
- **Changed:** `0025-power-pro1x-adc-battery-charge-pace-only-while-charging.patch`:
  with the cable in but nothing charging (the charger's input suspended,
  which is how every standby measurement runs, or a full battery) the
  battery was read at the charging pace, every 5 s, a burst of ADC
  conversions each time; now every 30 s as on the battery. The Bluetooth
  UART (0023/0024, above). Kernel option: the trace events
  (`device/kernel/leste.fragment`, tracepoints only).
- **Tools:** `tools/pro1x-wakeups [SECONDS]` counts, from the trace
  events, the interrupts, each CPU's idle exits, the timer and hrtimer
  callbacks (and the tasks their timers wake), the work items, the task
  wake-ups, the IPIs and who sent the function-call ones, plus the
  context switches per process; the trace buffer is read only at the end.
  `pro1x-powerlog` now ends with the interrupts per second over the run
  by source, the mean without the first 5 minutes, the `pcm` column, and
  runs `pro1x-wakeups` for a minute halfway through when it is installed
  (`68-power-standby.sh` installs both and fetches `<name>.wakeups`).
- **Next, from pro1x-wakeups on the phone:** whatever leads its lists.
  Candidates seen in the Leste image: `cron` (once a minute), `rsyslog`
  and `jbd2` (writes every few seconds keep UFS waking: a longer
  `vm.dirty_writeback_centisecs`, `commit=` for the root), `dsme-thermal`,
  `icd2` (Wi-Fi scans while disconnected), `haveged`.

## Power fixes for pro1x.10 (2026-10-30, built, untested)

The three failures of the first suspends, Bluetooth while it is off, and
the sound card's idle time, fixed on the PC; everything has a switch
back. Kernel patches 0029-0032 (0028 is the alarm power-on's) go into
7.1.0-pro1x.10, which is built from all of 0001-0032 together. A test
build of 0001-0027 plus 0029-0032 alone: `$WORK/out-pro1x.10-power`
(7.1.0-pro1x.10-power, from the worktree `$WORK/build/linux-power`, so
that `build/linux` stays as it is for the combined build). User space:
`scripts/71-power-userspace.sh` puts it into the systems on the phone,
new images get it from their builders.

| What | Fix | Switch back |
|---|---|---|
| ADSP `sensor_process` crash at the thaw | 0031: FastRPC calls frozen in place | `echo N > /sys/module/fastrpc/parameters/freezable_invoke` |
| USB gadget "not attached" after a suspend | 0032: VBUS override written again on resume | boot the old kernel (`docs/kernel-update.md`) |
| Bluetooth UART waking the system | 0030: its wake-up interrupt only while open, and for a suspend only when Bluetooth may wake | Bluetooth may wake it: `echo enabled > /sys/bus/serial/devices/serial0/power/wakeup`; lost Bluetooth events: `echo on > .../4a8c000.serial/power/control` (0023's switch) |
| Bluetooth chip powered while "off" | 0029: power-off pulse and regulators off at hci down | boot the old kernel |
| Sound card open 300 s after a sound | 3 s with a kernel that has 0026 (PulseAudio), 3 s on PipeWire | `touch /etc/pulse/pro1x-suspend-300`; WirePlumber: the file below |
| Wi-Fi power save | on at every start of the interface, NetworkManager keeps it | `touch /etc/pro1x-wlan-powersave-off` |
| hexagonrpcd stopped for every suspend | only on kernels without 0031 | `StopServices=pro1x-hexagonrpcd` in `/etc/pro1x-autosuspend.conf` |
| Kali and Arch: the modem's GNSS engine from boot on, a fix a second for ever (2026-10-31 audit, below) | `pro1x-gnss.service` only while gpsd runs, gpsd started by `gpsd.socket` for its first client | `systemctl enable gpsd.service` (the engine from boot on again) |
| Leste and the MeeGo copy: gpsd in a runlevel would do the same | gpsd in no runlevel (location-daemon starts it) | `rc-update add gpsd default` |

### The sensor process crash (0031)

The kernel log of 2026-10-25 has the crash 2 ms after the tasks were
thawed, in the ADSP's FastRPC thread:

    [ 336.683309] Restarting tasks: Starting
    [ 336.685206] PDM: service 'sensor_process' crash: 'EX:sensor_process:0x1:frpc_dsp:0xc0:PC=0xb2022ad0'
    [ 336.702919] qcom_q6v5_pas a400000.remoteproc: fatal error received: err_qdi.c:1045:EX:sensor_process:...
    [ 359.391851] qcom-q6afe aprsvc:service:4:4: AFE close failed -110

hexagonrpcd's reverse-RPC listener waits nearly all the time in a
`FASTRPC_IOCTL_INVOKE` for the DSP's next request. Mainline's fastrpc
waits for the answer interruptibly; the freezer's fake signal ends that
wait with -ERESTARTSYS, the context stays queued with the DSP, the
user's maps are moved away, and after the thaw the restarted ioctl sends
the same call as a new context. The DSP gets its listener call twice and
its sensor process dies in `frpc_dsp`; remoteproc restarts the ADSP and
the APR audio services are gone until a reboot (Android's adsprpc
restores the interrupted context instead). Stopping hexagonrpcd for each
suspend (postmarketOS, Debian, pro1x-autosuspend until now) avoids the
call, at the price of the sensors' reverse tunnel going down and up
every time.

`device/kernel/patches/0031-misc-fastrpc-freeze-user-invocations-in-place.patch`
waits `TASK_KILLABLE | TASK_FREEZABLE` instead (as qcomtee and FUSE do):
the freezer freezes hexagonrpcd where it waits, nothing is sent twice,
an answer that arrives meanwhile is taken at the thaw. SIGTERM (no
handler in hexagonrpcd) still ends it; `rc-service pro1x-hexagonrpcd
stop` works as before. pro1x-autosuspend stops hexagonrpcd now only when
`/sys/module/fastrpc/parameters/freezable_invoke` is not `Y` (a kernel
without 0031, or the switch set back), unless `StopServices` says
otherwise.

Not changed here: the sensors keep streaming into a suspended system (the
ADSP's glink edge is no wake-up source, so their indications wait).
Batched, non-waking requests and the ADSP told of suspends (kernel
0038/0039) came on 2026-10-31: [sensors.md](sensors.md).

### The USB gadget after a suspend (0032)

On SM6115 the USB controller's power domain (the USB30 GDSC,
`PWRSTS_OFF_ON` in gcc-sm6115) is switched off by genpd for s2idle (and
by runtime PM once the glue's `power/control` is `auto`). The legacy glue
(`dwc3-qcom-legacy`, which the SM6115's two-node device tree binds) wrote
the VBUS override into its QSCRATCH registers once at probe; they come
back at their reset value, so the gadget never sees VBUS again and stays
"not attached", and switching the role does not help because the role
switch does not go through this glue. The flattened glue (`dwc3-qcom.c`)
writes the override again on every role change and run/stop for the same
reason, downstream after every power collapse.
`0032-usb-dwc3-qcom-legacy-restore-qscratch-on-resume.patch` writes it
(and the UTMI clock selection, which the Pro1X does not use) again in the
glue's resume. "HS-PHY not in L2" was not the cause: a peripheral's PHYs
are powered off by the core (`dwc3_core_exit()`), not suspended, so the
message is a debug message for a peripheral now. The other way would be
`PWRSTS_RET_ON` for the GDSC (as sm8250, sc7180, sm6350 and sm6375 have),
which keeps the domain on through every suspend.

With it, `SuspendOnUSB=true` and the USB runtime PM rule
(`rootfs-overlay/usr/share/pro1x/60-usb-runtime-pm.rules`) can be tried;
both stay off until the test below.

### Bluetooth (0029, 0030)

- **0029:** WCN3950 support (upstream, 2025) added the chip to every
  WCN399x case of the power-on path but to none of `qca_power_off()`:
  `hciconfig hci0 down` (or Bluetooth off in the UI) sent no power-off
  pulse and left its regulators enabled, so its Bluetooth part stayed
  powered behind a closed UART. Now powered off like the WCN3990 (pulse,
  regulators; they are shared with Wi-Fi, whose own enable stays). The
  FM radio brings hci0 up itself (`qca_fm_get()`), so it is not
  affected.
- **0030:** the GENI UART requested its wake-up interrupt (RX, GPIO 11) at
  probe and armed it for every suspend, also with the port closed. The
  Bluetooth core does not want to be woken there: `hci_qca` asks the
  serdev controller's `power/wakeup` (off by default) and without it
  disconnects everything for the suspend. Now the interrupt is there only
  while the port is open (Bluetooth up), and for a suspend only when the
  controller may wake the system, so the UART and the Bluetooth core
  agree. In-band sleep's runtime wake-ups (0023/0024) keep it while the
  port is open. Bluetooth waking the phone (paired devices allowed to,
  e.g. a keyboard) is one switch:
  `echo enabled > /sys/bus/serial/devices/serial0/power/wakeup`.
  pro1x-autosuspend's `NoWakeup=4a8c000.serial` stays (for older kernels)
  but leaves the UART alone while that switch is on. If Bluetooth loses
  events while awake (`Frame reassembly failed`, a connection that does
  not answer), the way back is 0023's: `echo on >
  /sys/devices/platform/soc@0/4ac0000.geniqup/4a8c000.serial/power/control`
  keeps the UART up, its wake-up interrupt is then not needed.

### The sound card's idle time

`rootfs-overlay/etc/pulse/default.pa.d/50-pro1x.pa` (Leste and the MeeGo
copy, whose session uses Leste's PulseAudio) loads module-suspend-on-idle
with `timeout=3` when `/sys/module/snd_soc_aw87339/parameters/on_delay_ms`
exists (kernels with the loudspeaker fix, 0026: pro1x.8 and later),
otherwise `timeout=300` as before; PulseAudio's `.ifexists` takes absolute
paths. Still a pop: `touch /etc/pulse/pro1x-suspend-300`, PulseAudio's
next start is back at 300 s. Kali and Arch run PipeWire, whose
WirePlumber suspends after 5 s by default and cannot test the kernel:
`kali/rootfs-overlay/etc/wireplumber/wireplumber.conf.d/50-pro1x-suspend.conf`
(Arch installs Kali's) sets 3 s; 300 there (or the file removed for 5 s)
is the way back.

### Wi-Fi power save

cfg80211 starts interfaces with power save on (`CONFIG_CFG80211_DEFAULT_PS`,
ath10k supports it unless its firmware says `NO_PS`), but nothing checked
that it stays on. `85-pro1x-wlan-powersave.rules` runs
`/usr/local/sbin/pro1x-wlan-powersave` (`iw dev wlan0 set power_save on`)
when the interface appears, on all four systems; on Kali and Arch
NetworkManager keeps it on for every connection
(`50-pro1x-wifi-powersave.conf`, `wifi.powersave = 3`). Off for good:
`touch /etc/pro1x-wlan-powersave-off`.

### Checked, nothing to change (from the code, 2026-10-30)

- **CPU idle:** PSCI OS-initiated mode with per-CPU power-collapse states
  and two cluster states each (GDHS, power collapse) in `sm6115.dtsi`,
  `psci-cpuidle-domain` and the menu governor in the kernel; the clusters
  are not below the MPM (0009), so cpuidle probes. Whether they get there:
  `Rejected`/`Above` in `/sys/kernel/debug/pm_genpd/power-domain-cpu-cluster*/idle_states`.
- **GPU:** devfreq (simple_ondemand) over the Adreno 610's OPP table, on
  CX; the GPU node has no interconnect, so it holds no DDR vote.
- **Panel off:** `boe-bf060y8m-aj0` sends display off and sleep in, holds
  reset and switches its five supplies off.
- **Interconnect votes:** the QUP UART's are dropped with its runtime PM
  (0023), the USB glue's at its suspend; without a cable the USB
  controller keeps them while awake until the runtime PM rule is on.
- **Own user space:** `pro1x-steps` (the ADSP counts, signals at most
  every 5 s), `pro1x-proximity` (streams only while watched),
  `pro1x-light` (one reading), `pro1x-volume-osd` (timers only while
  shown) are event-driven; the init scripts' loops run only at start.
  Left for the wake-up measurement: cron, rsyslog and jbd2 (a journal
  commit every 5 s while something logs: `commit=` for the root,
  `vm.dirty_writeback_centisecs`), dsme-thermal, icd2's scans, haveged.

## Audit of the Linux systems (2026-10-31, from the PC)

Everything the repository builds for the five Linux systems on 7.1.0-pro1x.10,
looked at again for energy: the kernel's configuration
(`$WORK/out-pro1x.10/config-7.1.0-pro1x.10`), the device tree, the init
scripts, units, udev rules and settings of `rootfs-overlay/`, `kali/`,
`arch/`, `luneos/` and `meego/`, and the defaults of the packages they
start from (Leste's base image of 2026-04-26, leste-config-common 1.146,
Devuan's and Debian's gpsd, Arch Linux ARM's gpsd, kali-themes,
xfce4-power-manager). Classes: **A** changed now (user space, with a way
back, in `71-power-userspace.sh` and the builders), **B** a measurement or
decision for today (below, "Measurements for 2026-10-31", steps 4-10),
**C** not worth a change. Ranked by the expected effect. The owner
decided the B items the same day (Bluetooth off, no "device on" light,
dt2w in the controller's gesture mode, display off after 2 minutes, the
file systems' options): "The owner's decisions" below.

**Kali and Arch**

1. **A: the GNSS engine ran from boot on.** `kali/20-pro1x-hardware.sh` and
   `arch/20-pro1x-hardware.sh` enabled `pro1x-gnss.service` (WantedBy
   multi-user) and `gpsd.service`, `/etc/default/gpsd` had `-n`, and
   pro1x-gnss starts a periodic LOC session (one fix a second) as soon as
   it runs (`tools/pro1x-gnss/pro1x-gnss.c`, `start_session()`): the
   modem's GNSS receiver on and a position indication waking the phone
   every second, all the time, for no client at all (Xfce and Phosh have
   none by default). Now `pro1x-gnss.service` is `WantedBy=gpsd.service`
   and `BindsTo=gpsd.service`, gpsd is started by `gpsd.socket` for its
   first client (cgps, gpspipe, kismet...), `-n` is gone. Debian's gpsd
   enables `gpsd.service` itself, so the builders disable it (which takes
   the socket along through `Also=`) and enable the socket again. After
   the first use gpsd and the engine stay up until `systemctl stop
   gpsd.service` or the next boot: off after boot, not off after use (the
   next step would be pro1x-gnss stopping its session while gpsd has no
   client). The images on the phone get it from `71-power-userspace.sh`
   (the links of what `systemctl enable` and `disable` do, each saved).
2. **B: the display's blanking.** Kali's own Xfce settings
   (kali-themes' `xfce4-power-manager.xml`) set only the power button, so
   xfce4-power-manager's defaults apply: dimmed to 20 % after 300 s on
   the battery, DPMS sleep after 5 and off after 10 minutes. On a phone
   that is minutes of the lit panel after each use. Phosh
   (Arch) blanks after GNOME's `idle-delay` (300 s unless Arch's phosh
   overrides it). A decision for the owner (step 10).
3. **C:** journald (synced every 5 min), `RuntimeWatchdogSec=30s` (a
   watchdog ping every 15 s), snapper without timeline snapshots, Btrfs
   with `noatime` (Arch), NetworkManager and ModemManager defaults,
   WirePlumber's 3 s and NetworkManager's Wi-Fi power save (pro1x.10 fixes).

**Leste and the MeeGo copy**

1. **A (insurance): gpsd in no runlevel.** `46-sensor-stack.sh` installs
   Devuan's gpsd, whose postinst runs `update-rc.d gpsd start 26 2 3 4 5`;
   with leste-config's OpenRC script in its place (no LSB header) that most
   likely added nothing, but if gpsd were in `default`, `/etc/conf.d/gpsd`
   would start pro1x-gnss at boot (the script's `checkconfig` passes while
   the location gconf keys are unset). 46 and 71 now take it out of
   `default`; location-daemon starts gpsd on demand, as leste-config's
   policy-rc.d says ("gpsd is started on-demand by location-daemon").
   Check on the phone: step 4.
2. **B: the touchscreen with the display off.** `dt2w=1`
   (`rootfs-overlay/etc/modprobe.d/pro1x-dt2w.conf`) keeps the Goodix
   controller scanning and its bit-banged I2C reads running for every
   touch in a pocket. What it costs: step 5.
3. **B: the "device on" light.** Leste's mce has
   `PatternDeviceOn=254;0;0;255;255;255;1000;5000` (`/etc/mce/mce.ini`,
   led-sw: white, all three channels at 255, 1 s on and 5 s off, from
   mce's timers) and osso-applet-display's gconf default
   `/system/osso/dsm/leds/PatternDeviceOn` is true: with the display off
   the LED blinks and mce wakes twice in 6 s. Worse once pro1x-autosuspend
   runs: a suspend that falls into the second the LED is on keeps it lit
   for the whole sleep (mce's timers are frozen, and leds-qcom-lpg has no
   suspend handling and no `LED_CORE_SUSPENDRESUME`). Step 6.
4. **B: Bluetooth up from boot** (`45-pro1x-hardware.sh` adds `bluetooth`
   to `default`; BlueZ's `AutoEnable` powers hci0). With pro1x.10 the
   UART sleeps between in-band sleeps (0023/0024) and the chip is off at
   `hci down` (0029); what "up but idle" still costs is in step 7.
5. **B: the root's journal.** The ext4 root on the loop device is mounted
   with the defaults (`commit=5`, `relatime`: `pro1x-loop-root` sets no
   `ROOTFLAGS` for ext4 and Leste's fstab has no root line), on `/host`
   (`rw,noatime`), which journals the image file's writes once more. Step 8.
6. **C, deliberate:** the stand-by screen for 60 s at 20 % after each
   blank (`50-pro1x.ini`, the N9's low power mode), PulseAudio's 300/3 s,
   USB runtime PM off until 0032 is proven, UFS clock scaling off
   (`60-pro1x-ufs.rules`: the devfreq hang of 2026-10-28; idle UFS is
   clock gated and ufs-qcom drops its interconnect votes to the minimum
   at the gating, so it costs only while there is I/O), Leste's display
   timeouts (dim after 30 s, blank 3 s later).
7. **C:** haveged (in Leste's `default`; once the kernel's CRNG is ready
   its `select()` for writing never returns), cron (a wake-up a minute),
   rsync, dundee, cellulard, dnsmasq (idle or not started), dsme-thermal
   (a one-shot at start), bmed's 60 s check (`meego/bme/bmeipc.c`),
   pro1x-events' feed every 15 and weather every 30 minutes while the
   look is on, also with the display off (`tools/pro1x-events`; a fetch
   or two an hour), the own daemons (event-driven, see above).

**LuneOS**

- **C:** no Wi-Fi power-save rule and not in 71: cfg80211 starts the
  interface with power save on anyway (`CONFIG_CFG80211_DEFAULT_PS=y`);
  whether connman leaves it on is one `iw dev wlan0 get power_save` the
  next time LuneOS runs. No GNSS or sensor daemons.

**Kernel and device tree (all systems)**

- **C, right as it is:** `NO_HZ_IDLE`, `HZ=250`, schedutil by default and
  nothing in user space that changes a governor or a frequency limit,
  `WQ_POWER_EFFICIENT_DEFAULT`, lazy RCU on offloaded CPUs, no lockup or
  hung task detectors, no workqueue watchdog, no KFENCE, no
  `DEBUG_PREEMPT` or lockdep; `PM_DEBUG` and the trace events cost
  nothing while unused; NUMA balancing is built but inactive with one
  node. In the device tree only `vph_pwr` is always-on; the RPM's
  regulators are not, the wake-up sources are the power key, the RTC,
  the keyboard slide, volume up and the keyboard matrix (GPIO 107,
  which is not in the MPM map: it wakes from s2idle, not from vmin).
- **B: the cpuidle governor.** menu only (`CONFIG_CPU_IDLE_GOV_TEO` is not
  set). With the clusters' deeper state rejected nearly always, TEO is
  the candidate for the next kernel: both built, switched at run time in
  `/sys/devices/system/cpu/cpuidle/current_governor` and compared, not
  assumed better on this big.LITTLE. Today: the baseline (step 9).

## The owner's decisions (2026-10-31, built, untested)

The owner decided the audit's B items the same day; all of it is the
default in the image builders and goes into the systems on the phone with
`scripts/71-power-userspace.sh install` (`restore` puts the old back), the
initramfs part with the next `65-kernel-update.sh prepare` (it installs
the repository's `initramfs/` into Leste before it makes the initramfs,
lines 90-95), the kernel part with 7.1.0-pro1x.11.

| What | Now | Way back |
|---|---|---|
| GNSS after use | pro1x-gnss stops the LOC session (and the engine) 30 s after gpsd stopped reading (gpsd closes its UDP source a minute after its last client; pro1x-gnss sends on a connected socket and sees ECONNREFUSED), and starts it again within 10 s when gpsd reads again (a TXT probe every 5 s). Kali/Arch: `-x 60` exits 60 s later and the unit then stops gpsd (gpsd 3.25+ wakes every 2 s even idle); its socket starts both for the next client. Leste: location-daemon stops the gpsd it started once its clients are gone (`patches/fixes/location-daemon.patch`, built by 49-hidpi.sh) | `pro1x-gnss -g 0` (never stop); the old location-daemon package |
| Bluetooth | off until switched on: `AutoEnable=false` in `/etc/bluetooth/main.conf` (Leste, MeeGo copy, Kali, Arch; LuneOS's own Bluetooth service decides there). Switched on with `bluetoothctl power on`, Phosh's quick setting or blueman; the FM radio brings hci0 up by itself (0014) | `AutoEnable=true` |
| Status LED | the N900's notification light: no "device on" pattern; missed call, SMS, e-mail, chat and other notifications a short blue flash every 3 s while unread (hildon-home's notification groups activate and deactivate them; Harmattan's `PatternCommunication` too); battery low a red flash with the display off (`patches/fixes/mce.patch`, mce +pro1x4); charging and full as Leste's (amber, green). Calendar and alarm reminders light nothing (hildon-home's `alarm-event` has no LED pattern, as in Maemo 5). pro1x-autosuspend switches the LED off for each suspend and back after it (`LedsOff`). Arch: Phosh's feedbackd drives LEDs only through its own device files (not set up for the Pro1X); Kali's Xfce has nothing that drives a notification LED | `LEDPatterns` out of `mce.ini.d/50-pro1x.ini`; `LedsOff=` (empty) in `/etc/pro1x-autosuspend.conf` |
| Double tap to wake | kept; with 0033 (pro1x.11) the GT9286 waits in its own gesture mode while the display is off and interrupts only for a gesture, instead of scanning at full rate with a bit-banged I2C read for every touch. A controller without gesture firmware falls back by itself to 0002's detection | `echo N > /sys/module/goodix_ts/parameters/dt2w_gesture` (from the next blank on) |
| Display off | after 2 minutes: Leste and the MeeGo copy the backlight time-out 120 s as a gconf default in `/etc/gconf/gconf.xml.defaults` (a user's own value wins; the stand-by screen follows for `LowPowerModeBlankTimeout`), Kali xfce4-power-manager's DPMS sleep and off after 2 min (`/etc/xdg/.../xfce4-power-manager.xml`), Arch Phosh's `idle-delay` 120 (`90-pro1x.gschema.override`). LuneOS keeps it in its own settings service (its Settings app) | 71 `restore`; Settings |
| Root file systems | ext4 roots with `lazytime,commit=30` (journal commits every 30 s instead of 5, timestamps written lazily): the initramfs's `ROOTFLAGS` and each system's fstab line for `/` (Leste and the MeeGo copy get one: `checkroot.sh` remounts `/` with the fstab's options, "defaults" without a line, which drops lazytime; Kali's and LuneOS's lines get the options, systemd remounts `/` with them). Arch's Btrfs: `lazytime` (it commits every 30 s by itself) | `touch /host/pro1x-fs-defaults` (the initramfs's part, next start); 71 `restore` (fstab) |
| /host (userdata) | `rw,noatime,lazytime,commit=30` from the initramfs (both mounts in `pro1x-loop-root`; no system's fstab has `/host`, the initramfs moves it in) | `touch /host/pro1x-fs-defaults` |
| Loop images | direct I/O (`losetup --direct-io=on`, util-linux's losetup in the initramfs): no second copy of the image's pages in the backing file's page cache, no second write-back. The loop device then takes the UFS disk's 4 KiB sectors, so only an image whose file system has 4 KiB blocks gets it (blkid's `BLOCK_SIZE`), else the plain loop device | `touch /host/pro1x-fs-defaults` |
| Write-back | `vm.dirty_writeback_centisecs=1500`, `vm.dirty_expire_centisecs=3000` (`/etc/sysctl.d/60-pro1x-writeback.conf`, every system): the flusher every 15 s while there are dirty pages, a page expired after 30 s as the journals. Up to about 45 s of writes are lost by a power-key reset | the file removed |
| cpuidle | TEO built next to menu (pro1x.11), menu stays the default | (none needed) |

Logs stay on the flash until today's tests are done (no logs in RAM).

### Wi-Fi offloads (ath10k on the WCN3950, checked in 7.1)

- **Beacons and beacon loss:** the firmware handles them. ath10k sets
  `SUPPORTS_PS`, `SUPPORTS_DYNAMIC_PS` and `CONNECTION_MONITOR` (`mac.c`),
  the firmware reports a beacon miss itself (`ath10k_mac_handle_beacon_miss`),
  and beacons do not reach the host in power save: Wi-Fi was 0.7 interrupts
  a second on 2026-10-25, against 10 beacons a second.
- **ARP and NS offload:** not implemented in mainline ath10k (wmi-tlv has
  the command id and TLV tags, nothing sends it); what it could save is
  part of those 0.7 interrupts a second. No patch: not clearly worth it.
- **WoWLAN:** wmi-tlv implements it, but `ath10k_snoc` never makes the
  device wakeup-capable (`device_may_wakeup()` fails, the suspend then
  disconnects; hence pro1x-autosuspend's `StayAwake=45` for the reconnect).
  It matters only once pro1x-autosuspend runs. Step 11 below shows whether
  the firmware offers it at all (`iw phy phy0 info`); only then a patch
  (0036: wakeup-capable SNOC device, the WoW wake interrupt as a wake-up
  source), measured with a suspend that keeps the association.
- **From user space:** power save (on, 85-pro1x-wlan-powersave), and
  NetworkManager's `wifi.powersave` (3) on Kali and Arch; nothing else.

### dt2w in gesture mode: the device test (pro1x.11, not flashed)

`$WORK/out-pro1x.11` is 7.1.0-pro1x.10 plus 0033 and TEO. After it is on
the phone (`65-kernel-update.sh prepare/flash`, a day after pro1x.10 was
confirmed), display off:

1. `dmesg | grep -i goodix`: no "no gesture mode" line yet.
2. Double tap the dark screen: the display comes on (KEY_POWER on
   "Goodix double-tap wake"). `cat /sys/module/goodix_ts/parameters/dt2w_gesture`
   Y. Several taps that are not a double tap: nothing.
3. If "no gesture mode: double tap to wake from the touch reports" is in
   `dmesg` after the first touch: the firmware has none; dt2w still works
   (0002), as before.
4. Touches while dark: `grep goodix /proc/interrupts` before and after a
   minute of single touches and swipes on the dark screen: a gesture mode
   that works counts (nearly) nothing.
5. The cost: `pro1x-powerlog dt2w-gesture 15`, then
   `echo N > /sys/module/goodix_ts/parameters/dt2w_gesture`, display on and
   off again, `pro1x-powerlog dt2w-reports 15`; compare with step 5 below.
6. A suspend and resume (`69-rpm-test.sh 60 rpm bt-down`): the double tap
   works afterwards (the resume arms the gesture mode again).

## Measurements for 2026-10-31

What to compare: pro1x.9 with the old user space (the state of the
flash) against pro1x.10 with `scripts/71-power-userspace.sh install`.
Over Wi-Fi with the cable out for anything that suspends (until 0032 is
proven), the phone lying still, display off.

1. **Awake, display off** (no suspend), on the battery by the charger's
   input suspend: `pro1x-powerlog <name> 30` (or `68-power-standby.sh`);
   its last lines give the mean current without the first 5 minutes, the
   interrupts per second by source and, halfway, `pro1x-wakeups` for a
   minute. Once on each kernel; on pro1x.10 also after one sound (the
   `pcm` column: the card closed about 3 s after it).
2. **Suspended:** `69-rpm-test.sh 600 rpm bt-down` on each kernel, then
   on pro1x.10 the new parts one at a time:
   - `69-rpm-test.sh 60 rpm bt-down sensors`: hexagonrpcd running through
     the suspend; the log's `remoteproc crashes 0`, afterwards
     `rc-service pro1x-hexagonrpcd status` started, the step counter
     counting (`GetSteps` before and after a few steps) and a sound
     playing (q6afe answers).
   - `69-rpm-test.sh 60 rpm bt-up btwake`: Bluetooth up, the UART's
     wake-up as the kernel sets it (none with serial0's wakeup disabled);
     not woken at once. Then `hciconfig hci0 down` and `grep 4a8c000
     /proc/interrupts`: no `4a8c000.serial:wakeup` line while Bluetooth
     is down (0030), and Bluetooth up again works (scan, FM radio).
   - Over USB (cable in, gadget configured): `pro1x-rpmtest 60 norpm`
     run on the phone from a Wi-Fi shell; afterwards
     `cat /sys/class/udc/*/state` is `configured` again and the USB
     network answers (0032). If so, `SuspendOnUSB=true` and the runtime PM
     rule next.
3. **Standby per hour:** `rc-service pro1x-autosuspend start` (pro1x.10
   only, after 2 passed), unplugged, display off, for at least an hour:
   battery `capacity` and `voltage_now` before and after,
   `grep pro1x-autosuspend /var/log/syslog` (sleeps, wake-ups, vlow/vmin
   per sleep), `/sys/kernel/debug/qcom_stats/{vlow,vmin}` Count and
   Accumulated Duration before and after,
   `sort -k4 -n -r /sys/kernel/debug/wakeup_sources | head -15` (wakeup
   counts) and `/sys/power/suspend_stats/*`.

The audit's steps (2026-10-31, "Audit of the Linux systems" above), on
pro1x.10 after block 6 of `docs/device-day-2026-10-31.md`. The pairs in
5-7 run on the same conditions as step 1 (charger input suspended, display
off, lying still), 15 minutes each, and compare the means without the
first 5 minutes; every switch in them is a run-time one and goes back by
itself at the next boot.

4. **GNSS only on demand, and off after use.** Leste, a few minutes after
   the boot and before any location application: `rc-update show default | grep gpsd` and
   `pidof gpsd pro1x-gnss` both empty (if gpsd was in `default` before 71,
   note it: then the engine ran in every earlier Leste measurement). Kali
   (block 10) and Arch (block 11): `systemctl is-enabled gpsd.socket
   gpsd.service pro1x-gnss.service` gives enabled, disabled, enabled (the
   last through `gpsd.service.wants`), `pgrep -x pro1x-gnss` is empty;
   `gpspipe -w -n 5` starts both (`systemctl is-active gpsd pro1x-gnss`),
   `systemctl stop gpsd.service` stops both; without it, about 2.5 minutes
   after the last client both are gone by themselves (`journalctl -u
   pro1x-gnss`: "session 1 stopped", "exit"). Leste with the new
   location-daemon: an application's positions, then closed, and about
   15 s later `pidof gpsd pro1x-gnss` empty. If Kali or Arch has to be
   measured anyway: one `pro1x-powerlog` there before and after `systemctl
   stop gpsd.service` on the old setting shows what the engine cost.
5. **dt2w** (decided: it stays; pro1x.11's gesture mode, above):
   `pro1x-powerlog dt2w-on 15`, then `echo 0 >
   /sys/module/goodix_ts/parameters/dt2w` and `pro1x-powerlog dt2w-off 15`,
   on pro1x.10: what the touch reports' dt2w costs, the baseline for the
   gesture mode's test.
6. **The "device on" light** (decided: gone, notifications only): on the
   old user space first, with the display off, look whether the LED
   blinks white every 6 s, and `cat /sys/class/leds/*:status/brightness`;
   after 71 and mce's restart (a reboot) it stays dark, a missed call or
   an SMS makes it flash blue until read.
   Then `pro1x-powerlog led-on 15`; `dbus-send --system --type=method_call
   --dest=com.nokia.mce /com/nokia/mce/request
   com.nokia.mce.request.req_led_pattern_deactivate string:PatternDeviceOn`;
   `pro1x-powerlog led-off 15`, looking that the LED stays dark through
   it (mce may switch the pattern on again at the next blank; the
   persistent switch is the gconf key
   `/system/osso/dsm/leds/PatternDeviceOn`, Settings, notification light).
   Before one suspend of step 2 (or with `69-rpm-test.sh` itself), the
   three `brightness` values: one not 0 means the LED stays lit through the
   suspend. Decision: the pattern into the LPG's hardware pattern trigger
   (`CONFIG_LEDS_TRIGGER_PATTERN` is built), lower and shorter (e.g.
   `24;24;24` for 100 ms), or off by default.
7. **Bluetooth up but idle** (decided: off by default): after 71 and a
   reboot `hciconfig hci0` shows DOWN; `pro1x-powerlog bt-down 15`,
   `bluetoothctl power on`, `pro1x-powerlog bt-up 15`: what an unused but
   switched-on Bluetooth costs. The FM radio plays with Bluetooth off.
8. **Storage** (decided: `lazytime,commit=30` and the rest, above). After
   the boot with the new initramfs: `findmnt -n -o OPTIONS / /host` (both
   with `lazytime` and `commit=30`), `losetup -l -O NAME,DIO,LOG-SEC`
   (loop0 DIO 1, 4096), `sysctl vm.dirty_writeback_centisecs` (1500). In
   step 1's `.wakeups`, the lines of `jbd2/*` and the writeback kworkers,
   against the old values (the run on the old boot image, or one with
   `/host/pro1x-fs-defaults`).
   Also once: `cat /sys/bus/platform/devices/4804000.ufshc/{clkgate_enable,clkgate_delay_ms,auto_hibern8}`
   (expected 1, a delay in ms and a non-zero hibern8 timer: idle UFS
   gated).
9. **cpuidle baseline for TEO:** `cat
   /sys/devices/system/cpu/cpuidle/current_governor` (menu), then before and
   after a 10 minute display-off period
   `grep . /sys/devices/system/cpu/cpu[04]/cpuidle/state*/{name,usage,above,below}`
   and `cat /sys/kernel/debug/pm_genpd/power-domain-cpu-cluster*/idle_states`.
   A deeper state with `above` close to `usage` (it was entered and the
   next wake-up came sooner than it pays) is the case for building TEO
   next to menu in the next kernel and comparing step 1 with
   `echo teo > /sys/devices/system/cpu/cpuidle/current_governor`.
10. **Blanking** (decided: 2 minutes everywhere): Leste `gconftool-2 -g
    /system/osso/dsm/display/display_dim_timeout` as `user` (120 unless
    set otherwise), Kali `xfconf-query -c xfce4-power-manager -lv` (as
    `kali`), Arch `gsettings get org.gnome.desktop.session idle-delay` (as
    `alarm`, 120), and the time from the last touch until the panel goes
    off.
11. **Wi-Fi:** `iw phy phy0 info | sed -n '/WoWLAN/,/^\t[A-Z]/p'` (does the
    firmware offer WoWLAN at all, for patch 0036), `iw dev wlan0 get
    power_save` (on), and in step 1's log the Wi-Fi interrupts a second
    (`ath10k`/`wlan` lines) while associated and idle.

| Measurement | pro1x.9, old user space | pro1x.10, new user space |
|---|---|---|
| Awake, display off: mean mA (without the first 5 min) | | |
| Awake: interrupts/s, top 3 sources | | |
| Awake: pro1x-wakeups top 3 processes (wake-ups/s) | | |
| After a sound: seconds until `pcm` 0 | 300 | |
| rpmtest 600 s: vlow +N / vmin +N, accumulated s | | |
| rpmtest: woken by | | |
| rpmtest with `sensors`: remoteproc crashes | 1 (2026-10-25) | |
| USB `configured` after a suspend | no | |
| Standby 1 h with autosuspend: % and mV lost | (not run) | |
| Standby: sleeps, wake-ups and their sources | | |
| gpsd / pro1x-gnss after boot: Leste, Kali, Arch | Kali, Arch: running | |
| dt2w on / off: mA | | |
| "Device on" LED on / off: mA; lit through a suspend | | |
| Bluetooth up / down, awake: mA | | |
| jbd2 + writeback wake-ups/s, commit=5 / 60 | | |
| UFS clkgate_enable, auto_hibern8 | | |
| cpuidle: menu; deeper states' usage / above | | |
| Kali, Arch: seconds until the panel is off | | |
| /, /host with lazytime,commit=30; loop0 DIO | no | |
| Wi-Fi: WoWLAN offered by the firmware; interrupts/s associated | (0.7/s, 10-25) | |
| pro1x.11: double tap in gesture mode; touch interrupts while dark | (every touch) | |

## The notify_rpm test (2026-10-31, with the owner)

What it shows: whether the RPM takes the SoC into vlow/vmin once the MPM
hands it over (patch 0011), and whether the RTC alarm (PMIC RTC over the
SPMI arbiter, an MPM pin) wakes it again. The test runs on 7.1.0-pro1x.9
(0001-0027: everything of pro1x.7, plus the loudspeaker pop change and
the headset jack's thresholds of `docs/audio.md`).

Before the flash, on the old kernel: the reference listening of
`docs/audio.md` (its step 0). Then the kernel on the phone, from the build
host (Leste, over USB):

    WORK=/run/media/sebastian/2e638a7f-26db-4e89-9446-81d688464798/pro1x-leste
    PHONE="ssh ... root@172.16.42.1" \
        WORK=$WORK OUT=$WORK/out-pro1x.9 bash scripts/65-kernel-update.sh prepare
    PHONE=... WORK=$WORK OUT=$WORK/out-pro1x.9 bash scripts/65-kernel-update.sh flash

then a reboot with the owner there (trial boot, `pro1x-boot-trial`
confirms it), `uname -r` 7.1.0-pro1x.9, Wi-Fi up, and its address. The
loudspeaker test (`docs/audio.md`, steps 1-6) fits in after the RPM test,
with the cable in again.

The test: the cable out (the USB gadget does not come back from a
suspend yet), over Wi-Fi, the phone lying still:

1. Without the hand-over, for comparison (vlow and vmin must stay +0):
   `PHONE="ssh ... root@<Wi-Fi address>" bash scripts/69-rpm-test.sh 60 norpm bt-down`
2. With it, Bluetooth down (the SoC's own state only):
   `... bash scripts/69-rpm-test.sh 60 rpm bt-down`.
   Expected: `OK: suspended`, vlow or vmin `+1` or more with an
   accumulated duration close to the 60 s, woken by the RTC alarm's
   interrupt (`pm_wakeup_irq`).
3. If 2 woke up: the same for 600 s, and the battery's voltage before and
   after (the log has both; the current while suspended cannot be read).
4. With Bluetooth up (patches 0023/0024 too):
   `... bash scripts/69-rpm-test.sh 60 rpm bt-up`; afterwards Bluetooth and
   the FM radio still work (see the section on the Bluetooth UART).
5. Awake, Bluetooth up, display off: `pro1x-wakeups 60` (and the IBS
   counters and the UART's `runtime_status`) for the wake-up audit.

`scripts/69-rpm-test.sh` copies `tools/pro1x-rpmtest` to the phone, starts
it detached, waits and fetches its log to `~/ps/pro1x-power/<date>/`.
`pro1x-rpmtest` arms the RTC alarm first, then switches `notify_rpm` on,
syncs and suspends (s2idle); back, `notify_rpm` goes off before anything
else, also when the suspend fails or the script is killed, and it is off
after any reboot. Around the suspend it stops `pro1x-hexagonrpcd`, takes
the Bluetooth UART's wake-up away and switches `pm_debug_messages` on,
and puts all of it back afterwards. Its log: `qcom_stats` before and after
(vlow, vmin and the subsystems, raw RPM values), the battery, CX in
`pm_genpd_summary`, the IBS counters, the suspend statistics, the wake-up
interrupt, the interrupts during the try and the kernel's log of it; the
last line `OK` or `FAILED` with the vlow/vmin deltas.

If the phone does not come back (the script says so about four minutes
after the alarm): the SoC did not wake up. Hold the power key until it
restarts (`qcom_wdt` stops the watchdog for a suspend, so nothing resets
it by itself); `notify_rpm` is off after the restart. Then
`FETCH=1 PHONE=... bash scripts/69-rpm-test.sh` fetches the log, which
ends before the suspend. Next steps then: the vMPM timer values and the
MPM's pin enables in the log of the hand-over (0011), or vlow without
vmin, before the test is repeated.

## FreeBSD

What FreeBSD 15 does on the Pro1X (`freebsd/`), in the same terms:

- **CPU idle:** `wfi` only, the core's clock stops, nothing more.
  FreeBSD's arm64 kernel has no deeper idle states. The SM6115's PSCI
  states are all power-down states (`arm,psci-suspend-param` 0x40000003 for
  a core, 0x41000044 for a cluster): the core loses its registers, so the
  kernel would have to save and restore its context around
  `PSCI_CPU_SUSPEND` (and come back through a warm-boot entry with the MMU
  off), and the per-CPU generic timer stops with the core, so another timer
  has to wake it (the SoC's memory-mapped `arm,armv7-timer-mem`, which
  FreeBSD has no driver for). QEMU's PSCI treats CPU_SUSPEND as `wfi`, so
  none of that can be tried in the VM.
- **Suspend:** none. FreeBSD has no suspend on arm64 (`acpi_sleep_machdep`
  is a stub there), and nothing of the Qualcomm side (RPMh/RPM votes, the
  modem and ADSP) runs under FreeBSD anyway.
- **CPU frequency:** `powerd` with `qcom_cpufreq_hw` (`-a adaptive -b
  adaptive -n adaptive -p 500`: adaptive also on a charger, where the
  default polled 4 times a second for ever); `hw.acpi.acline` from
  `pro1x_battery` tells it charger or battery.
- **Display:** DCS commands from `qcom_dpu` (`patches/0022`): brightness,
  display off/on, idle mode, partial mode, sleep in/out; the stand-by
  screen (`tools/pro1x-standby`) dims, then runs in idle and partial mode,
  and switches the panel off in a pocket. The display controller still
  sends 60 frames a second meanwhile (about 560 MB/s of memory traffic):
  stopping the interface timing engine and the DSI lanes (ULPS) while the
  panel sleeps is the next step, on the phone.
- **The rest, checked against the N9 (2026-10-26, built, untested):**
  - `qcom_wdt` fed every 15 s with 5 s of slack instead of every 5 s
    (`0023`).
  - `pro1xkbd` scans a held key every 30 ms instead of 15 (`0024`).
  - `ufshci`: the kept link gets Linux's Auto-Hibernate (150 ms) if the
    previous kernel set none (`0025`).
  - `pro1x_battery`: every 30 s on the battery (5 s on a charger), the ADC
    waits sleep instead of spinning, changes go to devd (`0026`); the
    status LED follows those events instead of polling every 10 s, the
    Hildon battery applet reads every 30 s instead of 10.
  - the random harvester once a second once seeded instead of ten times
    (`0027`, `kern.random.harvest.seeded_hz=1`).
  - `goodix` can sleep (`0028`, `dev.goodix.0.sleep`): the stand-by screen
    puts it to sleep while the proximity sensor is covered.
  - cron off; the DSP daemons restart at most 5 times, 10 s apart, instead
    of every second for ever (Nokia: `respawn limit 3 300`).
  - ZFS root: lz4 instead of zstd, a transaction group every 30 s instead
    of 5 (`vfs.zfs.txg.timeout=30`).
  - The touchscreen's bit-banged I2C follows the bus frequency, not the
    device tree's delay: `hint.iicbus.N.frequency=250000` for its bus
    once the unit number is seen on the phone.
  - Not done, larger: PSCI idle states (above), the I2C of the touchscreen
    in hardware instead of bit-banged (GPI DMA), iphbd (Hildon's shared
    wake-up heartbeat) as a daemon. (Sensor batching in the ADSP requests:
    libssc's patch, FreeBSD's too, docs/sensors.md; FreeBSD has no
    sleepstate, so non-waking requests still reach a suspend.)

So FreeBSD idles like Leste does today without autosuspend (no SoC sleep
states), minus the deeper CPU idle states Linux uses while awake.


## Code-level power fixes (2026-10-31, from the PC)

Our own programs, looked at for what they do while nobody looks at them:
(1) pause while the display is off or the window is not seen, (2) timers
grouped, (3) network updates grouped, (4) fewer writes. Leste and the MeeGo
copy learn the display's state from mce's `display_status_ind` ("on",
"dimmed", "off", and "lpm" for the stand-by screen, `patches/lpm`); FreeBSD
has no mce, so `pro1x-standby` (without `-M`, and only when nothing owns
`com.nokia.mce`) now sends the same signal: "on", "dimmed", "lpm" and "off"
when the panel goes off (`4aed93a`). Upstream hildon-desktop then stops
drawing on "off" and deletes `_HILDON_APPLET_ON_CURRENT_DESKTOP` of its
Home widgets, libosso tells the applications, and our clients pause. All
our clients treat "off" and "lpm" as not seen, "dimmed" as seen, and stay
"on" when nobody sends the signal (Arch's Hildon session today).

| Program (systems) | Scenario | Before | After | How |
|---|---|---|---|---|
| pro1x-events (Leste, FreeBSD, Arch) | page seen, display on, idle | 60 wake-ups an hour from two 60 s ticks (GLib groups them into one) | none between updates | measured, Xvfb, 300 s: 5 wake-ups before, 0 after |
| pro1x-events | display off (mce "off") | the same ticks, and the feed every 16 and the weather every 31 minutes: 5.5 requests in 5.5 radio bursts an hour | nothing at all | measured with the intervals / 15, 480 s = 2 h: 11 requests, 11 bursts, 226 wake-ups before; 0, 0, 0 after |
| pro1x-events | display on, 2 h | 11 requests in 9 bursts, the 30 posts sent and parsed each time | 12 requests in 8 bursts (feed and weather in the same second), 8 of them `304 Not Modified` without a body | measured, intervals / 15 |
| pro1x-events | a feed update with pictures shown | up to 90 `utime()` (one inode write per shown picture) every 15 min | at most once a day per picture, none on a 304 | code |
| pro1x-events | the page shown, the screen turned | 3 GConf writes each time (gconfd's file on the flash, hildon-desktop lays the widgets out again) | none when unchanged | code |
| pro1x-events | a new session with a weather cache under 30 min old | Open-Meteo asked at once | when the cache is 30 min old | code |
| Settings, Events (apps/pro1x-events) | Save without a change | 10 GConf writes; the page took them as a new place and account: a weather update, the feed cleared and fetched again | no write, nothing fetched | code |
| pro1x-volume-osd | a volume key with the display off | the window mapped and faded at 40 frames/s for 2 s, composited | nothing shown | code |
| pro1x-fm (Leste, FreeBSD, Arch) | radio on, window behind others or display off | the status asked for every 1.5 s: 2400 wake-ups an hour (FreeBSD: two socket requests each, waking pro1x-fmd and pro1x-audiod too) | none; at once when seen again | computed |
| pro1x-camera (Leste) | camera app behind others or display off | the viewfinder ran: the sensor at 1280x960, the ISP, two conversions, the flip and the video sink | stopped 1 s after the focus or the display went | code (the camera is not on the PC) |
| meego-clock (all) | the clock behind others or display off | its seconds hand: 3600 wake-ups and redraws an hour | none | computed (`73af05f` in smatkovi/meego-clock) |
| iphbd (the MeeGo session on Leste) | three clients for 120 s (every 10 s; 3..30 s; 0..20 s) | each woken at its min time: wake-ups in 48 different seconds, 99 context switches of the daemon | lined up as dsme did (until the max time, then all past their min time): 11 seconds, 34 | measured, `tests/power-code/iphb-test.py` |
| pro1x-standby (FreeBSD) | the panel off | hildon-desktop and the clients drew on | display_status_ind "off": they stop | tested under Xvfb: "on", "dimmed" after `-s`, "lpm" 3 s later, "off" after `-O` |
| pro1x-standby (FreeBSD) | `pro1x.standby_off=2` | the panel dark 2 min 33 s after the last input (`-O` counted whole minutes of the stand-by screen) | 2 min (`-O 87s`, the seconds after the stand-by screen) | code |
| harmattan sysuid (the N9's system UI, Qt 5) | idle, display on, 180 s | 21 wake-ups | 14 | measured, Xvfb, stand-in mce/UPower/BlueZ |
| harmattan sysuid | display off, 180 s | 16 (QmDisplayState never told anyone on Qt 5) | 4 | measured |
| harmattan sysuid, the Bluetooth row | menu closed, BlueZ RSSI changes 10/s, 60 s | 2993 wake-ups, 600 GetManagedObjects calls | 5, none | measured |
| a MeeGo Touch application (a spinner) | display off, 30 s | 2200 wake-ups (mcompositor never heard mce in GLX builds) | 4, again on "on" | measured |
| harmattan mcompositor | display off, 30 s | 18 | 0 | measured |
| harmattan meegotouchhome | switcher page, a window drawing at 30 frames/s, 30 s | 24702 wake-ups (a whole-window XGetImage per damage) | 1252 (at most twice a second) | measured |
| meego-music | idle, 30 s | 133 | 12 | measured |
| meego-camera | minimized, 20 s | 4404 wake-ups, 1.09 s CPU | 0 | measured |
| meego-maps | idle, 40 s | 8 rewrites of its settings | 0 | measured |
| QmBattery on UPower | 10 UPower refreshes | 22 D-Bus calls | 2 | measured |
| sysuid's clock, the volume row, meego-video, meego-phone | display off; PulseAudio missing; paused; idle | 60/h; 1200 reconnects/h; 14400/h; 120/h and ModemManager's | 0; 0 while the menu is closed; 0 after 2 s; 0 | computed |

What changed in each:

- **pro1x-events 1.3** (`b284756`): nothing while it is not seen (the look
  off, `_HILDON_APPLET_ON_CURRENT_DESKTOP` deleted, mce's "off" or "lpm"):
  no timer, update, redraw or animation frame; notifications that change
  meanwhile are read once it is seen again; a timer slack of 200 ms while
  unseen. One timer for what is due next (an update, midnight, the next
  appointment's start or end). Both updates together: one due within
  5 minutes comes along with the other; none without a network (GLib's
  network monitor: netlink on Leste's GLib 2.74, always "available" on
  FreeBSD; a pull still updates); after a failure again after 5, 10, 20,
  40 and 60 minutes, after a rejected token not until the settings change.
  The home timeline is asked with its ETag (`If-None-Match`; Mastodon's
  Rails answers 304: checked on mastodon.social's public
  `/api/v1/trends/statuses`, the home timeline needs a token and was not
  tried); Open-Meteo sends no validator, so the weather is asked for
  every 30 minutes as before.
  Cache files without fsync (`g_file_set_contents_full` without
  `DURABLE`), GConf only where a value differs.
- **pro1x-volume-osd 1.1**, **pro1x-fm** (FreeBSD revision 2, Arch 1.1),
  **pro1x-camera**, **meego-clock 0.2**: as in the table; pro1x-fm follows
  HildonWindow's `is-topmost` once hildon-desktop has set it, the camera
  Qt's application state (QtDBus for mce's signal).
- **The Harmattan Qt 5 port** (smatkovi/harmattan-qt5 `f9e97dd`..`d0c32ea`,
  pinned in `freebsd/hildon/harmattan.sh`): two port bugs were most of it.
  On Qt 5 QmDisplayState never emitted `displayStateChanged` (only the
  `connectNotify(const QMetaMethod &)` overload is called; "lpm" now maps
  to Off), and mcompositor listened to mce only in GLES2 builds while the
  port builds GLX, so it never obscured the windows and every MeeGo Touch
  application kept animating with the display off ("lpm" stays on for
  mcompositor: the stand-by screen's client draws then). Besides:
  QmHeartbeat without iphbd lines its waits up on wall-clock steps and
  ends them together, `IWokeUp()` cancels; QmBattery's kernel fallback
  once a minute through the heartbeat, UPower's values taken from its
  signal; QmTime only for `/etc/localtime`; the status bar's widgets off
  display while the display is off or the bar hidden; the status menu's
  rows (volume, internet, Bluetooth) follow their services only while
  shown; switcher thumbnails at most twice a second, the home pages built
  once per burst and only when seen; meego-music and -video poll only
  while playing, meego-camera stops when minimized, meego-maps writes only
  a changed position, meego-phone's 30 s modem poll is gone (ModemManager
  signals). Left: QmLocks, QmDeviceMode, QmSystemState, QmUSBMode and
  others have the same dead `connectNotify` (fixing them changes sysuid's
  behaviour: its own decision); the thumbnails still copy whole windows
  (XRender scaling would cut that 16 times).
- **iphbd** (`meego/iphb`, `7c8e709`): Harmattan's heartbeat as dsme had
  it: a client waits until its max time at the latest, and every client
  past its min time is woken with it; timer slack 200 ms.
- **pro1x-standby 1.2** (`4aed93a`): the signal above, `-O` in seconds
  (`87s`; minutes as before), a timer slack of 100 ms on Linux. FreeBSD's
  `.xinitrc` passes `pro1x.standby_off` minus the stand-by screen's delay
  as seconds. Untested on FreeBSD's hildon-desktop (its stop on "off" is
  upstream's code, not tried in the VM). pro1x-alarmboot takes a display
  "off" during its 10 s "switching off" countdown for the power key
  (Leste's mce sends "off" for it) and stays on; with this signal a
  time-out could have looked the same, and on FreeBSD the power key gives
  "lpm" (the stand-by screen), not "off". Since pro1x-alarmboot 1.4 its
  screens keep the display on (mce's blanking pause, X's idle time reset
  every 10 s), so no time-out falls into the countdown, and "lpm" counts
  as the power key too ([alarm-power-on.md](alarm-power-on.md)).

Checked, nothing to change:

- **rtcom-eventlogger**: it opens the database with `synchronous = OFF`
  and `journal_mode = TRUNCATE` (`src/db.c`), so nothing is ever fsynced.
  WAL with `synchronous = NORMAL` would add fsyncs at its checkpoints; WAL
  with OFF would save only the journal's create and truncate per
  transaction, but `eventlogger.c` switches the journal OFF and back to
  TRUNCATE around its bulk deletes, which fails or undoes WAL while other
  processes have the database open. It writes only when an event comes.
  (`rtcom_el_set_read_events` runs one transaction per event; Clear on the
  Events page marks a handful at a time. A transaction around the loop
  would be the one patch worth making.)
- **calendar-backend**: `synchronous=off`, `journal_mode=memory`, no
  timers, writes only on changes (our patch is the 64-bit recurrence fix).
- **patches/meego**: no periodic timers (one-shot idle callbacks only); the
  page ring's and the launcher's animations are finite timelines, and
  upstream hildon-desktop stops all drawing on "off" (`hd-dbus.c`).
- **pro1x-standby**: already event-driven (minute-aligned ticks only while
  the stand-by screen is lit, nothing while covered or off).
- **pro1x-look** (writes its key only on a change), **pro1x-rtc-sync** (one
  rename-atomic write at shutdown), **pro1x-theme-scale** (once per theme).
- **leste-apps**: pedometerhomewidget (listens only on the visible desktop,
  saves 60 s after a change), headphoned (event-driven), yappari-leste (no
  polling of ours; whatsmeow's keep-alive is the library's), wifi-assistant.
- **meego/bme** (bmed's 60 s check): the battery's uevents (every 30 s on
  the battery, kernel patch 0025) restart its `poll()`, so it rarely fires.
- Not looked at here: FreeBSD's own daemons (`freebsd/`), the sensor tools
  (`docs/sensors.md`), audio, the kernel.

To repeat the measurements (Xvfb, gtk2, gconf, json-glib and libcurl on
the PC): `tests/power-code/build-events.sh <old> <new>` builds the page at
two revisions (and `-x15` with the intervals / 15) against a stand-in of
rtcom-eventlogger, `tests/power-code/run-events.sh BIN DISPLAY PORT ON OFF
LABEL` runs one with a private session and "system" bus, a stand-in
server (an ETag for the feed, `weather.json`, Open-Meteo's answer, CC BY
4.0) and its own HOME and GConf, and counts its context switches and
requests with the display on and after mce's "off".
`tests/power-code/iphb-test.py SECONDS PID` drives a running iphbd.

What needs to be built and installed:

- Leste: `leste-hildon-2026.10.31` has pro1x-events 1.3 and
  pro1x-volume-osd 1.1 (and the Linux power work's mce and
  location-daemon; `pro1x-latest`'s manifest has them); meego-clock 0.2
  and gPodder 4.17.2+leste2 (its podcasts as `media.role=music`, so that
  the deep-buffer music sink takes them) are in their own repositories'
  releases (smatkovi/meego-clock v0.2, smatkovi/gpodder-leste
  v4.17.2+leste2). pro1x-standby (`scripts/49-standby.sh`), pro1x-fm and
  pro1x-camera (`scripts/49-apps.sh`) and iphbd
  (`meego/41-leste-helpers.sh`) are installed by their scripts, not as
  packages: run those again on the phone, or the next image has them.
- FreeBSD: pro1x-standby 1.2, pro1x-events 1.3, pro1x-volume-osd 1.1,
  pro1x-fm revision 2, meego-clock 0.2 (pin `73af05f`), gpodder-leste
  4.17.2+leste2 (pin `fb3c82f`), the root's `.xinitrc`.
- Arch: pro1x-events 1.3, pro1x-volume-osd 1.1, pro1x-fm 1.1, meego-clock
  0.2, gpodder-leste 4.17.2+leste2, with the next Arch build; Harmattan's
  parts from `arch/30-meego.sh` (it builds harmattan-qt5's main).
- Harmattan (all three systems): qmsystem2, system-ui, statusmenu-plugins,
  meegotouchhome, mcompositor and apps/meego-camera, -maps, -music, -phone,
  -video. Leste's packages are built on the phone
  (`PKGREV=3 ONLY="qmsystem2 system-ui statusmenu-plugins meegotouchhome
  mcompositor meego-camera meego-maps meego-music meego-phone meego-video"
  bash meego/50-harmattan-qt5-debs.sh`; PKGREV above every installed
  +pro1xN of them, system-ui's being +pro1x2 in leste-harmattan-2026.10.29.2,
  or apt keeps the old ones). FreeBSD: the pin `d0c32ea` and the revisions in
  `pkgmeta.py`. Before mcompositor goes on a phone, try lock, blank and
  unblank there: on "on" with tklock "locked" it now waits for the lock
  screen to paint (Nokia's logic), which on the PC never ended for a lock
  window whose stacking layer was set after it was mapped (sysuid's own
  lock screen could not be tried on the PC). Its commit (`ff4519c`) can be
  held back on Leste by leaving it out of `ONLY`.
