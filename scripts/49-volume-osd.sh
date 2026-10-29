#!/bin/bash
# The N9's volume bar on Leste: tools/pro1x-volume-osd, started by the X
# session (Xsession.post), shown on volume changes while Hildon's
# MeeGo-like look is on (patches/meego, GConf
# /apps/osso/hildon-desktop/meego_pages).
#
# Runs as root INSIDE the target (chroot or the running phone).
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
export DEBIAN_FRONTEND=noninteractive

apt-get install -y -q --no-install-recommends gcc libc6-dev pkg-config \
  libgtk2.0-dev libgconf2-dev libpulse-dev
# shellcheck disable=SC2046
gcc -O2 -Wall -o /usr/local/bin/pro1x-volume-osd \
  "$REPO/tools/pro1x-volume-osd/pro1x-volume-osd.c" \
  $(pkg-config --cflags --libs gtk+-2.0 gconf-2.0 libpulse-mainloop-glib) -lm
install -m 644 "$REPO/rootfs-overlay/etc/X11/Xsession.post/46pro1x-volume-osd" \
  /etc/X11/Xsession.post/
echo ">> done: pro1x-volume-osd from the next X session"
