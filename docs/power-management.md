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
for the step counter during suspend.

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
mce's timer, sensor batching in the ADSP requests (libssc asks for every
sample as a wake-up), the panel's ELVDD/ELVSS enable (gpio65) as a
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
| Bluetooth UART waking the system | 0030: its wake-up interrupt only while open, and for a suspend only when Bluetooth may wake | `echo enabled > /sys/bus/serial/devices/serial0/power/wakeup` (Bluetooth may wake it) |
| Bluetooth chip powered while "off" | 0029: power-off pulse and regulators off at hci down | boot the old kernel |
| Sound card open 300 s after a sound | 3 s with a kernel that has 0026 (PulseAudio), 3 s on PipeWire | `touch /etc/pulse/pro1x-suspend-300`; WirePlumber: the file below |
| Wi-Fi power save | on at every start of the interface, NetworkManager keeps it | `touch /etc/pro1x-wlan-powersave-off` |
| hexagonrpcd stopped for every suspend | only on kernels without 0031 | `StopServices=pro1x-hexagonrpcd` in `/etc/pro1x-autosuspend.conf` |

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

Not changed: the sensors keep streaming into a suspended system (the
ADSP's glink edge is no wake-up source, so their indications wait);
batching in the libssc requests is still open.

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
  but leaves the UART alone while that switch is on.

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
    in hardware instead of bit-banged (GPI DMA), sensor batching in the
    ADSP requests, iphbd (Hildon's shared wake-up heartbeat) as a daemon.

So FreeBSD idles like Leste does today without autosuspend (no SoC sleep
states), minus the deeper CPU idle states Linux uses while awake.

