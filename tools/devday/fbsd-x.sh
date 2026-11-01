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
	fi
	# The root image of 2026-11-01 has a pro1x_typec.conf devd cannot
	# parse (an escaped quote, fixed in the repository): a copy of the
	# configuration in /tmp with that one mended
	if ! service devd status; then
		d=/tmp/pro1x-devd.d
		mkdir -p $d
		cp /usr/local/etc/devd/*.conf $d/
		sed -i '' 's/\\"//g' $d/pro1x_typec.conf
		sed "s|\"/usr/local/etc/devd\"|\"$d\"|" /etc/devd.conf \
		    > /tmp/pro1x-devd.conf
		echo "--- devd -f /tmp/pro1x-devd.conf"
		devd -f /tmp/pro1x-devd.conf
		sleep 1
	fi
	service devd status || pgrep -lx devd
} > $L 2>&1
cat $L > /dev/pro1xshot
cd /root || exit 1
# The desktop through the kernel environment, which ~/.xinitrc reads:
# startx gives ~/.xinitrc no arguments (with any, it runs xterm instead)
kenv pro1x.desktop="${1:-hildon}" > /dev/null
# userdata's X configuration first when there is one (freebsd/xorg.conf.d,
# a touchscreen transformation on trial)
C=
[ -d $H/xorg.conf.d ] && C="-configdir $H/xorg.conf.d"
# shellcheck disable=SC2086
HOME=/root startx -- \
    -modulepath $H/xorg-modules,/usr/local/lib/xorg/modules $C
cat $L /var/log/Xorg.0.log > /dev/pro1xshot 2>&1
echo "X ended; its log is in /dev/pro1xshot"
