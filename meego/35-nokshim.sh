#!/bin/bash
# Build the Nokia EGL extension shim (meego/nokshim) for armhf in the Debian
# tree and install it into the grafted Harmattan rootfs:
#   /usr/lib/libEGL.so               the shim (Harmattan's EGL soname)
#   /usr/libexec/nok-image-broker    holds the shared images' dma-bufs
#
#   D=/host/debian-armhf H=/mnt/harmattan bash meego/35-nokshim.sh
set -eu

D=${D:-/host/debian-armhf}
H=${H:-/mnt/harmattan}
SRC=$(cd "$(dirname "$0")" && pwd)/nokshim
STATE=$H/var/lib/pro1x-graft

[ -f "$STATE/added" ] || { echo "run 30-graft-debian-runtime.sh first"; exit 1; }

for m in proc sys dev; do
	mountpoint -q "$D/$m" || mount --bind "/$m" "$D/$m"
done
chroot "$D" env DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
	gcc libc6-dev libegl-dev libgles-dev libdrm-dev libx11-dev > /dev/null

B=$D/tmp/nokshim-build
rm -rf "$B"
cp -r "$SRC" "$B"
chroot "$D" sh -c 'cd /tmp/nokshim-build &&
	gcc -O2 -Wall -fPIC -shared -fvisibility=hidden -I/usr/include/libdrm \
		-Wl,-soname,libEGL.so -o libEGL-nokshim.so egl-nokshim.c \
		-Wl,--no-as-needed -lEGL -lGLESv2 -lX11 &&
	gcc -O2 -Wall -o nok-image-broker nok-image-broker.c'

install -D -m 755 "$B/nok-image-broker" "$H/usr/libexec/nok-image-broker"
install -m 644 "$B/libEGL-nokshim.so" "$H/usr/lib/libEGL-nokshim.so"
rm -f "$H/usr/lib/libEGL.so"
ln -s libEGL-nokshim.so "$H/usr/lib/libEGL.so"
for p in /usr/libexec/nok-image-broker /usr/lib/libEGL-nokshim.so; do
	grep -qxF "$p" "$STATE/added" || echo "$p" >> "$STATE/added"
done
rm -rf "$B"
echo "nokshim installed"
