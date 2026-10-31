#!/bin/bash
# run-pc.sh DEB...: mce's low power mode against the power key and the
# keyboard slide, on the PC, in scripts/leste-chroot.sh's chroot (mount it
# first; the chroot has gcc and libdbus-1-dev after a 49-hidpi.sh mce
# build there). For each mce package (e.g. +pro1x4 and +pro1x5) one run of
# ns-run.sh, as PID 1 of new PID, mount and network namespaces, with a fake
# panel, a FIFO for the power key, a private system bus and a stand-in
# system UI; nothing reaches the PC's devices, buses or X server. The
# timeline of each run (display status, lock, brightness, the panel's lpm,
# the touchscreen's inhibit) goes to stdout and to
# $CHROOT/root/root/mcetest/out-<variant>/ (with mce's debug log and the
# signals).
#
#   sudo is needed (mounts, unshare, chroot); never run while the chroot
#   builds.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
WORK=${WORK:-$(cd "$REPO/.." && pwd)}
CHROOT=${CHROOT:-$WORK/build/leste-chroot}
R=$CHROOT/root
T=$(cd "$(dirname "$0")" && pwd)
M=$R/root/mcetest
[ $# -gt 0 ] || { sed -n '2,/^set /p' "$0" | sed '$d'; exit 1; }
mountpoint -q "$R/proc" || { echo "chroot not mounted: scripts/leste-chroot.sh mount"; exit 1; }
in_chroot() {
	sudo -n chroot "$R" /usr/bin/env -i PATH=/usr/local/chroot-bin:/usr/sbin:/usr/bin:/sbin:/bin \
		HOME=/root LANG=C.UTF-8 "$@"
}

sudo -n mkdir -p "$M"
sudo -n cp "$T/fake-systemui.c" "$T/evsend.c" "$T/bus.conf" "$T/fake-power.sh" "$T/ns-run.sh" "$M/"
in_chroot sh -c 'cd /root/mcetest &&
	gcc -O2 -Wall -o fake-systemui fake-systemui.c $(pkg-config --cflags --libs dbus-1) &&
	gcc -O2 -Wall -o evsend evsend.c'

variants=()
for deb in "$@"; do
	sudo -n cp "$deb" "$M/new.deb"
	v=$(in_chroot dpkg-deb -f /root/mcetest/new.deb Version)
	v=${v##*+}
	sudo -n rm -rf "$M/$v" "$M/conf-$v"
	sudo -n mkdir -p "$M/$v" "$M/conf-$v/mce.ini.d" "$M/var-mce"
	sudo -n mv "$M/new.deb" "$M/$v.deb"
	in_chroot dpkg-deb -x "/root/mcetest/$v.deb" "/root/mcetest/$v"
	sudo -n cp "$M/$v/etc/mce/mce.ini" "$M/conf-$v/"
	sudo -n cp "$M/$v/etc/mce/mce.ini.d/10-maemo.ini" "$M/conf-$v/mce.ini.d/"
	# rtconf-ini takes a missing key as false: use_low_power_mode set, as
	# 50-pro1x.ini's UseLowPowerMode=1 is on the phone
	{ sudo -n sed 's/^display_dim_timeout=.*/display_dim_timeout=3/' "$M/$v/etc/mce/rtconf.ini"
	  echo use_low_power_mode=1; } | sudo -n tee "$M/conf-$v/rtconf.ini" > /dev/null
	sudo -n cp "$M/$v/var/lib/mce/mode" "$M/var-mce/"
	# the phone's modules without those needing hardware, buses or X
	# (x11-ctrl, iio-*, led-*, button-backlight, battery-upower, callstate,
	# alarm) and with power-generic and rtconf-ini for dsme and GConf;
	# the phone's low power mode, with short time-outs
	sudo -n tee "$M/conf-$v/mce.ini.d/50-test.ini" > /dev/null <<INI
[Modules]
ModulePath=/root/mcetest/$v/usr/lib/mce/modules
Modules=rtconf-ini;lock-tklock;power-generic;input-ctrl;inactivity;inactivity-inhibit;filter-brightness-als-iio;display
ModulesUser=

[Display]
UseLowPowerMode=1
LowPowerModeBrightness=20
LowPowerModeBlankTimeout=4
DimToBlankTimeout=2
INI
	variants+=("$v")
done

for v in "${variants[@]}"; do
	echo "=== mce +$v"
	sudo -n unshare --mount --propagation private --net --pid --fork --kill-child \
		chroot "$R" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root \
		LANG=C.UTF-8 /bin/sh /root/mcetest/ns-run.sh "$v"
	sudo -n cat "$M/out-$v/result.txt"
	echo "--- systemui requests"
	sudo -n cat "$M/out-$v/systemui.log"
	sudo -n cat "$M/out-$v/power-requests.log" 2> /dev/null || true
done
