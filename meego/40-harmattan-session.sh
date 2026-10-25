#!/bin/bash
# Run the Harmattan (MeeGo 1.2) user interface on the Pro1X display.
#
# Maemo Leste stays the hardware layer: its kernel, Xorg (modesetting +
# glamor), modem, WLAN and audio services keep running. The Harmattan
# rootfs runs on top in a chroot and draws on Leste's X server; its
# compositor (mcompositor) takes the place of hildon-desktop. Harmattan's
# user is Leste's user (uid 1000, 37-harmattan-user.sh) and shares Leste's
# system bus (mce, dsme and icd2 speak the same Nokia interfaces) and
# PulseAudio.
#
#   bash meego/40-harmattan-session.sh mount|start|stop|umount|shell
set -eu

IMG=${IMG:-/host/harmattan.img}
H=${H:-/mnt/harmattan}
HUID=1000                       # "user", the same on Leste and Harmattan
# Harmattan works in 854x480; the 2160x1080 panel shows a 960x480 screen,
# in landscape like the N950 (keyboard orientation). MeeGo Touch turns its
# content to portrait itself.
SCALE=${SCALE:-0.444444}
OUTPUT=${OUTPUT:-DSI-1}
ROTATE=${ROTATE:-right}
TOUCH="Goodix Capacitive TouchScreen"
LOG=$H/var/log/pro1x-session
HOST_USER=user

# Harmattan's groups (adm, dialout, input, pulse-access) and Leste's
# device groups, by number.
GROUPS_H=4,20,30002,670
GROUPS_L=$(getent group audio video input pulse-access | cut -d: -f3 | paste -sd,)

as_user() {
	chroot --userspec=$HUID:$HUID --groups=$GROUPS_H,$GROUPS_L "$H" /usr/bin/env -i \
		HOME=/home/user USER=user LOGNAME=user SHELL=/bin/sh \
		PATH=/opt/wunderw/bin:/usr/bin:/bin:/usr/sbin:/sbin \
		LANG=en_US.utf8 TMPDIR=/var/tmp QT_IM_MODULE=MInputContext \
		DISPLAY=:0 XAUTHORITY=/home/user/.Xauthority \
		PULSE_SERVER=unix:/run/user/$HUID/pulse/native \
		DBUS_SESSION_BUS_ADDRESS="$(cat "$H/var/run/pro1x-session-bus" 2>/dev/null)" \
		"$@"
}
host_x() { su $HOST_USER -c "DISPLAY=:0 $*"; }

do_mount() {
	mountpoint -q "$H" || mount -o loop "$IMG" "$H"
	for m in dev dev/pts proc sys; do
		mountpoint -q "$H/$m" || mount --bind "/$m" "$H/$m"
	done
	# Shared with Leste: POSIX shm (PulseAudio's memory blocks), the
	# system bus, the user's runtime dir (PulseAudio socket), X.
	mountpoint -q "$H/dev/shm" || mount --bind /dev/shm "$H/dev/shm"
	mountpoint -q "$H/var/run" || mount -t tmpfs -o mode=1777 run "$H/var/run"
	mkdir -p "$H/var/run/dbus" "$H/tmp/.X11-unix" "$H/run/user/$HUID"
	mountpoint -q "$H/var/run/dbus" || mount --bind /run/dbus "$H/var/run/dbus"
	if [ -d /run/user/$HUID ]; then
		mountpoint -q "$H/run/user/$HUID" || mount --bind /run/user/$HUID "$H/run/user/$HUID"
	fi
	mountpoint -q "$H/tmp/.X11-unix" || mount --bind /tmp/.X11-unix "$H/tmp/.X11-unix"
	cp /home/$HOST_USER/.Xauthority "$H/home/user/.Xauthority"
	chown $HUID:$HUID "$H/home/user/.Xauthority"
	if [ -f /home/$HOST_USER/.config/pulse/cookie ]; then
		install -o $HUID -g $HUID -m 600 /home/$HOST_USER/.config/pulse/cookie "$H/home/user/.pulse-cookie"
	fi
	cp /etc/resolv.conf "$H/etc/resolv.conf"
	mkdir -p "$LOG"
	chown $HUID:$HUID "$LOG"
}

do_umount() {
	do_stop || true
	for m in tmp/.X11-unix run/user/$HUID var/run/dbus var/run dev/shm dev/pts dev proc sys; do
		mountpoint -q "$H/$m" && umount "$H/$m"
	done
	umount "$H"
}

# Qt's GL shader cache (a QSharedMemory segment) outlives the processes and
# would hand stale program binaries to the next session: drop it. Its SysV
# key is ftok() of the qipc file in Harmattan's TMPDIR, proj_id 'Q'.
clear_qt_ipc() {
	local f key
	for f in "$H"/var/tmp/qipc_sharedmemory_qtgles2shadercache* "$H"/var/tmp/qipc_systemsem_qtgles2shadercache*; do
		[ -e "$f" ] || continue
		key=$(( (0x51 << 24) | (($(stat -c %d "$f") & 0xff) << 16) | ($(stat -c %i "$f") & 0xffff) ))
		case "$f" in
		*sharedmemory*) ipcrm -M $key 2>/dev/null || true ;;
		*) ipcrm -S $key 2>/dev/null || true ;;
		esac
		rm -f "$f"
	done
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
	for d in n9-sensors iphbd mainvolumed bmed; do
		start-stop-daemon --stop --quiet --pidfile /run/pro1x-$d.pid --remove-pidfile || true
	done
	pids=$(h_pids)
	if [ -n "$pids" ]; then
		kill $pids 2>/dev/null || true
		sleep 2
		pids=$(h_pids)
		[ -z "$pids" ] || kill -9 $pids 2>/dev/null || true
	fi
	clear_qt_ipc
	host_x xrandr --output $OUTPUT --scale 1x1 --filter nearest || true
	# Back to Leste's desktop, unless this boot is the MeeGo system.
	if [ "$(pro1x-boot current 2>/dev/null)" != meego ]; then
		su $HOST_USER -c "/usr/sbin/dsmetool -c 3 -T 180 -m -17 -f /usr/bin/hildon-desktop" || true
	fi
}

start_bg() {
	local name=$1; shift
	as_user /bin/sh -c "exec $* > /var/log/pro1x-session/$name.log 2>&1" &
}

# Harmattan's system services that run as root on the (shared) system bus.
start_root() {
	local name=$1; shift
	chroot "$H" /usr/bin/env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin \
		/bin/sh -c "exec $* > /var/log/pro1x-session/$name.log 2>&1" &
}

# Harmattan's own upstart jobs (/etc/init/xsession/<job>.conf, or
# /etc/init/<job>.conf for system services): their env lines, pre-start
# script and exec line or script, run as root in the chroot the way upstart
# ran them; the jobs switch to "user" through aegis-exec
# (36-aegis-permissive.sh), with the groups in PRO1X_GROUPS. Upstart's own
# commands are stubs. Extra arguments are environment settings (PAR1=...).
JOBS=/var/run/pro1x-jobs
run_job() {
	local job=$1 conf
	shift
	conf=$H/etc/init/xsession/$job.conf
	[ -f "$conf" ] || conf=$H/etc/init/$job.conf
	[ -f "$conf" ] || { echo "no job $job"; return 0; }
	if [ ! -d "$H$JOBS/bin" ]; then
		mkdir -p "$H$JOBS/bin"
		for c in initctl start stop status waitdbus sysinfoclient; do
			printf '#!/bin/sh\nexit 0\n' > "$H$JOBS/bin/$c"
			chmod 755 "$H$JOBS/bin/$c"
		done
	fi
	awk '
		/^env / { sub(/^env /, "export "); env = env $0 "\n"; next }
		/^pre-start script/ { mode = "pre"; next }
		/^script/ { mode = "main"; next }
		/^(post-start|pre-stop|post-stop) script/ { mode = "skip"; next }
		/^end script/ { mode = ""; next }
		mode == "pre" { pre = pre $0 "\n"; next }
		mode == "main" { main = main $0 "\n"; next }
		mode == "skip" { next }
		/^exec / { main = $0 "\n" }
		END { printf "%s(\n:\n%s)\n%s", env, pre, main }
	' "$conf" > "$H$JOBS/$job.sh"
	chroot "$H" /usr/bin/env -i PATH=$JOBS/bin:/usr/bin:/bin:/usr/sbin:/sbin \
		HOME=/root LANG=en_US.utf8 TMPDIR=/var/tmp QT_IM_MODULE=MInputContext \
		DISPLAY=:0 XAUTHORITY=/home/user/.Xauthority \
		PULSE_SERVER=unix:/run/user/$HUID/pulse/native \
		DBUS_SESSION_BUS_ADDRESS="$(cat "$H/var/run/pro1x-session-bus")" \
		UPSTART_JOB=$job UPSTART_JOB_RESPAWNED=0 \
		PRO1X_GROUPS=$GROUPS_H,$GROUPS_L "$@" \
		/bin/sh -c "exec /bin/sh $JOBS/$job.sh > /var/log/pro1x-session/$job.log 2>&1" &
}

do_start() {
	do_mount
	clear_qt_ipc
	# hildon-desktop and its panels run under dsme's process lifeguard.
	for p in hildon-desktop hildon-home hildon-status-menu hildon-input-method; do
		/usr/sbin/dsmetool -k /usr/bin/$p 2>/dev/null || true
	done
	host_x xrandr --output $OUTPUT --rotate $ROTATE --scale ${SCALE}x$SCALE --filter bilinear
	# X does not turn absolute touch input with the screen (hildon-desktop
	# does that for Leste).
	case $ROTATE in
	right) matrix="0 1 0 -1 0 1 0 0 1" ;;
	left) matrix="0 -1 1 1 0 0 0 0 1" ;;
	inverted) matrix="-1 0 1 0 -1 1 0 0 1" ;;
	*) matrix="1 0 0 0 1 0 0 0 1" ;;
	esac
	host_x xinput set-prop "\"pointer:$TOUCH\"" "\"Coordinate Transformation Matrix\"" $matrix || true

	start_root gconfd /usr/lib/gconf2/gconfd-2
	# The Pro1X's accelerometer, proximity sensor and keyboard slide as the
	# N9's input device (41-leste-helpers.sh), read by Harmattan's
	# sensord (orientation, Screen.TopEdge) and contextkit (keyboard open).
	start-stop-daemon --start --quiet --background --make-pidfile \
		--pidfile /run/pro1x-n9-sensors.pid --exec /usr/lib/pro1x-meego/n9-sensors || true
	# sensord looks for its input devices once, at start.
	i=0
	while [ $i -lt 20 ] && ! grep -q "ST LIS3LV02DL Accelerometer" /proc/bus/input/devices; do
		sleep 0.25
		i=$((i + 1))
	done
	start_root sensord /usr/sbin/sensord
	# dsme's IP heartbeat service, which Leste's dsme lacks: Harmattan's
	# periodic wakeups (the status bar clock among them) depend on it.
	start-stop-daemon --start --quiet --background --make-pidfile \
		--pidfile /run/pro1x-iphbd.pid --exec /usr/lib/pro1x-meego/iphbd || true
	# The main volume in steps, which sysuid's volume bar (side keys) uses,
	# on Leste's PulseAudio: N9's PulseAudio had it on its own D-Bus server.
	install -d -o $HUID -g $HUID "$H/var/run/pulse"
	env HOME=/home/$HOST_USER PULSE_SERVER=unix:/run/user/$HUID/pulse/native \
		start-stop-daemon --start --quiet --background --make-pidfile \
		--pidfile /run/pro1x-mainvolumed.pid --chuid $HUID:$HUID \
		--exec /usr/lib/pro1x-meego/mainvolumed -- "$H/var/run/pulse/dbus-socket" || true
	# The battery: the replaced libbmeipc reads the kernel's power supply
	# class and is told about changes through BME's event file.
	start-stop-daemon --start --quiet --background --make-pidfile \
		--pidfile /run/pro1x-bmed.pid --exec /usr/lib/pro1x-meego/bmed -- "$H/tmp/.bmeevt" || true
	as_user /usr/bin/dbus-daemon --session --fork --print-address \
		> "$H/var/run/pro1x-session-bus"
	ln -sf /var/run/m.mthemedaemon "$H/tmp/m.mthemedaemon"
	start_bg nok-image-broker /usr/libexec/nok-image-broker
	run_job timed
	run_job ohmd
	run_job policy-cfd

	start_bg mthemedaemon /usr/bin/mthemedaemon
	sleep 3
	start_bg mcompositor env LD_PRELOAD=/usr/lib/pro1x/libmcompositor-touch.so /usr/bin/mcompositor
	sleep 2
	start_bg sysuid /usr/bin/sysuid -graphicssystem meego
	start_bg meegotouchhome /usr/bin/meegotouchhome -remote-theme -graphicssystem meego
	start_bg applauncherd /usr/bin/applauncherd
	# Harmattan's group-desktop and group-load-launcher jobs, as far as the
	# Pro1X has their hardware. Left out: the cellular ones (celluid,
	# simatkuid, wappushd: no csd), the N9's WLAN driver (wl1271-cal,
	# wlancond: Leste's icd2 does WLAN), usb_moded, the Immersion vibra
	# (immvibed), devicelockd (Leste's lock code), first-boot and crash
	# reporting (mstartup, cherryui, crash-reporter-daemon, bugreporter).
	for job in qmkeyd meego-im-uiserver ngfd mfeedbackd ohm-session-agent \
		signond-launch tracker-miner nped contactsd commhistoryd mdecorator \
		clipboard smartsearch presenced mission-control messageserver \
		mfacebookd mtwitter usetimed tonegend webupload-recovery \
		calendar-icon-update pkgm-timed-checks backup-framework; do
		run_job $job
	done
	# Leste's mce keeps the display on until the desktop reports ready
	# (its startup-hildon module), which hildon-desktop does in a Leste boot.
	# Without it the screen never blanks, and sysuid, which only runs its
	# status bar clock across display state changes, shows the start time.
	dbus-send --system --type=signal /com/nokia/hildon_desktop com.nokia.HildonDesktop.ready || true
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
