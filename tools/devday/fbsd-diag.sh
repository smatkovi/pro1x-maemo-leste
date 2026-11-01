#!/bin/sh
# FreeBSD on the Pro1X, typed on its console when the desktop does not
# come up (the keymap has no "|", so one short line):
#
#   sh /dev/pro1x-host/freebsd/diag.sh [hildon|harmattan|xfce] [usb]
#
# What the desktop's start needs (the memory filesystems, root's home, X's
# files, the framebuffer, devd, the random seed), then startx in the
# background: after 45 s the processes with what they wait for (ps -l,
# procstat -kk), X's screen as xwd (gzip, base64), startx's output and
# Xorg's log. Then X is stopped again and the console comes back (should
# it stay dark: "reboot" and Enter blind). All of it into /dev/pro1xshot:
# Linux reads it after the next boot (ramconsole.py --shot, collect.sh's
# ramconsole-shot.bin; fbsd-diag-xwd.py turns the screen into a PNG).
# With "usb" also the USB gadget for ssh from the PC (rc.d/pro1x_usbnet),
# with miibus from userdata when the root image has none. Every write to
# /dev/pro1xshot lands at once, so a hang or reset keeps what came before.
H=/dev/pro1x-host/freebsd
D=${1:-hildon}
U=${2:-}
x_answers() {
	DISPLAY=:0 XAUTHORITY=/root/.Xauthority timeout 10 \
	    xwd -root -silent > /dev/null 2>&1
}
{
	echo "=== pro1x diag $(date)"
	uname -a
	echo "=== mounts"
	mount
	df -h
	echo "=== /var/log /tmp /root"
	ls -la /var/log /tmp /tmp/.X11-unix /root
	echo "=== kenv, services"
	kenv | grep pro1x
	service -e
	echo "=== devd"
	service devd status
	ls -l /var/run/devd.pipe /var/run/devd.seqpacket.pipe
	cat /var/log/devd.log
	grep -i devd /var/log/messages /var/log/daemon.log
	echo "=== random"
	sysctl kern.random.random_sources kern.random.block_seeded_status
	echo "=== X's files"
	ls -l /usr/local/bin/X /usr/local/bin/Xorg /usr/local/libexec/Xorg \
	    /usr/local/libexec/Xorg.wrap /usr/local/bin/startx \
	    /usr/local/bin/xinit /usr/local/bin/xauth
	ls -l /dev/fb* /dev/ttyv0 /dev/input /dev/dri 2>&1
	dmesg | grep -i 'fb\|vt\|simplefb\|dpu\|backlight'
	echo "=== startx $D in the background"
	(cd /root && HOME=/root startx "$D" -- -verbose 6 \
	    > /tmp/pro1x-startx.log 2>&1 &)
	sleep 45
	echo "=== after 45 s: processes"
	ps -axlww
	for n in startx xinit Xorg hildon-desktop hildon-home hildon-status-menu \
	    maemo-launcher pro1x-standby xfwm4 xfce4-session matchbox-window-manager; do
		for p in $(pgrep -x $n); do
			echo "--- $n $p"
			procstat -kk $p
		done
	done
	echo "=== X answers: $(x_answers && echo yes || echo no)"
	DISPLAY=:0 XAUTHORITY=/root/.Xauthority timeout 10 xprop -root
	rm -f /tmp/pro1x-screen.xwd /tmp/pro1x-screen.xwd.gz
	if DISPLAY=:0 XAUTHORITY=/root/.Xauthority timeout 20 \
	    xwd -root -silent > /tmp/pro1x-screen.xwd; then
		# what survives the shot's flipped bits: is anything drawn?
		echo "screen: $(tr -d '\000' < /tmp/pro1x-screen.xwd | wc -c)" \
		    "bytes not zero of $(wc -c < /tmp/pro1x-screen.xwd)"
		gzip -9 /tmp/pro1x-screen.xwd
		echo "screen gzip: $(wc -c < /tmp/pro1x-screen.xwd.gz) bytes"
		echo "=== screen (xwd, gzip, base64)"
		b64encode /tmp/pro1x-screen.xwd.gz pro1x-screen.xwd.gz
	fi
	echo "=== startx's output"
	cat /tmp/pro1x-startx.log
	echo "=== Xorg.0.log"
	cat /var/log/Xorg.0.log
	echo "=== stopping X"
	pkill -x xinit
	sleep 3
	pkill -x Xorg
	sleep 3
	pkill -9 -x Xorg
	sleep 1
	vidcontrol -s 1 < /dev/ttyv0
	echo "=== messages"
	tail -n 60 /var/log/messages
	echo "=== kernel, last lines"
	dmesg | tail -n 60
	if [ "$U" = usb ]; then
		echo "=== USB gadget"
		if ! kldstat -q -m miibus; then
			if [ -f /boot/modules/miibus.ko ] || [ -f /boot/kernel/miibus.ko ]; then
				kldload miibus
			else
				kldload $H/modules/miibus.ko
			fi
		fi
		service pro1x_usbnet onestart
		ifconfig ue0
	fi
	echo "=== diag done $(date)"
} > /dev/pro1xshot 2>&1
echo "diag kept in /dev/pro1xshot: now reboot"
