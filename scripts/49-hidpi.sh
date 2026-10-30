#!/bin/bash
# Hildon at the N900's size on the Pro1X's full resolution
# (tools/pro1x-hidpi): the theme scaler, its boot service and session
# hook, and the Hildon packages with patches/hidpi/ (their lengths times
# HILDON_SCALE), rebuilt as <version>+pro1x<N> and held, so an apt upgrade
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

# part|revision on maemo/daedalus|N of +pro1xN (raise it when this
# repository's patches for the part change), in build order
PARTS=(
  "gtk|d1e3c3b3e5e3|1"
  "libhildon|e734f0d533aa|2"
  "hildon-status-menu|33ba47660e7c|2"
  "hildon-home|0d34886acdc2|1"
  "hildon-desktop|1273f9eee252|5"
  "mce|422d5f2bd618|3"
  # the location framework's bugs (patches/fixes): liblocation linked to
  # its libraries, its dialog calls with dbus-glib's output terminator, the
  # user's methods (the "enable positioning" dialog's yes); location-ui's
  # dialogs without an answer at the start, its answer to "close" (which
  # aborted it on every OK); a match rule's missing quote, positioning
  # started by merely creating a LocationGPSDControl; start after stop,
  # location-daemon kept running by controls not started
  "liblocation|d1ae86503411|4"
  "location-ui|55b9700aed33|2"
  # repeating appointments on 64-bit (patches/fixes): none was expanded,
  # in qalendar's views and on the Events page (pro1x-events)
  "calendar-backend|7a149bfc9f76|1"
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
  # ours from an earlier run are held. No services restarted: a restarted
  # mce leaves the display off, the new one runs after a reboot.
  local guard=
  if [ ! -e /usr/sbin/policy-rc.d ]; then
    printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
    chmod 755 /usr/sbin/policy-rc.d
    guard=1
  fi
  apt-get install -y -q --allow-change-held-packages "${debs[@]}" ||
    { [ -z "$guard" ] || rm -f /usr/sbin/policy-rc.d; return 1; }
  [ -z "$guard" ] || rm -f /usr/sbin/policy-rc.d
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
  name=${entry%%|*} rev=${entry#*|} n=${rev#*|} rev=${rev%%|*}
  d=$SRC/$name
  log "$name: source at $rev"
  [ -d "$d/.git" ] || git clone -q -b maemo/daedalus "https://github.com/maemo-leste/$name.git" "$d"
  git -C "$d" fetch -q origin maemo/daedalus
  git -C "$d" checkout -q -f "$rev"
  git -C "$d" clean -fdxq
  # Quilt packages take the patch at the end of their series, the others
  # directly
  # the MeeGo-like home (patches/meego), the display's low power mode
  # (patches/lpm) and bug fixes (patches/fixes), where a part has one
  for kind in hidpi meego lpm fixes; do
    p=$REPO/patches/$kind/$name.patch
    [ -f "$p" ] || continue
    if [ -f "$d/debian/patches/series" ]; then
      cp "$p" "$d/debian/patches/pro1x-$kind.patch"
      echo "pro1x-$kind.patch" >> "$d/debian/patches/series"
    else
      patch -s -d "$d" -p1 < "$p"
    fi
  done
  # mce also as the MeeGo boot needs it (meego/39-leste-mce.sh): it closes
  # a Harmattan system UI's lock screen, and the radiostates module maps
  # Harmattan's radio states on the device mode (loaded where
  # mce.ini.d/60-pro1x.ini names it)
  if [ "$name" = mce ]; then
    patch -s -d "$d" -p1 < "$REPO/meego/patches/mce-tklock-close-harmattan.patch"
    cp "$REPO/meego/mce/radiostates.c" "$d/src/modules/"
    cat >> "$d/src/modules/CMakeLists.txt" <<'CMAKE'

add_library(radiostates SHARED radiostates.c)
target_link_libraries(radiostates ${COMMON_LIBRARIES})
target_include_directories(radiostates PRIVATE ${COMMON_INCLUDE_DIRS} ${MODULE_INCLUDE_DIRS})
install(TARGETS radiostates DESTINATION ${MCE_MODULE_DIR})
CMAKE
  fi
  (cd "$d" && DEBEMAIL="pro1x-maemo-leste" dch -v "$(dpkg-parsechangelog -S Version)+pro1x$n" \
    -D unstable "F(x)tec Pro1X: HiDPI, MeeGo-like home, low power mode, fixes (pro1x-maemo-leste patches/).")
  # mce builds against libcal: next to 47-libcal.sh's patched libcal1 only
  # the libcal-dev of that build fits
  if [ "$name" = mce ] && ls "$SRC"/libcal-dev_*+pro1x1_*.deb > /dev/null 2>&1; then
    apt-get install -y -q "$SRC"/libcal-dev_*+pro1x1_*.deb
  fi
  (cd "$d" && mk-build-deps -i -r -t "apt-get -y -q --no-install-recommends" debian/control)
  # mce's x11-ctrl module (DPMS and the input devices off when mce blanks
  # the display) is built only where CMake finds Xlib; a build root without
  # it left the module out without a word, and the panel stayed on
  if [ "$name" = mce ]; then
    apt-get install -y -q --no-install-recommends libx11-dev libxext-dev libxi-dev
  fi
done

for entry in "${PARTS[@]}"; do
  name=${entry%%|*}
  d=$SRC/$name
  log "$name: build"
  (cd "$d" && dpkg-buildpackage -b -us -uc > "$SRC/$name.build.log" 2>&1) ||
    { tail -30 "$SRC/$name.build.log"; exit 1; }
  v=$(cd "$d" && dpkg-parsechangelog -S Version)
  if [ "$name" = mce ] && ! dpkg-deb -c "$SRC/mce_${v#*:}_$(dpkg --print-architecture).deb" |
      grep -q '/libx11-ctrl\.so$'; then
    echo "mce: built without its x11-ctrl module (see $SRC/mce.build.log)"
    exit 1
  fi
  install_ours "$SRC"/*_"${v#*:}"_*.deb
done
apt-mark hold "${held[@]}"
echo ">> done (factor: pro1x-hidpi get; change: pro1x-hidpi set 2.25, then restart X)"
