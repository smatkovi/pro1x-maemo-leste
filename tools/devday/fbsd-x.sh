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
	# devd starts a moused for every input device, the touchscreen's too,
	# which then moves the pointer again through sysmouse, unrotated (the
	# root image has moused_nondefault_enable="NO" from 2026-11-02 on)
	if pgrep -x moused > /dev/null; then
		echo "--- moused: $(pgrep -lx moused | tr '\n' ' ')stopped"
		pkill -x moused
	fi
} > $L 2>&1
cat $L > /dev/pro1xshot
# While X runs, every 20 s into /dev/pro1xshot: this log, the touchscreen
# calibration, the CPU's clock and powerd, the load, X's input lines and
# the end of its log (the shot survives the power key's power-off)
(
	while sleep 20; do
		{
			cat $L /tmp/pro1x-touchcal.txt 2> /dev/null
			echo "=== $(date)"
			sysctl dev.cpu.0.freq dev.cpu.0.freq_levels hw.ncpu kern.smp.disabled 2>&1
			service powerd status 2>&1
			uptime
			ps -axo pid,pcpu,rss,command | sort -rn -k2 | head -n 12
			echo "=== Xorg input"
			grep -E "InputClass|libinput|Goodix|touch|TransformationMatrix|Using input driver" \
			    /var/log/Xorg.0.log
			echo "=== pointer (ptrlog)"
			tail -n 300 /tmp/pro1x-ptr.log
			if [ -s /tmp/pro1x-screen.xwd.gz ]; then
				echo "=== screen (xwd, gzip, base64)"
				b64encode /tmp/pro1x-screen.xwd.gz pro1x-screen.xwd.gz
			fi
			echo "=== the session's output"
			tail -n 60 /tmp/pro1x-startx.log
			echo "=== Xorg tail"
			tail -n 40 /var/log/Xorg.0.log
		} > /dev/pro1xshot 2>&1
	done
) &
logger_pid=$!
# Where X has the pointer (userdata's ptrlog, tools/devday/fbsd-ptrlog) and
# X's screen once, a minute in: which way the touches land in X's frame
(
	export DISPLAY=:0 XAUTHORITY=/root/.Xauthority
	i=0
	until xprop -root > /dev/null 2>&1 || [ $i -ge 120 ]; do
		sleep 1
		i=$((i + 1))
	done
	[ -x $H/ptrlog ] && $H/ptrlog > /tmp/pro1x-ptr.log 2>&1 &
	sleep 60
	xwd -root -silent | gzip -6 > /tmp/pro1x-screen.xwd.gz
) &
cd /root || exit 1
# The desktop through the kernel environment, which ~/.xinitrc reads:
# startx gives ~/.xinitrc no arguments (with any, it runs xterm instead)
kenv pro1x.desktop="${1:-hildon}" > /dev/null
# userdata's X configuration first when there is one (freebsd/xorg.conf.d,
# a touchscreen transformation on trial)
C=
[ -d $H/xorg.conf.d ] && C="-configdir $H/xorg.conf.d"
# touchcal.sh's, from this boot's taps, before that
[ -d /tmp/pro1x-xorg.conf.d ] && C="-configdir /tmp/pro1x-xorg.conf.d"
# shellcheck disable=SC2086
HOME=/root startx -- \
    -modulepath $H/xorg-modules,/usr/local/lib/xorg/modules $C \
    > /tmp/pro1x-startx.log 2>&1
kill $logger_pid 2> /dev/null
{
	cat $L /tmp/pro1x-touchcal.txt
	echo "=== pointer (ptrlog)"
	tail -n 300 /tmp/pro1x-ptr.log
	echo "=== the session's output"
	cat /tmp/pro1x-startx.log
	echo "=== Xorg"
	cat /var/log/Xorg.0.log
} > /dev/pro1xshot 2>&1
echo "X ended; its log is in /dev/pro1xshot"
