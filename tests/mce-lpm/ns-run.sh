#!/bin/sh
# ns-run.sh VARIANT: one run of mce against the power key in low power mode.
# Runs as root inside the Leste chroot as PID 1 of new PID, mount and
# network namespaces (run-pc.sh starts it with unshare): everything mounted
# here is private and goes with the namespace, every process with PID 1,
# and nothing reaches the PC's devices, buses or X server (no abstract
# sockets in a new network namespace). mce sees:
#   /dev/input           event0, a FIFO this script writes input events
#                        into (mce takes it for a keyboard), event1 a
#                        dangling link (the "touchscreen": not opened)
#   /sys/class/backlight panel0 with brightness, max_brightness 255,
#                        device/lpm (the panel's low power mode)
#   /sys/class/input     event1/device/inhibited (input-ctrl's touchscreen
#                        switch, from lock-tklock's policy)
#   /etc/mce             the package's mce.ini, 50-test.ini (modules,
#                        short time-outs), rtconf.ini (display_dim_timeout 3,
#                        use_low_power_mode 1)
#   the system bus       a private dbus-daemon, a stand-in system UI
#   poweroff, reboot...  a script that only logs
# VARIANT is a directory /root/mcetest/VARIANT with an mce package
# unpacked (dpkg-deb -x) and conf-VARIANT; results in /root/mcetest/out-VARIANT.
set -u
V=$1
M=/root/mcetest
O=$M/out-$V
T=/tmp/mcetest
mount -t proc proc /proc
mount -t tmpfs -o mode=755 mcetest /dev/input
mkfifo /dev/input/event0
ln -s /nonexistent /dev/input/event1
mount -t tmpfs -o mode=755 mcetest /sys/class/backlight
mkdir -p /sys/class/backlight/panel0/device
echo 0 > /sys/class/backlight/panel0/brightness
echo 255 > /sys/class/backlight/panel0/max_brightness
echo 0 > /sys/class/backlight/panel0/bl_power
echo 0 > /sys/class/backlight/panel0/device/lpm
mount -t tmpfs -o mode=755 mcetest /sys/class/input
mkdir -p /sys/class/input/event1/device
echo 0 > /sys/class/input/event1/device/inhibited
mount -t tmpfs mcetest /tmp
mount -t tmpfs mcetest /run
mount -t tmpfs -o mode=755 mcetest /etc/mce
cp -a "$M/conf-$V/." /etc/mce/
mount -t tmpfs mcetest /var/lib/mce
cp -a "$M/var-mce/." /var/lib/mce/
mkdir -p $T
for f in poweroff reboot halt shutdown; do
	p=$(command -v $f) && mount --bind $M/fake-power.sh "$p"
done
rm -rf "$O"
mkdir -p "$O"

export DBUS_SYSTEM_BUS_ADDRESS=unix:path=$T/bus
dbus-daemon --config-file=$M/bus.conf --nofork --nopidfile > "$O/bus.log" 2>&1 &
BUS=$!
i=0
while [ ! -S $T/bus ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
$M/fake-systemui > "$O/systemui.log" 2>&1 &
SUI=$!
dbus-monitor --system "type='signal',interface='com.nokia.mce.signal'" \
	> "$O/monitor.log" 2>&1 &
MON=$!
# the FIFO's writer end held open: mce's reads see no end of file
exec 7<> /dev/input/event0
$M/$V/usr/bin/mce --force-stderr --verbose --verbose --verbose --verbose \
	> "$O/mce.log" 2>&1 &
MCE=$!
i=0
until dbus-send --system --print-reply --dest=com.nokia.mce /com/nokia/mce/request \
	com.nokia.mce.request.get_display_status > /dev/null 2>&1; do
	sleep 0.2; i=$((i + 1))
	[ $i -lt 150 ] || { echo "mce did not come up" > "$O/result.txt"; break; }
done
START=$(date +%s.%N)
since() { awk -v a="$(date +%s.%N)" -v b="$START" 'BEGIN { printf "%.1f", a - b }'; }

ask() {
	dbus-send --system --print-reply=literal --dest=com.nokia.mce \
		/com/nokia/mce/request "com.nokia.mce.request.$1" 2> /dev/null | tr -d ' '
}
state() {
	printf '%6.1f %-34s status=%-6s tklock=%-8s br=%-3s lpm=%s ts_inhibited=%s\n' \
		"$(since)" "$1" "$(ask get_display_status)" \
		"$(ask get_tklock_mode)" "$(cat /sys/class/backlight/panel0/brightness)" \
		"$(cat /sys/class/backlight/panel0/device/lpm)" \
		"$(cat /sys/class/input/event1/device/inhibited)" >> "$O/result.txt"
}
# N samples a second apart
watch() {
	n=$1 label=$2
	while [ "$n" -gt 0 ]; do
		sleep 1
		state "$label"
		n=$((n - 1))
	done
}
mark() { echo "------ $(since) $*" >> "$O/result.txt"; }
key() { $M/evsend 1 116 1; sleep 0.2; $M/evsend 1 116 0; }
slide() { $M/evsend 5 10 "$1"; }
on_req() {
	dbus-send --system --type=method_call --dest=com.nokia.mce /com/nokia/mce/request \
		com.nokia.mce.request.req_display_state_on
}
# until the panel is in its low power mode (LPM_ON, the stand-by screen)
# or blank (LPM_OFF), at most 30 s; read from the fake panel, as +pro1x4
# says "off" for both
wait_lpm_on() {
	i=0
	until [ "$(cat /sys/class/backlight/panel0/device/lpm)" = 1 ] || [ $i -ge 300 ]; do
		sleep 0.1; i=$((i + 1))
	done
	sleep 0.5
}
wait_lpm_off() {
	i=0
	until { [ "$(cat /sys/class/backlight/panel0/device/lpm)" = 0 ] &&
		[ "$(cat /sys/class/backlight/panel0/brightness)" = 0 ]; } || [ $i -ge 300 ]; do
		sleep 0.1; i=$((i + 1))
	done
	sleep 0.5
}

state "mce up"
mark "idle: on, dimmed, low power mode (LPM_ON), then LPM_OFF"
watch 13 idle
wait_lpm_off
mark "power key (short) in LPM_OFF (blank)"
key
watch 9 "after the press in LPM_OFF"
# to the stand-by screen: from LPM_OFF only a press (5 s after it, the
# lock screen's time-out) leads there, with either version
wait_lpm_off
key
wait_lpm_on
mark "power key (short) in LPM_ON (the stand-by screen)"
key
watch 3 "after the press in LPM_ON"
wait_lpm_on
mark "req_display_state_on in LPM_ON, locked (the double tap)"
on_req
watch 3 "after req_display_state_on"
wait_lpm_off
mark "keyboard slide opened in LPM_OFF"
slide 1
watch 3 "after the slide opened"
mark "keyboard slide closed"
slide 0
wait_lpm_off
key
wait_lpm_on
state "LPM_ON again"
mark "keyboard slide opened in LPM_ON"
slide 1
watch 3 "after the slide opened"
mark "keyboard slide closed"
slide 0
watch 10 idle
state end
exec 7>&-
kill $MCE $MON $SUI 2> /dev/null
sleep 1
kill $BUS 2> /dev/null
wait 2> /dev/null
[ ! -f $T/power-requests.log ] || cp $T/power-requests.log "$O/"
