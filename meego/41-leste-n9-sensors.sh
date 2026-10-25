#!/bin/bash
# The Pro1X's sensors for Harmattan's sensord: n9-sensors (sensors/), which
# the MeeGo session starts on Leste, and sensord's configuration for it.
#
# Runs as root on Leste, after scripts/46-sensor-stack.sh (libssc).
#
#   H=/mnt/harmattan bash meego/41-leste-n9-sensors.sh
set -eu
SRC=$(cd "$(dirname "$0")" && pwd)/sensors
H=${H:-/mnt/harmattan}
STATE=$H/var/lib/pro1x-graft
export DEBIAN_FRONTEND=noninteractive
export PKG_CONFIG_PATH=/usr/local/lib/aarch64-linux-gnu/pkgconfig

[ -f "$STATE/added" ] || { echo "run 30-graft-debian-runtime.sh first"; exit 1; }

apt-get install -y -q --no-install-recommends gcc libc6-dev pkg-config libglib2.0-dev > /dev/null
install -d /usr/lib/pro1x-meego
gcc -O2 -Wall -o /usr/lib/pro1x-meego/n9-sensors "$SRC/n9-sensors.c" \
	$(pkg-config --cflags --libs libssc) -lm

install -m 644 "$SRC/95-pro1x.conf" "$H/etc/sensorfw/sensord.conf.d/95-pro1x.conf"
P=/etc/sensorfw/sensord.conf.d/95-pro1x.conf
grep -qxF "$P" "$STATE/added" || echo "$P" >> "$STATE/added"
echo "n9-sensors installed"
