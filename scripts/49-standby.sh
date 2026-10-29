#!/bin/bash
# The N9's stand-by screen on Leste: tools/pro1x-standby in its mce mode
# (-M), started by the X session, for mce's low power mode (patches/lpm,
# built into mce by 49-hidpi.sh; switched on in 50-pro1x.ini, [Display]
# UseLowPowerMode, at run time gconf
# /system/osso/dsm/display/use_low_power_mode).
#
# Runs as root INSIDE the target (chroot or the running phone).
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
export DEBIAN_FRONTEND=noninteractive

apt-get install -y -q --no-install-recommends gcc libc6-dev pkg-config \
  libx11-dev libxext-dev libxi-dev libcairo2-dev libpango1.0-dev \
  libsqlite3-dev libdbus-1-dev
# shellcheck disable=SC2046
gcc -O2 -Wall -o /usr/local/bin/pro1x-standby \
  "$REPO/tools/pro1x-standby/pro1x-standby.c" \
  $(pkg-config --cflags --libs x11 xext xi cairo pangocairo sqlite3 dbus-1) -lm
install -m 644 "$REPO/rootfs-overlay/etc/X11/Xsession.post/45pro1x-standby" \
  /etc/X11/Xsession.post/
install -m 644 "$REPO/rootfs-overlay/etc/mce/mce.ini.d/50-pro1x.ini" \
  /etc/mce/mce.ini.d/
echo ">> done: pro1x-standby -M from the next X session; mce's low power mode"
echo "   needs mce 1.10.17+pro1x3 (49-hidpi.sh ONLY=mce)"
