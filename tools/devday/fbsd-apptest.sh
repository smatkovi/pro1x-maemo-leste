#!/bin/sh
# FreeBSD on the Pro1X, in the desktop's terminal (X running through
# tools/devday/fbsd-x.sh): each program for T seconds (20) with its
# output in /tmp/pro1x-apptest.txt, which fbsd-x.sh keeps in
# /dev/pro1xshot every 20 s for Linux to read after the next boot.
#
#   sh /dev/pro1x-host/freebsd/apptest.sh [program ...]
#
# Without programs: those that did not start from the menu on 2026-11-02.
T=${T:-20}
O=/tmp/pro1x-apptest.txt
[ $# -gt 0 ] || set -- gpodder-leste pure-maps-mobile meego-maps pro1x-camera
{
	echo "=== apptest $(date)"
	env | grep -E '^(DISPLAY|DBUS_SESSION_BUS_ADDRESS|HOME|LANG|LC_ALL|XDG_[A-Z_]*)='
} > $O
for p in "$@"; do
	echo "running $p for $T s"
	{
		echo "=== $p ($(date +%T)): $(command -v $p || echo 'not in PATH')"
		timeout -k 3 $T $p 2>&1
		echo "--- $p: exit $?"
	} >> $O 2>&1
done
echo "done: $O, in /dev/pro1xshot within 20 s; then reboot"
