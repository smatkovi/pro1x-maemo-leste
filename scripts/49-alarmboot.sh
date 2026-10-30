#!/bin/bash
# Alarms that switch the phone on (docs/alarm-power-on.md): pro1x-alarmboot,
# the N9's alarm screens after a start by the RTC alarm, in Leste (from
# Xsession.d/02pro1x-alarmboot) and in the MeeGo system made from this
# image (from Xsession.d/01pro1x-meego). Built as its package by
# mkdeb-tools.sh and installed with apt, so that pro1x-update and later
# packages upgrade it. The boot menu's side (pro1x-boot, the initramfs) and
# the RTC alarm's translation (pro1x-rtc) are 45-pro1x-hardware.sh's and
# 46-sensor-stack.sh's.
#
# Runs as root INSIDE the target (chroot or the running phone), after
# 49-hidpi.sh (whose GTK it builds against where it rebuilt it).
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
SRC=${SRC:-/root/src}
export DEBIAN_FRONTEND=noninteractive
OUT=$SRC/alarmboot-debs
mkdir -p "$OUT"
rm -f "$OUT"/pro1x-alarmboot_*.deb
REPO=$REPO SRC=$SRC OUT=$OUT ONLY=pro1x-alarmboot bash "$REPO/scripts/mkdeb-tools.sh"
apt-get install -y -q --no-install-recommends "$OUT"/pro1x-alarmboot_*_*.deb
echo ">> done: pro1x-alarmboot $(dpkg-query -W -f='${Version}' pro1x-alarmboot), from the next X session"
