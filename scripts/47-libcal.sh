#!/bin/bash
# Builds and installs a libcal that accepts the block2mtd CAL emulation.
#
# Runs as root INSIDE the target (chroot or the running phone), after
# 45-pro1x-hardware.sh, which installs /etc/init.d/pro1x-cal. Needs network.
#
# Maemo stores the device lock code (libdevlock) in CAL on /dev/mtd1. Leste's
# libcal only accepts the N900's OneNAND there; patches/libcal-accept-
# block2mtd.patch lets it use the MTD_RAM device pro1x-cal provides. The
# package gets a +pro1x1 version and is held, so an apt upgrade does not put
# the unpatched library back.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
SRC=${SRC:-/root/src}
LIBCAL_REV=da48175f   # maemo-leste/libcal 0.3.2-0
export DEBIAN_FRONTEND=noninteractive

apt-get install -y -q --no-install-recommends git build-essential debhelper \
  devscripts autoconf automake libtool

mkdir -p "$SRC"
[ -d "$SRC/libcal/.git" ] || git clone -q https://github.com/maemo-leste/libcal.git "$SRC/libcal"
cd "$SRC/libcal"
git fetch -q origin
git checkout -q -f "$LIBCAL_REV"
git clean -fdq
git apply "$REPO/patches/libcal-accept-block2mtd.patch"
DEBEMAIL="pro1x-maemo-leste" dch -v 0.3.2-0+pro1x1 -D unstable \
  "Accept a block2mtd-emulated CAL partition (F(x)tec Pro1X)."
dpkg-buildpackage -b -us -uc

apt-get install -y -q "$SRC"/libcal1_0.3.2-0+pro1x1_*.deb
apt-mark hold libcal1
rc-update add pro1x-cal default
echo ">> done"
