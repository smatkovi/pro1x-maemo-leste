#!/bin/bash
# Builds and installs the Pro1X apps from apps/ (currently the camera).
#
# Runs as root INSIDE the target (chroot or the running phone), after
# 48-camera-stack.sh. Needs network for the build dependencies.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
SRC=${SRC:-/root/src}
export DEBIAN_FRONTEND=noninteractive

apt-get install -y -q --no-install-recommends build-essential qtbase5-dev \
  qtbase5-dev-tools qt5-qmake libqt5maemo5-dev libqt5x11extras5-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev

rm -rf "$SRC/pro1x-camera"
mkdir -p "$SRC"
cp -r "$REPO/apps/pro1x-camera" "$SRC/pro1x-camera"
cd "$SRC/pro1x-camera"
qmake
make -j"$(nproc)"
make install
# Leste does not refresh the icon cache when apps are installed.
gtk-update-icon-cache -f -q /usr/share/icons/hicolor 2>/dev/null || true
echo ">> done"
