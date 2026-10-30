#!/bin/bash
# Builds the FreeBSD camera app (pro1x-camera.c on pro1x-cam's code)
# against the Hildon sysroot of freebsd/hildon/build.sh, into a package
# root: usr/local/bin/pro1x-camera and its Hildon .desktop file.
#
#   WORK=/path/to/freebsd-build bash freebsd/camera/app/build-app.sh [PKGROOT]
#
# PKGROOT defaults to $WORK/cam-test/app-root (the binary is also copied
# to $WORK/cam-test/camera/ for a run by hand). Only reads the sysroot;
# hildon/build.sh's part pro1x-camera calls this with its own PKGROOT.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../../.." && pwd)}
WORK=${WORK:?set WORK=<build directory>}
H=$WORK/hildon
SYSROOT=$H/sysroot
PR=${1:-$WORK/cam-test/app-root}
C=$REPO/freebsd/camera
U=$C/tools/pro1x-cam
[ -f "$SYSROOT/usr/local/lib/pkgconfig/hildon-1.pc" ] ||
	{ echo "no Hildon sysroot in $SYSROOT (freebsd/hildon/build.sh)"; exit 1; }
export PATH=$H/hostbin:$PATH
export PKG_CONFIG_LIBDIR=$SYSROOT/usr/local/lib/pkgconfig:$SYSROOT/usr/local/libdata/pkgconfig:$SYSROOT/usr/libdata/pkgconfig:$SYSROOT/usr/local/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$SYSROOT PKG_CONFIG_PATH=
CC=(clang --target=aarch64-unknown-freebsd15.0 --sysroot="$SYSROOT")
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT
# shellcheck disable=SC2046
"${CC[@]}" -isystem "$SYSROOT/usr/local/include" \
    -idirafter "$REPO/freebsd/hildon/compat/include" -O2 -std=gnu17 -Wall \
    -Wno-unused-parameter -Wno-deprecated-declarations -fuse-ld=lld \
    -L"$SYSROOT/usr/local/lib" -Wl,-rpath-link,"$SYSROOT/usr/local/lib" \
    -Wl,-rpath-link,"$SYSROOT/usr/lib" -Wl,-rpath-link,"$SYSROOT/lib" \
    -I"$C/sys/arm64/qualcomm/camss" -I"$U" -o "$B/pro1x-camera" \
    "$C/app/pro1x-camera.c" "$U"/capture.c "$U"/image.c "$U"/jpeg.c \
    "$U"/iic.c "$U"/power.c "$U"/sensor.c "$U"/shot.c "$U"/util.c "$U"/calc.c \
    $(pkg-config --cflags --libs hildon-1 gtk+-2.0 gthread-2.0) -lz -lm
install -D -m 755 "$B/pro1x-camera" "$PR/usr/local/bin/pro1x-camera"
install -D -m 644 "$C/app/pro1x-camera.desktop" \
    "$PR/usr/local/share/applications/hildon/pro1x-camera.desktop"
if [ -d "$WORK/cam-test/camera" ]; then
	cp "$B/pro1x-camera" "$WORK/cam-test/camera/"
fi
ls -la "$PR/usr/local/bin/pro1x-camera"
