#!/bin/bash
# Builds and installs the Pro1X apps from apps/: the camera and the
# pedometer home widget; and printing (CUPS).
#
# Runs as root INSIDE the target (chroot or the running phone), after
# 48-camera-stack.sh. Needs network for the build dependencies.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
SRC=${SRC:-/root/src}
export DEBIAN_FRONTEND=noninteractive

apt-get install -y -q --no-install-recommends build-essential qtbase5-dev \
  qtbase5-dev-tools qt5-qmake libqt5maemo5-dev libqt5x11extras5-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libgtk2.0-dev libhildon1-dev libhildondesktop1-dev

rm -rf "$SRC/pro1x-camera"
mkdir -p "$SRC"
cp -r "$REPO/apps/pro1x-camera" "$SRC/pro1x-camera"
cd "$SRC/pro1x-camera"
qmake
make -j"$(nproc)"
make install

# Pedometer Home Widget (the step counter of tools/pro1x-steps)
rm -rf "$SRC/pedometer-home-widget"
cp -r "$REPO/apps/pedometer-home-widget" "$SRC/pedometer-home-widget"
make -C "$SRC/pedometer-home-widget" install

# Printing: CUPS with the command line tools (lp, lpr). No recommends, so no
# cups-browsed or avahi listening on the network; browsing stays off.
# Printers: lpadmin -p NAME -E -v ipp://PRINTER/ipp/print -m everywhere
apt-get install -y -q --no-install-recommends cups cups-client cups-bsd cups-filters
sed -i 's/^Browsing Yes/Browsing No/' /etc/cups/cupsd.conf
usermod -aG lpadmin user

# Leste does not refresh the icon cache when apps are installed.
gtk-update-icon-cache -f -q /usr/share/icons/hicolor 2>/dev/null || true
echo ">> done"
