# The display's low power mode in Leste's mce

`mce.patch` ports the low power mode of Harmattan's mce (1.11.20, LGPL,
from Nokia's N9 sources) to Leste's mce (maemo/daedalus 1.10.17). It is
built by `scripts/49-hidpi.sh` (`ONLY=mce`), since 2026-10-31 as
`mce 1.10.17+pro1x5` (+pro1x3 and +pro1x4 had the bugs below).

## How it behaves

As on the N9: instead of blanking, the display goes to *low power mode*:
the panel in its own low power mode (the sysfs file `lpm` of the
backlight's device, which the Pro1X's panel driver has from
`device/kernel/patches/0007`) at low brightness, showing a stand-by
screen. With the proximity sensor covered for 5 seconds (a pocket, a
table) it goes dark; uncovered, the stand-by screen comes back. A double
tap on it (`pro1x-standby -M`, which asks mce for the display) or the
power key bring the display back, with the lock screen.

- New display states `MCE_DISPLAY_LPM_ON` and `MCE_DISPLAY_LPM_OFF`.
  Requests to switch the display off become `LPM_ON` while low power mode
  is in use and the system is in user state, as Harmattan's display state
  filter does.
- `display_status_ind` says `lpm` for `LPM_ON` (`off` for `LPM_OFF`), so
  that hildon-desktop keeps drawing and the stand-by screen's client knows
  when to show; Harmattan said `off` and opened its lock screen's low power
  view itself.
- Everywhere else low power mode counts as off (`DISPLAY_IS_OFF`): the
  device locks, the light and orientation sensors, the keyboard light and
  the inactivity timer stop, LEDs and vibration follow their display-off
  policy. Except: X stays on in `LPM_ON` (the stand-by screen draws
  there), the touchscreen stays on in `LPM_ON` (for the double tap), and a
  touch alone does not end low power mode.
- The proximity sensor is claimed while in low power mode
  (`iio-proximity`), as during calls.
- A display-on request ends low power mode also while locked.
- The power key, the keyboard slide opened, a call and an alarm ringing
  end it as they end off: the display on, with the lock screen where the
  device is locked. Leste's lock module (`lock-tklock`, the one
  `mce.ini.d/10-maemo.ini` loads; `lock-generic` is not loaded) does it
  for the power key and the slide through synthesised activity, which
  low power mode ignores; so in low power mode it switches the display
  on itself, as Harmattan's `trigger_visual_tklock` did.
- Locked, the touchscreen follows the two states: on in `LPM_ON`, off in
  `LPM_OFF` (covered, or after `LowPowerModeBlankTimeout`).

## Fixed in +pro1x5 (2026-10-31, found on the phone)

On the phone (+pro1x3) a short press of the power key did not switch the
display on; about 5 s after the last press only the stand-by screen came
(dark: nothing drew on it), and only a press then gave the desktop.

- **The state's cache.** mce's datapipes cache what goes *in*, before the
  filters (`datapipe.c`), and the modules read that cache
  (`datapipe_get_gint`). Harmattan's display state filter therefore
  writes its result into `display_state_pipe.cached_data`; the port had
  left that line out. So `LPM_ON` read as `off` everywhere except in the
  triggers' argument: `get_display_status` and `display_status_ind` said
  `off` (pro1x-standby never showed: the panel lit at 20 % with no
  stand-by screen on it), the proximity sensor never blanked it, the
  touchscreen was switched off in it (no double tap), a display-on
  request was refused while locked, and activity *did* end it.
- **The power key.** A press from `LPM_OFF` opened the lock screen
  (`tklock_open`, visual) and synthesised activity, which `display.c`
  ignores in low power mode: nothing switched the display on. The lock
  screen's own 5 s time-out (`DEFAULT_VISUAL_BLANK_DELAY`, renewed by
  each press) then asked for "off", which became `LPM_ON`: the stand-by
  screen 5 s after the last press, for `LowPowerModeBlankTimeout`. A
  press in that `LPM_ON` worked only because the cache said `off`.
- The keyboard slide opened in low power mode: unlocked, but left the
  display in it. The touchscreen stayed as it was between `LPM_ON` and
  `LPM_OFF` with the lock screen up.

## Settings

`mce.ini`, `[Display]`:

    UseLowPowerMode=1            # off by default
    LowPowerModeBrightness=20    # percent
    LowPowerModeBlankTimeout=0   # seconds, 0: stays

At run time: gconf `/system/osso/dsm/display/use_low_power_mode`. The
Pro1X's `rootfs-overlay/etc/mce/mce.ini.d/50-pro1x.ini` switches it on;
`scripts/49-standby.sh` builds `pro1x-standby` and starts it in its mce
mode with the X session.

Built and tried: the patched mce compiles for Leste daedalus (arm64);
`pro1x-standby -M` against simulated `display_status_ind` signals (shows
on `lpm`, sends `req_display_state_on` on a double tap, hides on `on`).
`tests/mce-lpm/run-pc.sh` runs the built mce (under qemu in
`scripts/leste-chroot.sh`'s chroot, in its own PID, mount and network
namespaces) against a fake panel, a FIFO for the power key and the slide,
a private system bus and a stand-in system UI. On 2026-10-31 +pro1x4
reproduced the phone (a press while blank: still off, 5 s later the
stand-by screen with the status "off", the touchscreen off on it, the
double tap's request refused, the slide unlocking without light); +pro1x5
switched the display on after each press and slide, said `lpm` on the
stand-by screen and honoured the request. +pro1x5 is untested on the
phone.
