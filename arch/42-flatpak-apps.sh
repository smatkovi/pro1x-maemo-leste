#!/bin/bash
# Pure Maps and OSM Scout Server on Arch come from Flathub (aarch64), as
# on other Linux phones; Leste and FreeBSD have packages of their own
# (github.com/smatkovi/pure-maps-ports), Arch does not build them
# (arch/41-hildon-apps.sh skips them). System-wide installation, so that
# Phosh's and the Hildon and MeeGo sessions' menus list them.
#
#   bash arch/42-flatpak-apps.sh           # as root in Leste, image on $MNT
set -euo pipefail
MNT=${MNT:-/mnt/arch}
APPS="io.github.rinigus.PureMaps io.github.rinigus.OSMScoutServer"
log() { echo ">> $*"; }

mountpoint -q "$MNT" || { echo "$MNT is not mounted"; exit 1; }
for m in proc sys dev dev/pts; do
	mountpoint -q "$MNT/$m" || mount --bind "/$m" "$MNT/$m"
done
cleanup() {
	chroot "$MNT" gpgconf --homedir /etc/pacman.d/gnupg --kill all 2> /dev/null || true
	for m in dev/pts dev sys proc; do
		! mountpoint -q "$MNT/$m" || umount "$MNT/$m" || umount -l "$MNT/$m"
	done
}
trap cleanup EXIT
rm -f "$MNT/etc/resolv.conf"
cp /etc/resolv.conf "$MNT/etc/resolv.conf"

log "flatpak and Flathub"
chroot "$MNT" /bin/bash -euo pipefail <<'EOF'
pacman -S --noconfirm --needed flatpak > /dev/null
flatpak remote-add --system --if-not-exists flathub \
	https://dl.flathub.org/repo/flathub.flatpakrepo
EOF

log "apps: $APPS"
# shellcheck disable=SC2086
chroot "$MNT" flatpak install --system --noninteractive -y flathub $APPS
chroot "$MNT" flatpak list --system --app --columns=application,version
log "done"
