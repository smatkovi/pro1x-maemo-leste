#!/bin/bash
# Small adaptations of Harmattan binaries to the Pro1X hardware. Originals
# are kept in /usr/lib/harmattan-replaced ('30-graft-debian-runtime.sh undo'
# restores them).
#
#   H=/mnt/harmattan bash meego/38-hardware-adaptation.sh
set -eu

H=${H:-/mnt/harmattan}
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

# mcompositor's edge swipes (to the home screen and the app switcher) only
# listen to a touchscreen whose name contains "Atmel", the N9's.
patch_string /usr/lib/mcompositor/libnokiaswipeextension.so Atmel Goodi
