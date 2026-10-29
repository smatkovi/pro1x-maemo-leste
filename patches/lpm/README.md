# The display's low power mode in Leste's mce

`mce.patch` ports the low power mode of Harmattan's mce (1.11.20, LGPL,
from Nokia's N9 sources) to Leste's mce (maemo/daedalus 1.10.17). It is
built by `scripts/49-hidpi.sh` (`ONLY=mce`) as `mce 1.10.17+pro1x3`.

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
Untested on the phone.
