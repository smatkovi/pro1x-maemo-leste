#!/bin/bash
# Leste's mce, able to close Harmattan's lock screen, with Harmattan's
# radio states (flight mode, Bluetooth) and its low power mode view.
#
# Runs as root on Leste (the phone or the rootfs chroot). Needs network.
#
# In the MeeGo boot Harmattan's sysuid owns com.nokia.system_ui. Leste's mce
# closes the lock screen with Fremantle's tklock_close(s,s,s,s,b), sysuid
# only has tklock_close(b): the call fails, mce retries it every 500 ms
# for good and stays locked. patches/mce-tklock-close-harmattan.patch falls
# back to the one-argument call. Harmattan switches flight mode through
# mce's radio states, Leste through the device mode: the module in
# mce/radiostates.c translates, and keeps the Bluetooth bit, which
# Harmattan's bluetoothd follows (off at the start, as on Leste).
#
# In low power mode (patches/lpm) mce/harmattan-lpm.c opens sysuid's own
# stand-by screen, the lock screen's low power mode view, as Harmattan's
# mce did; Hildon's system UI does not know that mode, so the module is
# switched on only in MeeGo's own system (45-meego-image.sh names it
# "meego").
#
# All are built into mce by scripts/49-hidpi.sh (as 1.10.17+pro1x6, with
# the HiDPI and low power mode changes; the package is held): one mce for
# Leste and the MeeGo boot. This script builds it (BUILD=0: only the
# settings) and switches the modules on.
set -euo pipefail
MEEGO=$(cd "$(dirname "$0")" && pwd)
INI=/etc/mce/mce.ini.d

[ "${BUILD:-1}" = 0 ] || ONLY=mce REPO="$MEEGO/.." bash "$MEEGO/../scripts/49-hidpi.sh"
# ModulesUser of a later file replaces that of an earlier one: the list of
# 50-pro1x.ini (rootfs-overlay: keyboard light, vibration, accelerometer,
# proximity, LEDs) again, then ours. With radiostates alone (until
# 2026-10-31) the proximity sensor, the LEDs and the keyboard light were off.
mods=$(sed -n 's/^ModulesUser=//p' "$INI/50-pro1x.ini" 2>/dev/null | tail -1)
mods=${mods:+$mods;}radiostates
[ "$(cat /etc/hostname)" != meego ] || mods="$mods;harmattan-lpm"
cat > "$INI/60-pro1x.ini" <<INI
# F(x)tec Pro1X: 50-pro1x.ini's modules and Harmattan's radio states
# (meego/mce/radiostates.c), in MeeGo's system also its low power mode view
# (meego/mce/harmattan-lpm.c); written by meego/39-leste-mce.sh
[Modules]
ModulesUser=$mods
INI
echo ">> done: $INI/60-pro1x.ini has ModulesUser=$mods; reboot to start the new mce"
