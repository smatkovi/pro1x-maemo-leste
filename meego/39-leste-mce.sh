#!/bin/bash
# Leste's mce, able to close Harmattan's lock screen and with Harmattan's
# radio states (flight mode).
#
# Runs as root on Leste (the phone or the rootfs chroot). Needs network.
#
# In the MeeGo boot Harmattan's sysuid owns com.nokia.system_ui. Leste's mce
# closes the lock screen with Fremantle's tklock_close(s,s,s,s,b), sysuid
# only has tklock_close(b): the call fails, mce retries it every 500 ms
# for good and stays locked. patches/mce-tklock-close-harmattan.patch falls
# back to the one-argument call. Harmattan switches flight mode through
# mce's radio states, Leste through the device mode: the module in
# mce/radiostates.c translates.
#
# Both are built into mce by scripts/49-hidpi.sh (as 1.10.17+pro1x3, with
# the HiDPI and low power mode changes; the package is held): one mce for
# Leste and the MeeGo boot. This script builds it and switches the module
# on.
set -euo pipefail
MEEGO=$(cd "$(dirname "$0")" && pwd)

ONLY=mce REPO="$MEEGO/.." bash "$MEEGO/../scripts/49-hidpi.sh"
cat > /etc/mce/mce.ini.d/60-pro1x.ini <<'INI'
# F(x)tec Pro1X: Harmattan's radio states (meego/mce/radiostates.c)
[Modules]
ModulesUser=radiostates
INI
echo ">> done, reboot to start the new mce"
