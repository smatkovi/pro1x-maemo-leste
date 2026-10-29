#!/bin/bash
# Builds and installs the Pro1X camera app, the FM radio and the control
# panel's Desktop applet (apps/), installs the Maemo 5 ports from their
# repositories' releases, and printing (CUPS).
#
# Runs as root INSIDE the target (chroot or the running phone), after
# 48-camera-stack.sh. Needs network for the build dependencies.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
SRC=${SRC:-/root/src}
export DEBIAN_FRONTEND=noninteractive

apt-get install -y -q --no-install-recommends build-essential qtbase5-dev \
  qtbase5-dev-tools qt5-qmake libqt5maemo5-dev libqt5x11extras5-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev curl \
  hildon-control-panel-dev libhildon1-dev libgconf2-dev libosso-dev gettext

rm -rf "$SRC/pro1x-camera"
mkdir -p "$SRC"
cp -r "$REPO/apps/pro1x-camera" "$SRC/pro1x-camera"
cd "$SRC/pro1x-camera"
qmake
make -j"$(nproc)"
make install

# Settings, Desktop: Hildon in Maemo's style or in the N9's (patches/meego)
rm -rf "$SRC/pro1x-look"
cp -r "$REPO/apps/pro1x-look" "$SRC/pro1x-look"
make -C "$SRC/pro1x-look" install PREFIX=/usr

# FM radio: the WCN3950's receiver (kernel patches 0014/0015, docs/fm-radio.md)
rm -rf "$SRC/pro1x-fm"
cp -r "$REPO/apps/pro1x-fm" "$SRC/pro1x-fm"
make -C "$SRC/pro1x-fm" install PREFIX=/usr
install -m 755 "$REPO/tools/pro1x-fm/pro1x-fm-test" /usr/bin/pro1x-fm-test
install -m 755 "$REPO/tools/pro1x-fm/pro1x-fm-audio-test" /usr/bin/pro1x-fm-audio-test

# Ports of Maemo 5 apps, each in its own repository with release packages
# (Pedometer Home Widget on tools/pro1x-steps, headphoned, gPodder 4,
# WiFi Assistant, Petrovich, Yappari on whatsmeow, the N900's Chess and
# Marbles with free pictures).
GH=https://github.com/smatkovi
for deb in \
	pedometerhomewidget/releases/download/v0.4.0/pedometerhomewidget_0.4.0_arm64.deb \
	headphoned/releases/download/v2.0.0/headphoned_2.0.0_arm64.deb \
	gpodder-leste/releases/download/v4.17.2%2Bleste1/gpodder-leste_4.17.2+leste1_all.deb \
	wifi-assistant/releases/download/v1.0.0/wifi-assistant_1.0.0_all.deb \
	petrovich/releases/download/v0.3.0/petrovich_0.3.0_arm64.deb \
	yappari-leste/releases/download/v3.0.0/yappari-leste_3.0.0_arm64.deb \
	osso-chess-ui/releases/download/v1.8.3-2%2Bleste1/osso-chess-ui_1.8.3-2+leste1_arm64.deb \
	osso-lmarbles/releases/download/v1.6.38-2%2Bleste1/osso-lmarbles_1.6.38-2+leste1_arm64.deb; do
	curl -sfL -o "/tmp/${deb##*/}" "$GH/$deb"
	apt-get install -y -q "/tmp/${deb##*/}"
	rm -f "/tmp/${deb##*/}"
done

# The N900's media player's successor from Leste's extras; sdl12-compat in
# place of SDL 1.2, so that SDL 1.2 games (Marbles) are scaled to the full
# screen
apt-get install -y -q openmediaplayer libsdl1.2-compat-shim
# Mahjong on its own: it needs osso-sounds-game-mahjong | libre-mahjong-sounds
# and osso-graphics-game-mahjong | libre-mahjong-graphics, which neither
# daedalus nor excalibur has (2026-10-29, main, contrib, non-free and the
# extras); in one call with the others it made apt refuse all of them
apt-get install -y -q osso-mahjong ||
  echo ">> osso-mahjong not installable (its sound and graphics packages are not in the archive)"

# Printing: CUPS with the command line tools (lp, lpr). No recommends, so no
# cups-browsed or avahi listening on the network; browsing stays off.
# Printers: lpadmin -p NAME -E -v ipp://PRINTER/ipp/print -m everywhere
apt-get install -y -q --no-install-recommends cups cups-client cups-bsd cups-filters
sed -i 's/^Browsing Yes/Browsing No/' /etc/cups/cupsd.conf
usermod -aG lpadmin user

# Leste does not refresh the icon cache when apps are installed.
gtk-update-icon-cache -f -q /usr/share/icons/hicolor 2>/dev/null || true
echo ">> done"
