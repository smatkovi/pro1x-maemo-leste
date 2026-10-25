#!/bin/bash
# Small adaptations of Harmattan binaries to the Pro1X hardware. Originals
# are kept in /usr/lib/harmattan-replaced ('30-graft-debian-runtime.sh undo'
# restores them); helper libraries are built in the Debian armhf tree.
#
#   D=/host/debian-armhf H=/mnt/harmattan bash meego/38-hardware-adaptation.sh
set -eu

D=${D:-/host/debian-armhf}
H=${H:-/mnt/harmattan}
SRC=$(cd "$(dirname "$0")" && pwd)
STATE=$H/var/lib/pro1x-graft
SAVE=$H/usr/lib/harmattan-replaced

[ -f "$STATE/added" ] || { echo "run 30-graft-debian-runtime.sh first"; exit 1; }
touch "$STATE/patched"

# Replace one string by another of the same length in a binary.
patch_string() {
	local rel=$1 from=$2 to=$3
	[ ${#from} -eq ${#to} ] || { echo "length differs: $from/$to"; exit 1; }
	if ! grep -qxF "$rel" "$STATE/patched"; then
		mkdir -p "$(dirname "$SAVE$rel")"
		cp -a "$H$rel" "$SAVE$rel"
		echo "$rel" >> "$STATE/patched"
	fi
	if grep -qaF "$from" "$H$rel"; then
		sed -i "s/$from/$to/g" "$H$rel"
		echo "$rel: $from -> $to"
	fi
}

# Build <dir>/<name>.c into /usr/lib/pro1x/lib<name>.so.
build_lib() {
	local dir=$1 name=$2; shift 2
	local B=$D/tmp/pro1x-build
	rm -rf "$B"
	cp -r "$SRC/$dir" "$B"
	chroot "$D" sh -c "cd /tmp/pro1x-build &&
		gcc -O2 -Wall -fPIC -shared -fvisibility=hidden -o lib$name.so $name.c \
			-Wl,--no-as-needed $*"
	install -D -m 644 "$B/lib$name.so" "$H/usr/lib/pro1x/lib$name.so"
	grep -qxF "/usr/lib/pro1x/lib$name.so" "$STATE/added" ||
		echo "/usr/lib/pro1x/lib$name.so" >> "$STATE/added"
	rm -rf "$B"
	echo "/usr/lib/pro1x/lib$name.so built"
}

# Build <dir>/<name>.c in place of Harmattan's library <rel> (soname
# <soname>); the original is kept like patch_string's.
replace_lib() {
	local dir=$1 name=$2 rel=$3 soname=$4; shift 4
	local B=$D/tmp/pro1x-build
	rm -rf "$B"
	cp -r "$SRC/$dir" "$B"
	chroot "$D" sh -c "cd /tmp/pro1x-build &&
		gcc -O2 -Wall -fPIC -shared -fvisibility=hidden -o out.so $name.c \
			-Wl,-soname,$soname $*"
	if ! grep -qxF "$rel" "$STATE/patched"; then
		mkdir -p "$(dirname "$SAVE$rel")"
		cp -a "$H$rel" "$SAVE$rel"
		echo "$rel" >> "$STATE/patched"
	fi
	install -m 644 "$B/out.so" "$H$rel"
	rm -rf "$B"
	echo "$rel replaced"
}

for m in proc sys dev; do
	mountpoint -q "$D/$m" || mount --bind "/$m" "$D/$m"
done
chroot "$D" env DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
	gcc libc6-dev libx11-dev libxi-dev libxtst-dev > /dev/null

# mcompositor's edge swipes (to the home screen and the app switcher, and
# away from the lock screen) open a touchscreen whose name contains "Atmel",
# the N9's...
patch_string /usr/lib/mcompositor/libnokiaswipeextension.so Atmel Goodi
# ...and read the finger in the N9's touch coordinates; the preloaded
# mcompositor-touch gives them screen coordinates (40-harmattan-session.sh).
build_lib touch mcompositor-touch -lXtst -lX11

# Qt's touch events (MeeGo Touch pans with them) from the N9's X input,
# which harmattan-touch emulates for every process; harmattan-compat for
# the remaining N9 assumptions.
build_lib touch harmattan-touch
build_lib compat harmattan-compat
# The battery, from the kernel's power supply class: the N9's BME server
# needs the N9's charger hardware (bme/).
replace_lib bme bmeipc /usr/lib/libbmeipc.so.0 libbmeipc.so.0
P=/etc/ld.so.preload
if [ -e "$H$P" ] && ! grep -qxF "$P" "$STATE/added"; then
	if ! grep -qxF "$P" "$STATE/patched"; then
		mkdir -p "$(dirname "$SAVE$P")"
		cp -a "$H$P" "$SAVE$P"
		echo "$P" >> "$STATE/patched"
	fi
else
	grep -qxF "$P" "$STATE/added" || echo "$P" >> "$STATE/added"
fi
for l in harmattan-touch harmattan-compat; do
	grep -qxF /usr/lib/pro1x/lib$l.so "$H$P" 2>/dev/null ||
		echo /usr/lib/pro1x/lib$l.so >> "$H$P"
done
