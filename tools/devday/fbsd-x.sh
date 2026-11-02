#!/bin/sh
# FreeBSD on the Pro1X, as root on the console, until the root image has
# the fixed scfb (freebsd/scfb): the desktop with userdata's X modules.
#
#   sh /dev/pro1x-host/freebsd/x.sh [hildon|harmattan|xfce] [half]
#
# half: X's screen at half the size, 1080 x 540, each pixel 2 x 2 on the
# panel (scfb's Option "Scale" "2", freebsd/scfb), Hildon unscaled on it
# (pro1x-hidpi with SCALE=1 from /tmp, not userdata's pro1x-display.conf,
# which Leste shares): a quarter of the pixels to draw and composite.
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
			cat $L /tmp/pro1x-touchcal.txt /tmp/pro1x-apptest.txt \
			    /tmp/pro1x-wifi.log 2> /dev/null
			echo "=== $(date)"
			sysctl dev.cpu.0.freq dev.cpu.0.freq_levels hw.ncpu kern.smp.disabled 2>&1
			service powerd status 2>&1
			uptime
			ps -axo pid,pcpu,rss,command | sort -rn -k2 | head -n 12
			echo "=== busiest threads (top -H, every 6 s)"
			tail -n 900 /tmp/pro1x-perf.log
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
			grep -v -e 'did not exit properly' -e 'background image' \
			    -e bookmark -e 'operator' /tmp/pro1x-startx.log | tail -n 400
			echo "=== Xorg tail"
			tail -n 40 /var/log/Xorg.0.log
		} > /dev/pro1xshot 2>&1
	done
) &
logger_pid=$!
# Which threads take the CPU while one scrolls (Xorg: drawing and the
# rotation into the framebuffer; hildon-desktop's llvmpipe threads: the
# compositing): top's second display covers the 2 s since its first
(
	while sleep 4; do
		{
			echo "--- $(date +%T)"
			top -b -H -d 2 -s 2 -o cpu 12 |
			    awk '/^last pid/ { n++ } n == 2 && /^ *[0-9]/'
		} >> /tmp/pro1x-perf.log 2>&1
		if [ "$(wc -l < /tmp/pro1x-perf.log)" -gt 3000 ]; then
			tail -n 2000 /tmp/pro1x-perf.log > /tmp/pro1x-perf.log.new
			mv /tmp/pro1x-perf.log.new /tmp/pro1x-perf.log
		fi
	done
) &
perf_pid=$!
# The sampler's file stays for the boot: a second session (x.sh ... half
# after x.sh) is in the same shot, behind this mark
echo "=== x.sh ${1:-hildon} ${2:-full} started $(date +%T)" >> /tmp/pro1x-perf.log
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
	# X's one thread on the fast cluster (the CPUs with the higher top
	# frequency, 4-7 on the SM6115): FreeBSD's scheduler does not know
	# big from little and puts it on an A53 as often as on an A73
	# (pro1x.xcpus=off leaves it to the scheduler)
	f0=$(sysctl -n dev.cpu.0.freq_levels 2> /dev/null | cut -d/ -f1)
	f4=$(sysctl -n dev.cpu.4.freq_levels 2> /dev/null | cut -d/ -f1)
	x=$(pgrep -x Xorg | head -n 1)
	if [ "$(kenv -q pro1x.xcpus)" != off ] && [ -n "$x" ] &&
	    [ "${f4:-0}" -gt "${f0:-0}" ]; then
		cpuset -l 4-7 -p "$x" &&
		    echo "Xorg ($x) on CPUs 4-7 ($f4 MHz, CPU 0: $f0 MHz)" >> $L
	fi
	sleep 60
	xwd -root -silent | gzip -6 > /tmp/pro1x-screen.xwd.gz
) &
# The Wi-Fi (userdata's wifi.sh, tools/devday/fbsd-wifi.sh) beside X when
# the cmdline asks for it
if [ "$(kenv -q pro1x.wifi)" = on ] && [ -f $H/wifi.sh ]; then
	sh $H/wifi.sh > /dev/null 2>&1 &
fi
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
# half: the same configuration with the framebuffer driver's Scale 2, and
# Hildon's theme and fonts at the factor 1; otherwise the factor of
# userdata's pro1x-display.conf (back after a half session in this boot)
if [ "$2" = half ]; then
	d=/usr/local/etc/X11/xorg.conf.d
	[ -n "$C" ] && d=${C#-configdir }
	rm -rf /tmp/pro1x-xorg-half.conf.d
	mkdir -p /tmp/pro1x-xorg-half.conf.d
	for f in "$d"/*.conf; do
		sed '/Option[[:space:]]*"Rotate"/a\
	Option		"Scale" "2"
' "$f" > /tmp/pro1x-xorg-half.conf.d/"${f##*/}"
	done
	C="-configdir /tmp/pro1x-xorg-half.conf.d"
	echo "SCALE=1" > /tmp/pro1x-display-half.conf
	PRO1X_DISPLAY_CONF=/tmp/pro1x-display-half.conf
	export PRO1X_DISPLAY_CONF
fi
/usr/local/bin/pro1x-hidpi theme >> $L 2>&1
# shellcheck disable=SC2086
HOME=/root startx -- \
    -modulepath $H/xorg-modules,/usr/local/lib/xorg/modules $C \
    > /tmp/pro1x-startx.log 2>&1
kill $logger_pid $perf_pid 2> /dev/null
{
	cat $L /tmp/pro1x-touchcal.txt /tmp/pro1x-apptest.txt /tmp/pro1x-wifi.log
	echo "=== pointer (ptrlog)"
	tail -n 300 /tmp/pro1x-ptr.log
	echo "=== busiest threads (top -H, every 6 s)"
	tail -n 900 /tmp/pro1x-perf.log
	echo "=== the session's output"
	cat /tmp/pro1x-startx.log
	echo "=== Xorg"
	cat /var/log/Xorg.0.log
} > /dev/pro1xshot 2>&1
echo "X ended; its log is in /dev/pro1xshot"
