#!/bin/bash
# Helpers the MeeGo session runs on the Leste side (/usr/lib/pro1x-meego):
#   n9-sensors  the Pro1X's sensors as the N9's input device (sensors/), with
#               sensord's configuration for it
#   iphbd       dsme's IP heartbeat service, which Leste's dsme lacks (iphb/)
#   mainvolumed PulseAudio's com.Nokia.MainVolume1 for sysuid's volume bar,
#               on Leste's PulseAudio (mainvolume/)
#   bmed        the battery management server's event file, for the
#               replaced libbmeipc (bme/, 38-hardware-adaptation.sh)
#   positioningd  Nokia's positioning daemon for QtLocation, on Leste's
#               gpsd (positioning/)
#   leste-apps  Leste applications (camera, Firefox...) in Harmattan's
#               application grid (leste-apps/, /etc/pro1x-meego/leste-apps)
#
# Runs as root on Leste, after scripts/46-sensor-stack.sh (libssc).
#
#   H=/mnt/harmattan bash meego/41-leste-helpers.sh
set -eu
SRC=$(cd "$(dirname "$0")" && pwd)
H=${H:-/mnt/harmattan}
STATE=$H/var/lib/pro1x-graft
export DEBIAN_FRONTEND=noninteractive
export PKG_CONFIG_PATH=/usr/local/lib/aarch64-linux-gnu/pkgconfig

[ -f "$STATE/added" ] || { echo "run 30-graft-debian-runtime.sh first"; exit 1; }

apt-get install -y -q --no-install-recommends gcc libc6-dev pkg-config libglib2.0-dev \
	libpulse-dev > /dev/null
install -d /usr/lib/pro1x-meego
gcc -O2 -Wall -o /usr/lib/pro1x-meego/n9-sensors "$SRC/sensors/n9-sensors.c" \
	$(pkg-config --cflags --libs libssc) -lm

gcc -O2 -Wall -o /usr/lib/pro1x-meego/iphbd "$SRC/iphb/iphbd.c"
gcc -O2 -Wall -o /usr/lib/pro1x-meego/mainvolumed "$SRC/mainvolume/mainvolumed.c" \
	$(pkg-config --cflags --libs gio-unix-2.0 libpulse-mainloop-glib) -lm
gcc -O2 -Wall -DBMED -o /usr/lib/pro1x-meego/bmed "$SRC/bme/bmeipc.c"
apt-get install -y -q --no-install-recommends python3-dbus python3-gi > /dev/null
install -m 755 "$SRC/positioning/positioningd.py" /usr/lib/pro1x-meego/positioningd
install -m 755 "$SRC/leste-apps/leste-apps.py" /usr/lib/pro1x-meego/leste-apps
install -m 644 "$SRC/leste-apps/org.pro1x.LesteLauncher.conf" /etc/dbus-1/system.d/
install -d /etc/pro1x-meego
[ -e /etc/pro1x-meego/leste-apps ] || install -m 644 "$SRC/leste-apps/leste-apps.list" /etc/pro1x-meego/leste-apps

install -m 644 "$SRC/sensors/95-pro1x.conf" "$H/etc/sensorfw/sensord.conf.d/95-pro1x.conf"
P=/etc/sensorfw/sensord.conf.d/95-pro1x.conf
grep -qxF "$P" "$STATE/added" || echo "$P" >> "$STATE/added"
echo "n9-sensors, iphbd, mainvolumed, bmed, positioningd and leste-apps installed"
