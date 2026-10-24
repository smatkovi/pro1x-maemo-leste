#!/bin/bash
# Give Harmattan's "user" (uid/gid 29999) Leste's uid/gid 1000.
#
# The Harmattan UI shares Leste's system D-Bus, PulseAudio and X server, so
# it should be the same user as the Leste session: D-Bus policies, the
# PulseAudio socket and device permissions then just work. Harmattan has no
# uid or gid 1000 of its own, so the change is exactly reversible ('undo').
#
#   H=/mnt/harmattan bash meego/37-harmattan-user.sh [apply|undo]
set -eu

H=${H:-/mnt/harmattan}
STATE=$H/var/lib/pro1x-graft
OLD=29999
NEW=1000
MODE=${1:-apply}

remap() {
	local from=$1 to=$2
	sed -i -E "s/^(user|developer):([^:]*):$from:/\1:\2:$to:/; s/^(user|developer):([^:]*):([0-9]+):$from:/\1:\2:\3:$to:/" "$H/etc/passwd"
	sed -i -E "s/^users:([^:]*):$from:/users:\1:$to:/" "$H/etc/group"
	find "$H" -xdev -uid $from -exec chown -h $to {} +
	find "$H" -xdev -gid $from -exec chgrp -h $to {} +
}

case "$MODE" in
apply)
	if [ -f "$STATE/user-uid" ]; then
		echo "already done"
		exit 0
	fi
	if awk -F: -v n=$NEW '$3 == n' "$H/etc/passwd" "$H/etc/group" | grep -q .; then
		echo "uid/gid $NEW already used in Harmattan"
		exit 1
	fi
	remap $OLD $NEW
	mkdir -p "$STATE"
	echo $NEW > "$STATE/user-uid"
	echo "Harmattan user is now uid/gid $NEW"
	;;
undo)
	[ -f "$STATE/user-uid" ] || { echo "nothing to undo"; exit 0; }
	remap $NEW $OLD
	rm -f "$STATE/user-uid"
	echo "Harmattan user is uid/gid $OLD again"
	;;
*)
	echo "usage: $0 [apply|undo]"
	exit 1
	;;
esac
