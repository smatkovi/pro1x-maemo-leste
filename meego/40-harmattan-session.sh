#!/bin/bash
# Run the Harmattan (MeeGo 1.2) user interface on the Pro1X display.
#
# Maemo Leste stays the hardware layer: its kernel, Xorg (modesetting +
# glamor), modem, WLAN and audio services keep running. The Harmattan
# rootfs runs on top in a chroot and draws on Leste's X server; its
# compositor (mcompositor) takes the place of hildon-desktop.
#
#   bash meego/40-harmattan-session.sh mount|start|stop|umount|shell
set -eu

IMG=${IMG:-/host/harmattan.img}
H=${H:-/mnt/harmattan}
HUID=29999                      # "user" on Harmattan
# Harmattan works in 854x480; the 2160x1080 panel shows a 960x480 screen.
SCALE=${SCALE:-0.444444}
OUTPUT=${OUTPUT:-DSI-1}
LOG=$H/var/log/pro1x-session
HOST_USER=user

in_h() { chroot "$H" "$@"; }
as_user() {
	chroot --userspec=$HUID:$HUID --groups=4,20,30002,670 "$H" /usr/bin/env -i \
		HOME=/home/user USER=user LOGNAME=user SHELL=/bin/sh \
		PATH=/opt/wunderw/bin:/usr/bin:/bin:/usr/sbin:/sbin \
		LANG=en_US.utf8 TMPDIR=/var/tmp QT_IM_MODULE=MInputContext \
		DISPLAY=:0 XAUTHORITY=/home/user/.Xauthority \
		DBUS_SESSION_BUS_ADDRESS="$(cat "$H/var/run/pro1x-session-bus" 2>/dev/null)" \
		"$@"
}
host_x() { su $HOST_USER -c "DISPLAY=:0 $*"; }

do_mount() {
	mountpoint -q "$H" || mount -o loop "$IMG" "$H"
	for m in dev dev/pts proc sys; do
		mountpoint -q "$H/$m" || mount --bind "/$m" "$H/$m"
	done
	mountpoint -q "$H/dev/shm" || mount -t tmpfs -o mode=1777 shm "$H/dev/shm"
	mountpoint -q "$H/var/run" || mount -t tmpfs -o mode=1777 run "$H/var/run"
	mkdir -p "$H/var/run/dbus" "$H/tmp/.X11-unix"
	mountpoint -q "$H/tmp/.X11-unix" || mount --bind /tmp/.X11-unix "$H/tmp/.X11-unix"
	cp /home/$HOST_USER/.Xauthority "$H/home/user/.Xauthority"
	chown $HUID:$HUID "$H/home/user/.Xauthority"
	cp /etc/resolv.conf "$H/etc/resolv.conf"
	mkdir -p "$LOG"
	chown $HUID:$HUID "$LOG"
}

do_umount() {
	do_stop || true
	for m in tmp/.X11-unix var/run dev/shm dev/pts dev proc sys; do
		mountpoint -q "$H/$m" && umount "$H/$m"
	done
	umount "$H"
}

# Processes whose root is the Harmattan tree.
h_pids() {
	local p r
	for p in /proc/[0-9]*; do
		r=$(readlink "$p/root" 2>/dev/null) || continue
		[ "$r" = "$H" ] && echo "${p#/proc/}"
	done
	return 0
}

do_stop() {
	local pids
	pids=$(h_pids)
	if [ -n "$pids" ]; then
		kill $pids 2>/dev/null || true
		sleep 2
		pids=$(h_pids)
		[ -z "$pids" ] || kill -9 $pids 2>/dev/null || true
	fi
	# SysV IPC left behind: Qt's GL shader cache would hand stale program
	# binaries to the next session.
	ipcs -m | awk -v u=$HUID '$3 == u { print $2 }' | xargs -r -n1 ipcrm -m
	ipcs -s | awk -v u=$HUID '$3 == u { print $2 }' | xargs -r -n1 ipcrm -s
	host_x xrandr --output $OUTPUT --scale 1x1 --filter nearest || true
	su $HOST_USER -c "/usr/sbin/dsmetool -c 3 -T 180 -m -17 -f /usr/bin/hildon-desktop" || true
}

start_bg() {
	local name=$1; shift
	as_user /bin/sh -c "exec $* > /var/log/pro1x-session/$name.log 2>&1" &
}

do_start() {
	do_mount
	ipcs -m | awk -v u=$HUID '$3 == u { print $2 }' | xargs -r -n1 ipcrm -m
	ipcs -s | awk -v u=$HUID '$3 == u { print $2 }' | xargs -r -n1 ipcrm -s
	# hildon-desktop and its panels run under dsme's process lifeguard.
	for p in hildon-desktop hildon-home hildon-status-menu hildon-input-method; do
		/usr/sbin/dsmetool -k /usr/bin/$p 2>/dev/null || true
	done
	host_x xrandr --output $OUTPUT --scale ${SCALE}x$SCALE --filter bilinear

	rm -f "$H/var/run/dbus/pid"
	in_h /usr/bin/dbus-daemon --system --fork 2>&1 | grep -v 'Unable to translate credential' || true
	as_user /usr/bin/dbus-daemon --session --fork --print-address \
		> "$H/var/run/pro1x-session-bus"
	ln -sf /var/run/m.mthemedaemon "$H/tmp/m.mthemedaemon"
	start_bg nok-image-broker /usr/libexec/nok-image-broker

	start_bg mthemedaemon /usr/bin/mthemedaemon
	sleep 3
	start_bg mcompositor /usr/bin/mcompositor
	sleep 2
	start_bg sysuid /usr/bin/sysuid -graphicssystem meego
	start_bg meegotouchhome /usr/bin/meegotouchhome -remote-theme -graphicssystem meego
	start_bg applauncherd /usr/bin/applauncherd
	echo "Harmattan session started, logs in ${LOG#$H}"
}

case "${1:-}" in
mount) do_mount ;;
umount) do_umount ;;
start) do_start ;;
stop) do_stop ;;
shell) do_mount; as_user /bin/sh -l ;;
*) echo "usage: $0 mount|start|stop|umount|shell"; exit 1 ;;
esac
