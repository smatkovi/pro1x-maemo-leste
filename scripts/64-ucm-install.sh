#!/bin/bash
# The Pro1X's UCM profile with the headset jack (the NixOS port's
# device/audio/ucm2/Pro1X with patches/nixos-audio/0002-ucm-headset-jack.patch,
# docs/audio.md) into every Linux system the boot menu starts: Leste
# (running), the MeeGo copy, Kali, LuneOS and Arch. New images get it from
# their builders (45-pro1x-hardware.sh; the others copy Leste's); this is
# for the systems that are on the phone already. No image is rebuilt.
#
#   PHONE="ssh root@172.16.42.1" bash scripts/64-ucm-install.sh [install|restore]
#       from the build host: the profile is made here (the NixOS port's
#       files, NIXOS, default $WORK/build/nixos-pro1x, patched), then it and
#       this script go to the phone and run there, in Leste as root
#   UCM_SRC=<dir> bash 64-ucm-install.sh [install|restore]
#       on the phone, in Leste as root, with the profile's three files in
#       <dir> (or NIXOS=<checkout> to make them there)
#
# install (the default): each system's profile is saved first, once, as
# /var/backups/pro1x-ucm/Pro1X (a later install keeps that first copy),
# then the new files replace it; ucm2/sm4250/sm4250.conf, the name
# alsa-lib looks for (driver sm4250), is linked to it where missing.
# restore: the saved files back. Neither restarts the sound server: its
# next start reads the profile (a reboot; on Leste also "pulseaudio -k" as
# the session's user, docs/audio.md). Images that are mounted already are
# written where they are, as 66-boot-trial-install.sh does.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
ACTION=${1:-install}
PATCH=patches/nixos-audio/0002-ucm-headset-jack.patch
log() { echo ">> $*"; }

case $ACTION in
install|restore) ;;
*) sed -n '2,/^set -euo/p' "$0" | sed '$d'; exit 1 ;;
esac

# make_profile DIR: the NixOS port's three files, patched, in DIR
make_profile() {
	local n=$NIXOS/device/audio/ucm2/Pro1X
	mkdir -p "$1"
	cp "$n/Pro1X.conf" "$n/HiFi.conf" "$n/VoiceCall.conf" "$1/"
	patch -s -d "$1" -p1 < "$REPO/$PATCH" ||
	    { echo "$PATCH does not apply to $n"; exit 1; }
}

if [ -n "${PHONE:-}" ]; then
	WORK=${WORK:-$(cd "$REPO/.." && pwd)}
	NIXOS=${NIXOS:-$WORK/build/nixos-pro1x}
	d=$(mktemp -d)
	trap 'rm -r "$d"' EXIT
	make_profile "$d/Pro1X"
	install -m 755 "$0" "$d/64-ucm-install.sh"
	log "to the phone: the profile ($(cd "$d/Pro1X" && md5sum ./*.conf | tr -s ' \n' ' ')) and $ACTION"
	tar -C "$d" --owner=0 --group=0 -cf - Pro1X 64-ucm-install.sh |
	    $PHONE "set -e; t=\$(mktemp -d); tar -C \$t -xf -
		UCM_SRC=\$t/Pro1X bash \$t/64-ucm-install.sh $ACTION; rm -r \$t"
	exit 0
fi

[ "$(id -u)" = 0 ] || { echo "run as root (on the phone, in Leste)"; exit 1; }
if [ "$ACTION" = install ]; then
	if [ -z "${UCM_SRC:-}" ]; then
		[ -n "${NIXOS:-}" ] || { echo "set UCM_SRC=<the profile's files> or NIXOS=<checkout>"; exit 1; }
		UCM_SRC=$(mktemp -d)
		trap 'rm -r "$UCM_SRC"' EXIT
		make_profile "$UCM_SRC"
	fi
	ls "$UCM_SRC/Pro1X.conf" "$UCM_SRC/HiFi.conf" "$UCM_SRC/VoiceCall.conf" > /dev/null
	grep -q 'JackControl "Headphone Jack"' "$UCM_SRC/HiFi.conf" ||
	    { echo "$UCM_SRC/HiFi.conf is not the patched profile"; exit 1; }
fi

# into ROOT
into() {
	local u=$1/usr/share/alsa/ucm2 b=$1/var/backups/pro1x-ucm/Pro1X
	if [ "$ACTION" = restore ]; then
		if [ ! -f "$b/Pro1X.conf" ]; then
			log "  nothing saved ($b): left as it is"
			return 0
		fi
		install -m 644 "$b"/*.conf "$u/Pro1X/"
		log "  restored from $b"
		return 0
	fi
	if [ -f "$u/Pro1X/Pro1X.conf" ] && [ ! -e "$b" ]; then
		mkdir -p "$b"
		cp -a "$u/Pro1X/." "$b/"
		log "  saved the profile it had as $b"
	fi
	install -d -m 755 "$u/Pro1X" "$u/sm4250"
	install -m 644 "$UCM_SRC/Pro1X.conf" "$UCM_SRC/HiFi.conf" "$UCM_SRC/VoiceCall.conf" "$u/Pro1X/"
	if [ ! -e "$u/sm4250/sm4250.conf" ] && [ ! -L "$u/sm4250/sm4250.conf" ]; then
		ln -s ../Pro1X/Pro1X.conf "$u/sm4250/sm4250.conf"
	elif [ "$(readlink "$u/sm4250/sm4250.conf" || true)" != ../Pro1X/Pro1X.conf ]; then
		log "  note: $u/sm4250/sm4250.conf is not the link to ../Pro1X/Pro1X.conf, left as it is"
	fi
	log "  installed: $(cd "$u/Pro1X" && md5sum ./*.conf | tr -s ' \n' ' ')"
}

# with_image IMAGE: the image's root (Btrfs: subvolume @), mounted here
# unless it is already (66-boot-trial-install.sh)
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
		into "$m"
		return 0
	fi
	m=$(mktemp -d /mnt/pro1x-ucm.XXXX)
	if [ "$(blkid -o value -s TYPE "$img")" = btrfs ]; then
		mount -o loop,subvol=@ "$img" "$m"
	else
		mount -o loop "$img" "$m"
	fi
	log "$1"
	into "$m"
	sync
	umount "$m"
	rmdir "$m"
}

log "Leste (running)"
into /
with_image meego-leste.img
with_image kali.img
with_image luneos.img
with_image arch.img
log "done ($ACTION); the sound servers read it at their next start"
