#!/bin/bash
# Mobile data on Leste (docs/leste-mobile-data.md): Maemo Leste's oFono
# (1:2.18.0-1+4m7.1) rebuilt with patches/fixes/ofono.patch, which sets up
# the mainline IPA driver's data path as ModemManager does (DPM port, WDA
# data format, rmnet links with the IPA's checksum offload), as
# <version>+pro1x<N> and held; and the boot service pro1x-modem-wait in
# place of pro1x-rmnet (whose rmnet_data links oFono deletes when it
# starts). Calls and SMS go through the same oFono as before.
#
# Runs as root INSIDE the target (a chroot of the image, as
# 41-leste-debs-chroot.sh sets one up, or the running phone), after
# 45-pro1x-hardware.sh. Building needs network: the source package comes
# from Leste's archive (checked against the sha256 sums below, those of its
# Sources index), the build dependencies from apt.
#
#   OUT=/dir   also copy the built packages there
#   DEBS=/dir  install packages built earlier (the release's ofono and
#              ofono-scripts) instead of building
#   BUILD_ONLY=1  build, install nothing
#
# Installing does not restart oFono (a call would drop): the new one runs
# after the next boot, or after /etc/init.d/ofono restart. The two go
# together: 45-pro1x-hardware.sh no longer enables pro1x-rmnet, and the
# stock oFono without its rmnet_data links has no modem (no calls).
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
SRC=${SRC:-/root/src}
OUT=${OUT:-}
DEBS=${DEBS:-}
BUILD_ONLY=${BUILD_ONLY:-0}
export DEBIAN_FRONTEND=noninteractive
export DEB_BUILD_OPTIONS="nocheck nodoc parallel=$(nproc)"
log() { echo ">> $*"; }

# the source package (Leste daedalus main) and N of +pro1xN (raise it when
# patches/fixes/ofono.patch changes)
POOL=https://maedevu.maemo.org/leste/pool/main/o/ofono
UPSTREAM=2.18.0
DEBREV=2.18.0-1+4m7.1
EPOCH=1
N=1
SUMS="6099c9dc2f120a2b9a22ee06fc5befa5019794ed9a6815dec92945621158903b  ofono_$DEBREV.dsc
7b1a2e08311feb598a3257428b09f2ab2811fe7ac3db4ec91276390f36a998bf  ofono_$UPSTREAM.orig.tar.gz
774c753458ff8c51c202f0ecc816760ee0c74afa25ef3fc10fc9308f9ea81d79  ofono_$DEBREV.debian.tar.xz"
VERSION=$EPOCH:$DEBREV+pro1x$N

install_ours() {
  local debs=() f p cur
  for f in "$@"; do
    p=$(dpkg-deb -f "$f" Package)
    case $p in ofono | ofono-scripts) debs+=("$f") ;; esac
  done
  [ ${#debs[@]} -gt 0 ] || { echo "no ofono packages in $*"; exit 1; }

  # the way back: the archive's ofono, while it is installed
  cur=$(dpkg-query -W -f='${Version}' ofono 2> /dev/null || true)
  if [ -n "$cur" ] && [[ $cur != *+pro1x* ]]; then
    mkdir -p /root/ofono-orig
    (cd /root/ofono-orig && apt-get download -q "ofono=$cur") ||
      log "could not keep a copy of ofono $cur (apt-get download)"
  fi

  log "install ${debs[*]##*/}"
  # no service (re)started by the packages: oFono keeps running
  local guard=
  if [ ! -e /usr/sbin/policy-rc.d ]; then
    printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
    chmod 755 /usr/sbin/policy-rc.d
    guard=1
  fi
  apt-get install -y -q --allow-change-held-packages --allow-downgrades "${debs[@]}" ||
    { [ -z "$guard" ] || rm -f /usr/sbin/policy-rc.d; return 1; }
  [ -z "$guard" ] || rm -f /usr/sbin/policy-rc.d
  local held=(ofono)
  dpkg-query -W -f='${Status}' ofono-scripts 2> /dev/null | grep -q "ok installed" &&
    held+=(ofono-scripts)
  apt-mark hold "${held[@]}"

  log "boot: pro1x-modem-wait instead of pro1x-rmnet"
  install -m 755 "$REPO/rootfs-overlay/etc/init.d/pro1x-modem-wait" /etc/init.d/
  rc-update del pro1x-rmnet default 2> /dev/null || true
  rm -f /etc/init.d/pro1x-rmnet
  [ -e /etc/runlevels/default/pro1x-modem-wait ] ||
    rc-update add pro1x-modem-wait default
  # the rmnet links' data format, which Leste's iproute2 does not show
  install -m 755 "$REPO/tools/pro1x-rmnet-flags" /usr/local/bin/
  # qrtr-lookup for pro1x-modem-wait (45-pro1x-hardware.sh installs it)
  command -v qrtr-lookup > /dev/null ||
    apt-get install -y -q --no-install-recommends qrtr-tools ||
    log "qrtr-tools missing: pro1x-modem-wait waits only for rmnet_ipa0"
  echo ">> done: ofono $(dpkg-query -W -f='${Version}' ofono); active after a reboot (or /etc/init.d/ofono restart)"
}

if [ -n "$DEBS" ]; then
  install_ours "$DEBS"/*.deb
  exit 0
fi

apt-get update -q || log "apt-get update had errors: going on with the lists it got"
apt-get install -y -q --no-install-recommends build-essential debhelper \
  devscripts equivs quilt dpkg-dev wget ca-certificates

d=$SRC/ofono
log "ofono $DEBREV from $POOL"
rm -rf "$d"
mkdir -p "$d/dl"
for f in $(echo "$SUMS" | awk '{ print $2 }'); do
  wget -q -O "$d/dl/$f" "$POOL/$f"
done
(cd "$d/dl" && echo "$SUMS" | sha256sum -c --quiet)
(cd "$d" && dpkg-source -x "dl/ofono_$DEBREV.dsc" src > "$d/dpkg-source.log" 2>&1) ||
  { cat "$d/dpkg-source.log"; exit 1; }
s=$d/src
[ "$(cat "$s/debian/source/format")" = "3.0 (quilt)" ]
cp "$REPO/patches/fixes/ofono.patch" "$s/debian/patches/pro1x-mobile-data.patch"
echo "pro1x-mobile-data.patch" >> "$s/debian/patches/series"
(cd "$s" && DEBEMAIL="pro1x-maemo-leste" dch -v "$VERSION" -D unstable \
  "F(x)tec Pro1X: mobile data over the mainline IPA driver (pro1x-maemo-leste patches/fixes/ofono.patch).")
(cd "$s" && mk-build-deps -i -r -t "apt-get -y -q --no-install-recommends" debian/control)

log "ofono: build"
(cd "$s" && dpkg-buildpackage -b -us -uc > "$d/build.log" 2>&1) ||
  { tail -40 "$d/build.log"; exit 1; }
v=${VERSION#*:}
ls "$d"/*_"$v"_*.deb
# the patch is in the daemon (dpkg-source applies the series before the
# build and takes it off again after it)
dpkg-deb --fsys-tarfile "$d/ofono_${v}_$(dpkg --print-architecture).deb" |
  tar -xO ./usr/sbin/ofonod > "$d/ofonod.check"
grep -a -q 'no mobile data: ' "$d/ofonod.check" ||
  { echo "ofonod without patches/fixes/ofono.patch"; exit 1; }
rm -f "$d/ofonod.check"
if [ -n "$OUT" ]; then
  mkdir -p "$OUT"
  cp "$d"/*_"$v"_*.deb "$OUT/"
fi
[ "$BUILD_ONLY" = 1 ] || install_ours "$d"/*_"$v"_*.deb
