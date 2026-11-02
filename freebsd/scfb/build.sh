#!/bin/bash
# X's framebuffer driver for the phone, xf86-video-scfb 0.0.7 (the port's
# source, its SHA-256 from x11-drivers/xf86-video-scfb/distinfo) with
# 0001-no-xf86DisableRandR.patch: FreeBSD's package calls a function the
# release's xorg-server no longer has as soon as the screen is rotated,
# which the Pro1X's is (rootfs/usr/local/etc/X11/xorg.conf.d/10-pro1x.conf),
# and Xorg dies (docs/device-day-2026-10-31.md block 12.6); and
# 0002-rotate-cw-in-blocks.patch: that rotation through scfb_rotcw.h
# instead of the server's column walk (test/rotcw-test.c checks it against
# the mapping and times both). Built in a
# FreeBSD machine against its xorg-server package (vm/vm.sh), into
# $WORK/out/scfb/scfb_drv.so; rootfs.sh puts it over the package's.
#
#   WORK=/path/to/freebsd-build VM="ssh ... root@host" bash freebsd/scfb/build.sh
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
WORK=${WORK:?set WORK=<build directory>}
VM=${VM:?set VM=<ssh command for a FreeBSD machine, as root>}
VER=0.0.7
SUM=0b9774e73c6993be84def21d27addcca5c3b88f7172d39008bc6ce5dec6768f0
TAR=$WORK/dl/rayddteam-xf86-video-scfb-${VER}_GH0.tar.gz
OUT=$WORK/out/scfb
log() { echo ">> $*"; }

mkdir -p "$WORK/dl" "$OUT"
if [ ! -f "$TAR" ]; then
	log "source"
	curl -sSfL -o "$TAR.new" \
	    "https://codeload.github.com/rayddteam/xf86-video-scfb/tar.gz/$VER"
	mv "$TAR.new" "$TAR"
fi
echo "$SUM  $TAR" | sha256sum -c --quiet -

log "build in the FreeBSD machine"
$VM 'rm -rf /root/scfb && mkdir -p /root/scfb'
$VM 'cat > /root/scfb/src.tar.gz' < "$TAR"
for p in "$REPO"/freebsd/scfb/0*.patch; do
	$VM "cat > /root/scfb/${p##*/}" < "$p"
done
$VM 'cat > /root/scfb/scfb_rotcw.h' < "$REPO/freebsd/scfb/scfb_rotcw.h"
$VM 'mkdir -p /root/scfb/test && cat > /root/scfb/test/rotcw-test.c' < "$REPO/freebsd/scfb/test/rotcw-test.c"
$VM sh -s <<'REMOTE'
set -e
cd /root/scfb
# the headers and pkg-config files to build against
pkg install -qy xorg-server xorgproto pkgconf > /dev/null
tar xzf src.tar.gz
cd xf86-video-scfb-*
for p in ../0*.patch; do patch -s -p1 < "$p"; done
cp ../scfb_rotcw.h src/
# the rotation against its mapping, on this machine's NEON
cc -O2 -o ../test/rotcw-test ../test/rotcw-test.c
../test/rotcw-test 3
./configure -q --prefix=/usr/local > ../configure.log 2>&1 || { tail -20 ../configure.log; exit 1; }
gmake -j4 > ../make.log 2>&1 || { tail -30 ../make.log; exit 1; }
if nm -D src/.libs/scfb_drv.so | grep -q xf86DisableRandR; then
	echo "still calls xf86DisableRandR"
	exit 1
fi
pkg query '%n-%v' xorg-server
REMOTE
$VM 'cat /root/scfb/xf86-video-scfb-*/src/.libs/scfb_drv.so' > "$OUT/scfb_drv.so.new"
mv "$OUT/scfb_drv.so.new" "$OUT/scfb_drv.so"
ls -la "$OUT/scfb_drv.so"
log "done: rootfs.sh puts it into the image"
