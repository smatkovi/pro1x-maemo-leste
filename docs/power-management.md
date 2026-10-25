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

Over Wi-Fi (USB gets disconnected when unplugged), display off:

1. After the reboot that loads the new `qcom_glink_smem`:
   `cat /sys/kernel/debug/wakeup_sources | grep glink` lists the modem edge.
2. One suspend with an RTC wake-up, on USB:
   `pro1x-autosuspend --test 20`. Expected: "resumed after ~20 s asleep",
   wake-up interrupt of the RTC alarm. If it does not come back, the power
   key has to wake it.
3. Whether the SoC sleeps: the same line reports `vlow +N, vmin +N`. If both
   stay 0, repeat with `hciconfig hci0 down`, then unplugged from USB, to
   find what still holds it.
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
