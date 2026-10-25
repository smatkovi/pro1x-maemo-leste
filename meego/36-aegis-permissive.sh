#!/bin/bash
# Permissive Aegis: replace Harmattan's libcreds.so.2 with a build of its
# own (LGPL) source that lets every credential check pass, see
# meego/aegis/creds-permissive.c. Without it applauncherd refuses to start
# applications ("Security credential check failed"). aegis-exec becomes a
# plain user switch (meego/aegis/aegis-exec.c).
#
#   D=/host/debian-armhf H=/mnt/harmattan bash meego/36-aegis-permissive.sh
set -eu

D=${D:-/host/debian-armhf}
H=${H:-/mnt/harmattan}
SRC=$(cd "$(dirname "$0")" && pwd)/aegis
STATE=$H/var/lib/pro1x-graft
SAVE=$H/usr/lib/harmattan-replaced

URL=http://wunderwungiel.pl/MeeGo/harmattan-dev.nokia.com/pool/harmattan/free/libc/libcreds2/libcreds2_1.2.21+0m6.tar.gz
SHA256=aedbf6f5500ef33b55eecabce93d76fa08c00efda8047c0541ba27c681929b57

[ -f "$STATE/added" ] || { echo "run 30-graft-debian-runtime.sh first"; exit 1; }

B=$D/tmp/creds-build
rm -rf "$B"
mkdir -p "$B"
curl -sfL -o "$B/src.tar.gz" "$URL"
echo "$SHA256  $B/src.tar.gz" | sha256sum -c --quiet
tar -C "$B" --strip-components=1 -xzf "$B/src.tar.gz"
cp "$SRC/creds-permissive.c" "$B/"
# The /proc-only branch (no Aegis kernel headers) misses an argument.
sed -i 's/return fallback_get(pid, list, list_length);/return fallback_get(pid, list, list_length, GETTASK);/' "$B/credsctl.c"

for m in proc sys dev; do
	mountpoint -q "$D/$m" || mount --bind "/$m" "$D/$m"
done
chroot "$D" env DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
	gcc libc6-dev linux-libc-dev perl > /dev/null
cp "$SRC/aegis-exec.c" "$B/"
chroot "$D" sh -c 'cd /tmp/creds-build &&
	perl capnames.pl < /usr/include/linux/capability.h > cap_names.h &&
	gcc -O2 -fPIC -shared -I. -w -Wl,-soname,libcreds.so.2 \
		-o libcreds-permissive.so.2 creds-permissive.c credsctl.c &&
	gcc -O2 -Wall -o aegis-exec aegis-exec.c'

install -m 644 "$B/libcreds-permissive.so.2" "$H/usr/lib/libcreds-permissive.so.2"
if [ -L "$H/usr/lib/libcreds.so.2" ] && [ "$(readlink "$H/usr/lib/libcreds.so.2")" != libcreds-permissive.so.2 ]; then
	mkdir -p "$SAVE/usr/lib"
	mv "$H/usr/lib/libcreds.so.2" "$SAVE/usr/lib/libcreds.so.2"
	echo /usr/lib/libcreds.so.2 >> "$STATE/moved"
	# libcreds.so.2.0.0 keeps its soname; ldconfig must not link it again.
	mv "$H/usr/lib/libcreds.so.2.0.0" "$SAVE/usr/lib/libcreds.so.2.0.0"
	echo /usr/lib/libcreds.so.2.0.0 >> "$STATE/moved"
fi
ln -sf libcreds-permissive.so.2 "$H/usr/lib/libcreds.so.2"
for p in /usr/lib/libcreds-permissive.so.2 /usr/lib/libcreds.so.2; do
	grep -qxF "$p" "$STATE/added" || echo "$p" >> "$STATE/added"
done
chroot "$H" /sbin/ldconfig

# aegis-exec, which the session jobs start their daemons with, crashes
# without the Aegis module; aegis/aegis-exec.c only switches the user.
if ! grep -qxF /usr/bin/aegis-exec "$STATE/moved"; then
	mkdir -p "$SAVE/usr/bin"
	mv "$H/usr/bin/aegis-exec" "$SAVE/usr/bin/aegis-exec"
	echo /usr/bin/aegis-exec >> "$STATE/moved"
fi
install -m 755 "$B/aegis-exec" "$H/usr/bin/aegis-exec"
grep -qxF /usr/bin/aegis-exec "$STATE/added" || echo /usr/bin/aegis-exec >> "$STATE/added"
rm -rf "$B"
echo "permissive libcreds and aegis-exec installed"
