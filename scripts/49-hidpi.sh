#!/bin/bash
# Hildon at the N900's size on the Pro1X's full resolution
# (tools/pro1x-hidpi): the theme scaler, its boot service and session
# hook, and the Hildon packages with patches/hidpi/ (their lengths times
# HILDON_SCALE), rebuilt as <version>+pro1x1 and held, so an apt upgrade
# does not put the unpatched ones back.
#
# Runs as root INSIDE the target (chroot or the running phone), after
# 45-pro1x-hardware.sh. Needs network; gtk takes longest. The sources are
# maemo-leste's daedalus branches at fixed revisions (FreeBSD's Hildon
# build applies the same patches to master).
#
#   OUT=/dir   also copy the built packages there
#   DEBS=/dir  install packages built earlier (OUT of a run in a chroot of
#              the image) instead of building
#   ONLY="hildon-desktop ..."  build only these parts (the others are in)
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
SRC=${SRC:-/root/src}
OUT=${OUT:-}
DEBS=${DEBS:-}
ONLY=${ONLY:-}
export DEBIAN_FRONTEND=noninteractive
export DEB_BUILD_OPTIONS="nocheck nodoc parallel=$(nproc)"
log() { echo ">> $*"; }

# part|revision on maemo/daedalus, in build order
PARTS=(
  "gtk|d1e3c3b3e5e3"
  "libhildon|e734f0d533aa"
  "hildon-status-menu|33ba47660e7c"
  "hildon-home|0d34886acdc2"
  "hildon-desktop|1273f9eee252"
)
if [ -n "$ONLY" ]; then
  sel=()
  for entry in "${PARTS[@]}"; do
    case " $ONLY " in *" ${entry%%|*} "*) sel+=("$entry") ;; esac
  done
  PARTS=("${sel[@]}")
fi

apt-get update -q || log "apt-get update had errors: going on with the lists it got"
apt-get install -y -q --no-install-recommends gcc libc6-dev pkg-config \
  libgdk-pixbuf-2.0-dev

log "pro1x-theme-scale, pro1x-hidpi, boot service, session hook"
# shellcheck disable=SC2046
gcc -O2 -Wall -o /usr/local/bin/pro1x-theme-scale \
  "$REPO/tools/pro1x-hidpi/pro1x-theme-scale.c" \
  $(pkg-config --cflags --libs gdk-pixbuf-2.0) -lm
install -m 755 "$REPO/tools/pro1x-hidpi/pro1x-hidpi" /usr/local/bin/
install -m 755 "$REPO/rootfs-overlay/etc/init.d/pro1x-hidpi" /etc/init.d/
install -m 644 "$REPO/rootfs-overlay/etc/X11/Xsession.d/01pro1x-hidpi" /etc/X11/Xsession.d/
rc-update add pro1x-hidpi default

# Of packages built from one source, those the system has: installed and
# then held
held=()
install_ours() {
  local debs=() f p
  for f in "$@"; do
    p=$(dpkg-deb -f "$f" Package)
    if dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "ok installed"; then
      debs+=("$f")
      held+=("$p")
    fi
  done
  log "install ${debs[*]##*/}"
  apt-get install -y -q "${debs[@]}"
  if [ -n "$OUT" ]; then
    mkdir -p "$OUT"
    cp "${debs[@]}" "$OUT/"
  fi
}

if [ -n "$DEBS" ]; then
  install_ours "$DEBS"/*.deb
  apt-mark hold "${held[@]}"
  echo ">> done (factor: pro1x-hidpi get; change: pro1x-hidpi set 2.25, then restart X)"
  exit 0
fi

apt-get install -y -q --no-install-recommends git build-essential debhelper \
  devscripts equivs quilt

# Sources, patched and versioned; then every part's build dependencies
# first: once the patched gtk is in, the archive's libgtk2.0-dev (which
# wants its own libgtk2.0-0 exactly) could no longer be installed.
mkdir -p "$SRC"
for entry in "${PARTS[@]}"; do
  name=${entry%%|*} rev=${entry#*|}
  d=$SRC/$name
  log "$name: source at $rev"
  [ -d "$d/.git" ] || git clone -q -b maemo/daedalus "https://github.com/maemo-leste/$name.git" "$d"
  git -C "$d" fetch -q origin maemo/daedalus
  git -C "$d" checkout -q -f "$rev"
  git -C "$d" clean -fdxq
  # Quilt packages take the patch at the end of their series, the others
  # directly
  # and the MeeGo-like home (patches/meego), where a part has one
  for kind in hidpi meego; do
    p=$REPO/patches/$kind/$name.patch
    [ -f "$p" ] || continue
    if [ -f "$d/debian/patches/series" ]; then
      cp "$p" "$d/debian/patches/pro1x-$kind.patch"
      echo "pro1x-$kind.patch" >> "$d/debian/patches/series"
    else
      patch -s -d "$d" -p1 < "$p"
    fi
  done
  (cd "$d" && DEBEMAIL="pro1x-maemo-leste" dch -v "$(dpkg-parsechangelog -S Version)+pro1x1" \
    -D unstable "HiDPI: lengths times HILDON_SCALE (F(x)tec Pro1X).")
  (cd "$d" && mk-build-deps -i -r -t "apt-get -y -q --no-install-recommends" debian/control)
done

for entry in "${PARTS[@]}"; do
  name=${entry%%|*}
  d=$SRC/$name
  log "$name: build"
  (cd "$d" && dpkg-buildpackage -b -us -uc > "$SRC/$name.build.log" 2>&1) ||
    { tail -30 "$SRC/$name.build.log"; exit 1; }
  v=$(cd "$d" && dpkg-parsechangelog -S Version)
  install_ours "$SRC"/*_"${v#*:}"_*.deb
done
apt-mark hold "${held[@]}"
echo ">> done (factor: pro1x-hidpi get; change: pro1x-hidpi set 2.25, then restart X)"
