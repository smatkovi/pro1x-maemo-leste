#!/bin/bash
# The Nokia N9's Events page on Leste: tools/pro1x-events (the first home
# view while Hildon's MeeGo-like look is on: notifications and a Mastodon
# account's home timeline), its control panel applet (Settings, Events:
# the account, apps/pro1x-events), started by the X session
# (Xsession.post). Needs hildon-desktop with patches/meego (49-hidpi.sh)
# for its scrolling.
#
# Runs as root INSIDE the target (chroot or the running phone).
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
SRC=${SRC:-/root/src}
export DEBIAN_FRONTEND=noninteractive

pkgs=(gcc libc6-dev pkg-config make gettext libgconf2-dev libjson-glib-dev
  librtcom-eventlogger-dev hildon-control-panel-dev libosso-dev libx11-dev)
pkg-config --exists libcurl 2> /dev/null || pkgs+=(libcurl4-openssl-dev)
# The -dev packages of what 49-hidpi.sh rebuilt (+pro1xN, held) from that
# build, where they are missing: the archive's want the archive's libraries
# exactly
debs=()
for p in libgtk2.0-dev libhildon1-dev; do
  dpkg-query -W -f='${Status}' "$p" 2> /dev/null | grep -q "ok installed" && continue
  f=$(ls "$SRC"/"$p"_*+pro1x*_*.deb 2> /dev/null | tail -1 || true)
  if [ -n "$f" ]; then debs+=("$f"); else pkgs+=("$p"); fi
done
apt-get install -y -q --no-install-recommends "${pkgs[@]}" "${debs[@]}"

rm -rf "$SRC/pro1x-events"
mkdir -p "$SRC"
cp -r "$REPO/apps/pro1x-events" "$SRC/pro1x-events"
make -C "$SRC/pro1x-events" install PREFIX=/usr \
  PAGE="$REPO/tools/pro1x-events/pro1x-events.c"
install -m 644 "$REPO/rootfs-overlay/etc/X11/Xsession.post/47pro1x-events" \
  /etc/X11/Xsession.post/
echo ">> done: the Events page from the next X session (Settings, Events: the Mastodon account)"
