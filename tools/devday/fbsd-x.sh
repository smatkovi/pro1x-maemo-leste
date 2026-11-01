#!/bin/sh
# FreeBSD on the Pro1X, as root on the console, until the root image has
# the fixed scfb (freebsd/scfb): the desktop with userdata's X modules.
#
#   sh /dev/pro1x-host/freebsd/x.sh [hildon|harmattan|xfce]
#
# devd first when it is not running (X takes its input devices from it,
# the power key needs it): its own words on why (8 s in the foreground)
# go into /dev/pro1xshot, and again with Xorg's log once X has ended, for
# Linux to read after the next boot (ramconsole.py --shot).
H=/dev/pro1x-host/freebsd
L=/tmp/pro1x-x.log
{
	echo "=== pro1x x.sh $(date)"
	if ! service devd status; then
		echo "--- devd -d, 8 s"
		timeout 8 devd -d 2>&1 | head -n 60
		service devd onestart
		sleep 1
		service devd status
	fi
} > $L 2>&1
cat $L > /dev/pro1xshot
cd /root || exit 1
# The desktop through the kernel environment, which ~/.xinitrc reads:
# startx gives ~/.xinitrc no arguments (with any, it runs xterm instead)
kenv pro1x.desktop="${1:-hildon}" > /dev/null
HOME=/root startx -- \
    -modulepath $H/xorg-modules,/usr/local/lib/xorg/modules
cat $L /var/log/Xorg.0.log > /dev/pro1xshot 2>&1
echo "X ended; its log is in /dev/pro1xshot"
