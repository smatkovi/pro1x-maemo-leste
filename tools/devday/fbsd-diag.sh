#!/bin/sh
# FreeBSD on the Pro1X, typed on its console when the desktop does not
# come up (the keymap has no "|", so one short line):
#
#   sh /dev/pro1x-host/freebsd/diag.sh
#
# What the desktop's start needs (the memory filesystems, root's home, X's
# files, the framebuffer), one startx with its output, Xorg's log and the
# processes, into /dev/pro1xshot: Linux reads it after the next boot
# (ramconsole.py --shot, collect.sh's ramconsole-shot.bin). Then the USB
# gadget for ssh from the PC (rc.d/pro1x_usbnet), with miibus from
# userdata when the root image has none (root.zfs before 2026-11-01):
# phone.sh pin freebsd, SYS=freebsd. Every write to /dev/pro1xshot lands
# at once, so a hang or reset in the gadget's start keeps what came before.
H=/dev/pro1x-host/freebsd
D=${1:-hildon}
{
	echo "=== pro1x diag $(date)"
	uname -a
	echo "=== mounts"
	mount
	df -h
	echo "=== /var/log /tmp /root"
	ls -la /var/log /tmp /tmp/.X11-unix /root
	echo "=== rc.conf, kenv"
	kenv | grep pro1x
	service -e
	echo "=== X's files"
	ls -l /usr/local/bin/X /usr/local/bin/Xorg /usr/local/bin/startx \
	    /usr/local/bin/xinit /usr/local/bin/xauth
	ls -l /dev/fb* /dev/ttyv* /dev/input /dev/dri 2>&1
	sysctl kern.vty kern.vt.fb 2>&1
	echo "=== startx $D (60 s at most)"
	(cd /root && HOME=/root timeout -k 5 60 startx "$D" -- -verbose 6) 2>&1
	echo "startx exit $?"
	echo "=== Xorg.0.log"
	cat /var/log/Xorg.0.log
	echo "=== processes"
	ps axww
	echo "=== messages"
	tail -n 80 /var/log/messages
	echo "=== kernel, last lines"
	dmesg | tail -n 60
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
	echo "=== diag done $(date)"
} > /dev/pro1xshot 2>&1
echo "diag kept in /dev/pro1xshot; ue0: $(ifconfig ue0 2> /dev/null | grep inet)"
