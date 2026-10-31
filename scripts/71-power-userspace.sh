#!/bin/bash
# The user space side of the power fixes for 7.1.0-pro1x.10
# (docs/power-management.md, "Power fixes for pro1x.10") into the Linux
# systems that are on the phone already: Leste (running), the MeeGo copy,
# Kali and Arch. New images get the same files from their builders
# (45-pro1x-hardware.sh and 46-sensor-stack.sh, kali/ and arch/20-*). No
# image is rebuilt, nothing is restarted.
#
#   PHONE="ssh root@172.16.42.1" bash scripts/71-power-userspace.sh [install|restore]
#       from the build host: the files and this script go to the phone and
#       run there, in Leste as root
#   SRC=<repository checkout> bash 71-power-userspace.sh [install|restore]
#       on the phone, in Leste as root
#
# Leste and the MeeGo copy (PulseAudio, OpenRC):
#   /etc/pulse/default.pa.d/50-pro1x.pa   the card suspends after 3 s of
#       silence with a kernel that has the loudspeaker fix (0026), else 300 s
#   pro1x-autosuspend, compiled in Leste: hexagonrpcd stopped for a suspend
#       only on kernels without 0031, the Bluetooth UART's wake-up left to
#       Bluetooth's switch; in /etc/pro1x-autosuspend.conf the old default
#       line "StopServices=pro1x-hexagonrpcd" becomes a comment
#   pro1x-rpmtest (the options sensors and btwake)
#   gpsd in no runlevel (location-daemon starts it on demand; at boot it
#       would start the modem's GNSS engine for good, /etc/conf.d/gpsd)
# Kali and Arch (PipeWire, NetworkManager, systemd):
#   WirePlumber's suspend after 3 s, NetworkManager's Wi-Fi power save
#   GPS on demand: pro1x-gnss.service only while gpsd runs, gpsd started by
#       gpsd.socket for its first client instead of at boot (both ran the
#       GNSS engine's 1 Hz fixes from boot on), /etc/default/gpsd without -n
# All: the Wi-Fi power save hook (udev rule and pro1x-wlan-powersave).
#
# install (the default): every file or service link it replaces or removes
# is saved first, once, under /var/backups/pro1x-power/ of that system;
# restore puts the saved ones back and removes the ones that were new. The
# sound servers, udev, init and systemd read them at their next start (a
# reboot).
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
ACTION=${1:-install}
log() { echo ">> $*"; }

case $ACTION in
install|restore) ;;
*) sed -n '2,/^set -euo/p' "$0" | sed '$d'; exit 1 ;;
esac

FILES=(rootfs-overlay/etc/pulse/default.pa.d/50-pro1x.pa
	rootfs-overlay/etc/udev/rules.d/85-pro1x-wlan-powersave.rules
	rootfs-overlay/usr/local/sbin/pro1x-wlan-powersave
	tools/pro1x-autosuspend/pro1x-autosuspend.c
	tools/pro1x-autosuspend/pro1x-autosuspend.conf
	tools/pro1x-rpmtest/pro1x-rpmtest
	kali/rootfs-overlay/etc/wireplumber/wireplumber.conf.d/50-pro1x-suspend.conf
	kali/rootfs-overlay/etc/NetworkManager/conf.d/50-pro1x-wifi-powersave.conf
	kali/rootfs-overlay/etc/systemd/system/pro1x-gnss.service
	kali/rootfs-overlay/etc/default/gpsd
	scripts/71-power-userspace.sh)

if [ -n "${PHONE:-}" ]; then
	log "to the phone: ${#FILES[@]} files and $ACTION"
	tar -C "$REPO" --owner=0 --group=0 -cf - "${FILES[@]}" |
	    $PHONE "set -e; t=\$(mktemp -d); tar -C \$t -xf -
		SRC=\$t bash \$t/scripts/71-power-userspace.sh $ACTION; rm -r \$t"
	exit 0
fi

[ "$(id -u)" = 0 ] || { echo "run as root (on the phone, in Leste)"; exit 1; }
SRC=${SRC:-$REPO}

# put ROOT FILE MODE DEST: FILE (in SRC) as ROOT/DEST, the old one saved
put() {
	local r=$1 f=$SRC/$2 mode=$3 d=$1/$4 b=$1/var/backups/pro1x-power/$4
	if [ ! -e "$b" ] && [ ! -e "$b.new" ]; then
		mkdir -p "$(dirname "$b")"
		if [ -e "$d" ]; then cp -a "$d" "$b"; else touch "$b.new"; fi
	fi
	install -D -m "$mode" "$f" "$d"
	log "  $4"
}

# back ROOT DEST: the saved file back, or DEST removed if it was new
back() {
	local d=$1/$2 b=$1/var/backups/pro1x-power/$2
	if [ -e "$b" ]; then
		cp -a "$b" "$d"
		log "  $2 restored"
	elif [ -e "$b.new" ]; then
		rm -f "$d"
		log "  $2 removed (it was new)"
	fi
}

# The service links (a runlevel's entry, a systemd unit's .wants): -L, not
# -e, since a link into another system's root does not resolve from here.
# unlink ROOT LINK: LINK removed, saved first
unlink_saved() {
	local l=$1/$2 b=$1/var/backups/pro1x-power/$2
	[ -L "$l" ] || return 0
	if [ ! -L "$b" ]; then
		mkdir -p "$(dirname "$b")"
		cp -a "$l" "$b"
	fi
	rm -f "$l"
	log "  $2 removed"
}

# link_new ROOT LINK TARGET: LINK -> TARGET made, unless LINK is there
link_new() {
	local l=$1/$2 b=$1/var/backups/pro1x-power/$2
	if [ -L "$l" ] || [ -e "$l" ]; then return 0; fi
	mkdir -p "$(dirname "$l")" "$(dirname "$b")"
	touch "$b.new"
	ln -s "$3" "$l"
	log "  $2 -> $3"
}

# relink ROOT LINK (restore): the saved link back, or a new one removed
relink() {
	local l=$1/$2 b=$1/var/backups/pro1x-power/$2
	if [ -L "$b" ]; then
		mkdir -p "$(dirname "$l")"
		rm -f "$l"
		cp -a "$b" "$l"
		log "  $2 restored"
	elif [ -e "$b.new" ]; then
		rm -f "$l"
		log "  $2 removed (it was new)"
	fi
}

# The autosuspend binary for Leste and the MeeGo copy (the same libraries)
AS=
if [ "$ACTION" = install ]; then
	AS=$(mktemp)
	trap 'rm -f "$AS"' EXIT
	gcc -O2 -Wall -o "$AS" "$SRC/tools/pro1x-autosuspend/pro1x-autosuspend.c" \
		$(pkg-config --cflags --libs gio-unix-2.0)
fi

leste() {   # leste ROOT
	local r=$1 c=$1/etc/pro1x-autosuspend.conf
	if [ "$ACTION" = restore ]; then
		for f in etc/pulse/default.pa.d/50-pro1x.pa etc/udev/rules.d/85-pro1x-wlan-powersave.rules \
			usr/local/sbin/pro1x-wlan-powersave usr/local/sbin/pro1x-autosuspend \
			etc/pro1x-autosuspend.conf usr/local/bin/pro1x-rpmtest; do
			back "$r" "$f"
		done
		relink "$r" etc/runlevels/default/gpsd
		return 0
	fi
	# gpsd (and with it the GNSS engine) only when location-daemon asks
	unlink_saved "$r" etc/runlevels/default/gpsd
	put "$r" rootfs-overlay/etc/pulse/default.pa.d/50-pro1x.pa 644 etc/pulse/default.pa.d/50-pro1x.pa
	put "$r" rootfs-overlay/etc/udev/rules.d/85-pro1x-wlan-powersave.rules 644 \
		etc/udev/rules.d/85-pro1x-wlan-powersave.rules
	put "$r" rootfs-overlay/usr/local/sbin/pro1x-wlan-powersave 755 usr/local/sbin/pro1x-wlan-powersave
	SRC=/ put "$r" "$AS" 755 usr/local/sbin/pro1x-autosuspend
	put "$r" tools/pro1x-rpmtest/pro1x-rpmtest 755 usr/local/bin/pro1x-rpmtest
	if [ -e "$c" ]; then
		mkdir -p "$r/var/backups/pro1x-power/etc"
		[ -e "$r/var/backups/pro1x-power/etc/pro1x-autosuspend.conf" ] ||
			cp -a "$c" "$r/var/backups/pro1x-power/etc/pro1x-autosuspend.conf"
		sed -i 's/^StopServices=pro1x-hexagonrpcd$/#&/' "$c"
		log "  etc/pro1x-autosuspend.conf: StopServices $(grep -q '^StopServices=' "$c" && echo "kept as set" || echo "by the kernel")"
	else
		put "$r" tools/pro1x-autosuspend/pro1x-autosuspend.conf 644 etc/pro1x-autosuspend.conf
	fi
}

kali_arch() {    # kali_arch ROOT (PipeWire, NetworkManager, systemd)
	local r=$1 f d s w=etc/systemd/system
	for f in etc/wireplumber/wireplumber.conf.d/50-pro1x-suspend.conf \
		etc/NetworkManager/conf.d/50-pro1x-wifi-powersave.conf \
		$w/pro1x-gnss.service etc/default/gpsd; do
		if [ "$ACTION" = restore ]; then back "$r" "$f"; else put "$r" "kali/rootfs-overlay/$f" 644 "$f"; fi
	done
	for f in etc/udev/rules.d/85-pro1x-wlan-powersave.rules usr/local/sbin/pro1x-wlan-powersave; do
		if [ "$ACTION" = restore ]; then
			back "$r" "$f"
		else
			put "$r" "rootfs-overlay/$f" "$([ "${f#usr}" != "$f" ] && echo 755 || echo 644)" "$f"
		fi
	done
	# GPS on demand: what "systemctl enable pro1x-gnss.service gpsd.socket"
	# and "disable gpsd.service" do (kali/ and arch/20-pro1x-hardware.sh)
	if [ "$ACTION" = restore ]; then
		for f in multi-user.target.wants/pro1x-gnss.service multi-user.target.wants/gpsd.service \
			gpsd.service.wants/pro1x-gnss.service sockets.target.wants/gpsd.socket; do
			relink "$r" "$w/$f"
		done
		return 0
	fi
	unlink_saved "$r" $w/multi-user.target.wants/pro1x-gnss.service
	unlink_saved "$r" $w/multi-user.target.wants/gpsd.service
	link_new "$r" $w/gpsd.service.wants/pro1x-gnss.service /$w/pro1x-gnss.service
	s=
	for d in usr/lib lib; do
		if [ -f "$r/$d/systemd/system/gpsd.socket" ]; then s=/$d/systemd/system/gpsd.socket; break; fi
	done
	if [ -n "$s" ]; then
		link_new "$r" $w/sockets.target.wants/gpsd.socket "$s"
	else
		log "  no gpsd.socket in this system: gpsd starts only by hand"
	fi
}

# with_image IMAGE FUNCTION: the image's root (Btrfs: subvolume @), mounted
# here unless it is already (as 64-ucm-install.sh)
with_image() {
	local img=/host/$1 m dev
	[ -f "$img" ] || { log "$1: not there"; return 0; }
	dev=$(losetup -j "$img" | cut -d: -f1 | head -1)
	m=
	[ -z "$dev" ] || m=$(findmnt -n -o TARGET -S "$dev" | head -1)
	if [ -n "$m" ]; then
		if findmnt -n -o OPTIONS -S "$dev" | head -1 | tr , '\n' | grep -qx ro; then
			log "$1 (mounted read only on $m): left out, run this again once it is free"
			return 0
		fi
		log "$1 (mounted on $m)"
		"$2" "$m"
		return 0
	fi
	m=$(mktemp -d /mnt/pro1x-power.XXXX)
	if [ "$(blkid -o value -s TYPE "$img")" = btrfs ]; then
		mount -o loop,subvol=@ "$img" "$m"
	else
		mount -o loop "$img" "$m"
	fi
	log "$1"
	"$2" "$m"
	sync
	umount "$m"
	rmdir "$m"
}

log "Leste (running)"
leste /
with_image meego-leste.img leste
with_image kali.img kali_arch
with_image arch.img kali_arch
log "done ($ACTION); PulseAudio, WirePlumber, NetworkManager, udev, OpenRC and systemd read it at their next start"
