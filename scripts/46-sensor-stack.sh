#!/bin/bash
# Builds and installs the ADSP sensor stack in a Maemo Leste rootfs.
#
# Runs as root INSIDE the target (chroot from 40-make-rootfs.sh, or on the
# running phone), after 45-pro1x-hardware.sh. Needs network.
#
#   NIXOS=/path/to/fxtec-pro1x-nixos ./46-sensor-stack.sh
#
# The Pro1X sensors (accelerometer, gyroscope, magnetometer, light, proximity)
# belong to the ADSP (Qualcomm SSC). The chain is:
#
#   hexagonrpcd   serves the ADSP its sensor configuration over FastRPC
#                 (linux-msm/hexagonrpc pull request 27 + the NixOS port's
#                 sns-registry patch); QRTR service 400 appears.
#   libssc        talks to that service (needs libqmi >= 1.33.4; daedalus has
#                 1.32, so libqmi 1.34 is built too). patches/libssc-pedometer
#                 adds the ADSP's step counter ("pedometer"; ssccli --sensor
#                 pedometer), which pro1x-steps serves on D-Bus.
#   iio-sensor-proxy 3.9, the first release with SSC support; built with a
#                 tiny g_autofd shim because daedalus has GLib 2.74, not 2.76.
#                 It replaces Leste's 3.0 through a dpkg diversion, so mce's
#                 iio modules see the sensors through the usual D-Bus API.
#
# The same libqmi also carries GPS: tools/pro1x-gnss reads the modem's QMI
# LOC position reports and feeds NMEA to gpsd (see /etc/conf.d/gpsd).
#
# Everything goes to /usr/local; the sensor data files go to
# /usr/share/pro1x/hexagonfs and are used by /etc/init.d/pro1x-hexagonrpcd.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
NIXOS=${NIXOS:?set NIXOS=<checkout of fxtec-pro1x-nixos>}
SRC=${SRC:-/root/src}
export DEBIAN_FRONTEND=noninteractive
export PKG_CONFIG_PATH=/usr/local/lib/aarch64-linux-gnu/pkgconfig

HEXAGONRPC_REV=40e50412812adce9dddc2be9a4510ebce05eef89
LIBQMI_TAG=1.34.0
LIBSSC_TAG=v0.4.4
IIO_SENSOR_PROXY_TAG=3.9

log() { echo ">> $*"; }

log "build dependencies"
apt-get install -y -q build-essential pkg-config git cmake ninja-build \
  libglib2.0-dev libgudev-1.0-dev libpolkit-gobject-1-dev libqrtr-glib-dev \
  libprotobuf-c-dev protobuf-c-compiler protobuf-compiler libprotobuf-dev \
  python3-dev qrtr-tools
# hexagonrpc and libssc need meson >= 1.1 / >= 1.4; daedalus has 1.0.1.
apt-get install -y -q -t daedalus-backports meson

mkdir -p "$SRC"
fetch() {  # fetch <dir> <url> <ref to fetch> <commit or tag to check out>
  [ -d "$SRC/$1/.git" ] || git clone -q "$2" "$SRC/$1"
  git -C "$SRC/$1" fetch -q --tags origin "$3"
  git -C "$SRC/$1" checkout -q -f "$4"
  git -C "$SRC/$1" clean -fdq   # drop files of an earlier patch run
}
build() {  # build <dir> [meson options...]
  local d=$1; shift
  rm -rf "$SRC/$d/build"
  meson setup "$SRC/$d/build" "$SRC/$d" -Dprefix=/usr/local "$@"
  ninja -C "$SRC/$d/build" install
  ldconfig
}

log "hexagonrpcd"
fetch hexagonrpc https://github.com/linux-msm/hexagonrpc.git pull/27/head "$HEXAGONRPC_REV"
git -C "$SRC/hexagonrpc" apply "$NIXOS/packages/hexagonrpc-sns-registry.patch"
build hexagonrpc

log "libqmi $LIBQMI_TAG"
fetch libqmi https://gitlab.freedesktop.org/mobile-broadband/libqmi.git "$LIBQMI_TAG" "$LIBQMI_TAG"
build libqmi -Dmbim_qmux=false -Dqrtr=true -Dgtk_doc=false -Dintrospection=false \
  -Dbash_completion=false -Dman=false

log "libssc $LIBSSC_TAG"
fetch libssc https://codeberg.org/DylanVanAssche/libssc.git "$LIBSSC_TAG" "$LIBSSC_TAG"
git -C "$SRC/libssc" apply "$REPO/patches/libssc-pedometer.patch"
# libssc waited for its own replies by spinning through non-blocking main
# loop iterations: a busy core for as long as a reply took
git -C "$SRC/libssc" apply "$REPO/patches/libssc-sync-wait.patch"
build libssc

log "iio-sensor-proxy $IIO_SENSOR_PROXY_TAG"
fetch iio-sensor-proxy https://gitlab.freedesktop.org/hadess/iio-sensor-proxy.git "$IIO_SENSOR_PROXY_TAG" "$IIO_SENSOR_PROXY_TAG"
git -C "$SRC/iio-sensor-proxy" apply "$REPO/patches/iio-sensor-proxy-3.9-glib-2.74.patch"
build iio-sensor-proxy -Dssc-support=enabled -Dsystemdsystemunitdir=no \
  -Dudevrulesdir=/usr/local/lib/udev/rules.d -Dtests=false -Dgtk-tests=false -Dgtk_doc=false
rm -rf /usr/local/no   # systemdsystemunitdir=no still installs the unit there

log "replace Leste's iio-sensor-proxy 3.0 through a diversion"
if ! dpkg-divert --list /usr/sbin/iio-sensor-proxy | grep -q .; then
  dpkg-divert --local --rename --divert /usr/sbin/iio-sensor-proxy.leste --add /usr/sbin/iio-sensor-proxy
fi
ln -sfn /usr/local/libexec/iio-sensor-proxy /usr/sbin/iio-sensor-proxy

log "pro1x-gnss (modem GNSS -> NMEA -> gpsd)"
gcc -O2 -Wall -o /usr/local/bin/pro1x-gnss "$REPO/tools/pro1x-gnss/pro1x-gnss.c" \
  $(pkg-config --cflags --libs qmi-glib qrtr-glib)
log "pro1x-steps (the ADSP's step counter on D-Bus)"
gcc -O2 -Wall -o /usr/local/sbin/pro1x-steps "$REPO/tools/pro1x-steps/pro1x-steps.c" \
  $(PKG_CONFIG_PATH=/usr/local/lib/aarch64-linux-gnu/pkgconfig pkg-config --cflags --libs libssc gio-unix-2.0)
install -m 644 "$REPO/rootfs-overlay/etc/dbus-1/system.d/org.pro1x.Steps.conf" /etc/dbus-1/system.d/
install -m 755 "$REPO/rootfs-overlay/etc/init.d/pro1x-steps" /etc/init.d/
log "pro1x-rtc (real time across power off, wake alarms from power off)"
gcc -O2 -Wall -o /usr/local/sbin/pro1x-rtc "$REPO/tools/pro1x-rtc/pro1x-rtc.c"
gcc -O2 -Wall -fPIC -shared -fvisibility=hidden -o /usr/local/lib/aarch64-linux-gnu/librtc-realtime.so \
  "$REPO/tools/pro1x-rtc/rtc-realtime.c"
install -m 755 "$REPO/rootfs-overlay/etc/init.d/pro1x-rtc" /etc/init.d/
install -m 755 "$REPO/rootfs-overlay/usr/local/sbin/alarmd-pro1x" /usr/local/sbin/
install -m 644 "$REPO/rootfs-overlay/etc/udev/rules.d/84-pro1x-rtc.rules" /etc/udev/rules.d/

log "pro1x-autosuspend (suspend while the display is off; not enabled)"
gcc -O2 -Wall -o /usr/local/sbin/pro1x-autosuspend "$REPO/tools/pro1x-autosuspend/pro1x-autosuspend.c" \
  $(pkg-config --cflags --libs gio-unix-2.0)
install -m 755 "$REPO/rootfs-overlay/etc/init.d/pro1x-autosuspend" /etc/init.d/
[ -e /etc/pro1x-autosuspend.conf ] ||
  install -m 644 "$REPO/tools/pro1x-autosuspend/pro1x-autosuspend.conf" /etc/
# alarmd (started by dsme) runs through that wrapper; only DAEMON changes
sed -i 's|^DAEMON=.*|DAEMON="/usr/local/sbin/alarmd-pro1x"|' /etc/conf.d/alarmd
# gpsd-clients recommends matplotlib and friends; gpspipe/cgps need none of it.
apt-get install -y -q --no-install-recommends gpsd gpsd-clients

log "sensor data files from the NixOS port"
rm -rf /usr/share/pro1x/hexagonfs
install -d /usr/share/pro1x
cp -r "$NIXOS/device/hexagonfs" /usr/share/pro1x/hexagonfs
rm -f /usr/share/pro1x/hexagonfs/README.md

rc-update add pro1x-hexagonrpcd default
rc-update add pro1x-steps default
rc-update add pro1x-rtc boot
log "done"
