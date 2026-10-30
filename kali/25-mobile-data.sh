#!/bin/bash
# Mobile data for a Kali image made before 20-pro1x-hardware.sh had it:
# the rmtfs and ModemManager drop-ins and pro1x-modem-wait from
# rootfs-overlay/, and the two packages 20 now installs (the APN database,
# qmicli). A new image gets all of it from 20; running this again is harmless.
#
#   sudo bash kali/25-mobile-data.sh                 in Kali itself
#   MNT=/mnt/kali bash kali/25-mobile-data.sh        in Leste, the image
#                                                    mounted on $MNT
#
# Packages are only installed in Kali itself (with a network); for a
# mounted image the script says which ones are missing. Reboot Kali
# afterwards: restarting rmtfs by hand stops the modem (and the Wi-Fi).
# docs/kali-mobile-data.md has the test.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
MNT=${MNT:-/}
O=$REPO/kali/rootfs-overlay
PKGS="modemmanager libqmi-glib5 libqrtr-glib0 rmtfs tqftpserv qrtr-tools network-manager mobile-broadband-provider-info libqmi-utils"
log() { echo ">> $*"; }

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
if [ "$MNT" != / ]; then
	mountpoint -q "$MNT" || { echo "$MNT is not mounted"; exit 1; }
fi
grep -q '^ID=kali' "$MNT/etc/os-release" ||
	{ echo "$MNT is not Kali (in Leste: MNT=/mnt/kali)"; exit 1; }

log "drop-ins and pro1x-modem-wait"
for d in rmtfs.service.d ModemManager.service.d; do
	install -d "$MNT/etc/systemd/system/$d"
	install -m 644 "$O/etc/systemd/system/$d/pro1x.conf" "$MNT/etc/systemd/system/$d/pro1x.conf"
done
install -D -m 755 "$O/usr/lib/pro1x/pro1x-modem-wait" "$MNT/usr/lib/pro1x/pro1x-modem-wait"

missing=
for p in mobile-broadband-provider-info libqmi-utils; do
	dpkg-query --admindir="$MNT/var/lib/dpkg" -W -f='${Status}\n' "$p" 2> /dev/null |
		grep -q 'install ok installed' || missing="$missing $p"
done
if [ -n "$missing" ]; then
	if [ "$MNT" = / ]; then
		log "installing$missing"
		apt-get install -y -q $missing ||
			echo "!! apt-get failed (no network?): install$missing later; only gsm.auto-config and qmicli need them"
	else
		echo "!! missing in the image:$missing (in Kali: sudo apt install$missing)"
	fi
fi

if [ "$MNT" = / ]; then
	systemctl daemon-reload
fi
log "versions"
dpkg-query --admindir="$MNT/var/lib/dpkg" -W -f='${Package} ${Version}\n' $PKGS 2> /dev/null || true
log "done; reboot Kali to start the modem with the new order"
