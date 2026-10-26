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
  has its wake-up interrupt for system suspend. In a suspend the serial
  port is suspended anyway; while awake it costs CX nominal.
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

## Changes after the first suspends (2026-10-26, untested)

`pro1x-autosuspend` now does this around every suspend (also with
`--test`), each a setting in `/etc/pro1x-autosuspend.conf`:

- `StopServices=pro1x-hexagonrpcd`: stopped before the suspend, started
  again after it. With hexagonrpcd running, a suspend crashes the ADSP's
  sensor process on other Qualcomm phones too; postmarketOS and Debian
  stop it around a suspend
  ([pmaports MR 5400](https://gitlab.postmarketos.org/postmarketOS/pmaports/-/merge_requests/5400),
  [Debian's README](https://metadata.ftp-master.debian.org/changelogs/main/h/hexagonrpc/stable_README.Debian)).
  hexagonrpcd only serves the ADSP files (configuration, registry), which
  it asks for when the sensors start; libssc and iio-sensor-proxy talk to
  the ADSP over QRTR without it.
- `NoWakeup=4a8c000.serial`: the Bluetooth UART cannot wake the system.
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
  loudspeaker pop (`50-pro1x.pa`); the N9 used 1-5 s. Fixing the pop in
  the aw87339 sequencing would allow 5 s.
- MPM: GPIOs 106, 107 (keyboard) and 112 are not in the MPM map, so the
  keyboard cannot wake the SoC from vmin; the MPM's timer wake-up is not
  programmed by mainline.

Larger, not done: the touchscreen asleep while dt2w is off or the phone is
in a pocket (needs mce's proximity), the LED patterns in the LPG instead of
mce's timer, sensor batching in the ADSP requests (libssc asks for every
sample as a wake-up), the panel's ELVDD/ELVSS enable (gpio65) as a
regulator, the Bluetooth UART clock off between transfers (`hci_qca`'s
empty `__serial_clock_off`).

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

