#!/bin/sh
# FreeBSD on the Pro1X, typed on its console when the desktop does not
# come up (the keymap has no "|", so one short line):
#
#   sh /dev/pro1x-host/freebsd/diag.sh [hildon|harmattan|xfce]
#
# What the desktop's start needs (the memory filesystems, root's home, X's
# files, the framebuffer, the random seed), then startx in the background:
# after 40 s the processes with what they wait for (ps -l, procstat -kk of
# xinit and Xorg), startx's output and Xorg's log; X is stopped again
# unless it answers. All of it into /dev/pro1xshot: Linux reads it after
# the next boot (ramconsole.py --shot, collect.sh's ramconsole-shot.bin).
# Then the USB gadget for ssh from the PC (rc.d/pro1x_usbnet), with miibus
# from userdata when the root image has none (root.zfs before 2026-11-01):
# phone.sh pin freebsd, SYS=freebsd. Every write to /dev/pro1xshot lands
# at once, so a hang or reset in the gadget's start keeps what came before.
H=/dev/pro1x-host/freebsd
D=${1:-hildon}
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
	echo "=== random"
	sysctl kern.random.random_sources kern.random.harvest.mask_symbolic
	sysctl -a | grep -i 'random.*seed\|fortuna.*init'
	echo "=== X's files"
	ls -l /usr/local/bin/X /usr/local/bin/Xorg /usr/local/bin/startx \
	    /usr/local/bin/xinit /usr/local/bin/xauth
	ls -l /dev/fb* /dev/ttyv* /dev/input /dev/dri 2>&1
	sysctl kern.vty kern.vt.fb 2>&1
	echo "=== startx $D in the background"
	(cd /root && HOME=/root startx "$D" -- -verbose 6 \
	    > /tmp/pro1x-startx.log 2>&1 &)
	sleep 40
	echo "=== after 40 s: processes"
	ps -axlww
	for n in startx xinit Xorg; do
		for p in $(pgrep -x $n); do
			echo "--- $n $p"
			procstat -kk $p
			procstat -f $p
		done
	done
	echo "=== startx's output"
	cat /tmp/pro1x-startx.log
	echo "=== Xorg.0.log"
	cat /var/log/Xorg.0.log
	if x_answers; then
		echo "=== X answers on :0: left running"
	else
		echo "=== X does not answer: stopped"
		pkill -x xinit
		sleep 2
		pkill -x Xorg
		sleep 3
		pkill -9 -x Xorg
		vidcontrol -s 1 < /dev/ttyv0 2> /dev/null
	fi
	echo "=== messages"
	tail -n 80 /var/log/messages
	echo "=== kernel, last lines"
	dmesg | tail -n 80
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
