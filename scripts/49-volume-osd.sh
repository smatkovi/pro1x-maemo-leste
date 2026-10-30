#!/bin/bash
# The N9's volume bar on Leste: tools/pro1x-volume-osd, started by the X
# session (Xsession.post), shown on volume changes while Hildon's
# MeeGo-like look is on (patches/meego, GConf
# /apps/osso/hildon-desktop/meego_pages).
#
# Runs as root INSIDE the target (chroot or the running phone). As a
# package: scripts/mkdeb-tools.sh (pro1x-volume-osd_<version>_arm64.deb).
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
export DEBIAN_FRONTEND=noninteractive

# libgtk2.0-dev only where it is missing: after 49-hidpi.sh it is the
# patched gtk's (+pro1xN, held), and asking apt for it again pulls the
# archive's, which wants the archive's libgtk2.0-0 exactly
pkgs=(gcc libc6-dev pkg-config libgconf2-dev libpulse-dev)
dpkg-query -W -f='${Status}' libgtk2.0-dev 2> /dev/null | grep -q "ok installed" ||
  pkgs+=(libgtk2.0-dev)
apt-get install -y -q --no-install-recommends "${pkgs[@]}"
# shellcheck disable=SC2046
gcc -O2 -Wall -o /usr/bin/pro1x-volume-osd \
  "$REPO/tools/pro1x-volume-osd/pro1x-volume-osd.c" \
  $(pkg-config --cflags --libs gtk+-2.0 gconf-2.0 libpulse-mainloop-glib) -lm
# where this script put it before (the session hook takes /usr/bin first)
rm -f /usr/local/bin/pro1x-volume-osd
install -m 644 "$REPO/rootfs-overlay/etc/X11/Xsession.post/46pro1x-volume-osd" \
  /etc/X11/Xsession.post/
echo ">> done: pro1x-volume-osd from the next X session"
