#!/bin/bash
# Music with a deep buffer (docs/audio.md, "Music with a deep buffer") into
# the systems with PulseAudio that are on the phone already: Leste (running)
# and the MeeGo copy. New images get the same from 45-pro1x-hardware.sh. No
# image is rebuilt, nothing is restarted; it takes effect with a kernel that
# has patch 0034 (7.1.0-pro1x.11-audio and later) and the next X session
# (a reboot). Kali and Arch run PipeWire, which this does not touch.
#
#   PHONE="ssh root@172.16.42.1" bash scripts/72-audio-deep-buffer.sh [install|restore]
#       from the build host: the files and this script go to the phone and
#       run there, in Leste as root
#   SRC=<repository checkout> bash 72-audio-deep-buffer.sh [install|restore]
#       on the phone, in Leste as root
#
# Files:
#   /usr/local/bin/pro1x-music-sink        tools/pro1x-music-sink, compiled
#       in Leste: loads PulseAudio's sink on the deep-buffer front end,
#       moves the media player there, gives it the default sink's volume
#   /etc/X11/Xsession.post/48pro1x-music-sink   starts it with the session
#   /etc/udev/rules.d/86-pro1x-music-route.rules and
#   /usr/local/sbin/pro1x-music-route      the front end's route when the
#       card appears
# install (the default): every file it replaces is saved first, once, under
# /var/backups/pro1x-audio/ of that system; restore puts the saved ones
# back and removes the new ones. Off without restore: touch
# /etc/pulse/pro1x-deep-buffer-off (the next session loads no sink).
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
ACTION=${1:-install}
log() { echo ">> $*"; }

case $ACTION in
install|restore) ;;
*) sed -n '2,/^set -euo/p' "$0" | sed '$d'; exit 1 ;;
esac

FILES=(tools/pro1x-music-sink/pro1x-music-sink.c
	rootfs-overlay/etc/X11/Xsession.post/48pro1x-music-sink
	rootfs-overlay/etc/udev/rules.d/86-pro1x-music-route.rules
	rootfs-overlay/usr/local/sbin/pro1x-music-route
	scripts/72-audio-deep-buffer.sh)

if [ -n "${PHONE:-}" ]; then
	log "to the phone: ${#FILES[@]} files and $ACTION"
	tar -C "$REPO" --owner=0 --group=0 -cf - "${FILES[@]}" |
	    $PHONE "set -e; t=\$(mktemp -d); tar -C \$t -xf -
		SRC=\$t bash \$t/scripts/72-audio-deep-buffer.sh $ACTION; rm -r \$t"
	exit 0
fi

[ "$(id -u)" = 0 ] || { echo "run as root (on the phone, in Leste)"; exit 1; }
SRC=${SRC:-$REPO}
BK=var/backups/pro1x-audio

# put ROOT FILE MODE DEST: FILE (in SRC) as ROOT/DEST, the old one saved
put() {
	local r=$1 f=$SRC/$2 mode=$3 d=$1/$4 b=$1/$BK/$4
	if [ ! -e "$b" ] && [ ! -e "$b.new" ]; then
		mkdir -p "$(dirname "$b")"
		if [ -e "$d" ]; then cp -a "$d" "$b"; else touch "$b.new"; fi
	fi
	install -D -m "$mode" "$f" "$d"
	log "  $4"
}

# back ROOT DEST: the saved file back, or DEST removed if it was new
back() {
	local d=$1/$2 b=$1/$BK/$2
	if [ -e "$b" ]; then
		cp -a "$b" "$d"
		rm -f "$b"
		log "  $2 restored"
	elif [ -e "$b.new" ]; then
		rm -f "$d" "$b.new"
		log "  $2 removed (it was new)"
	fi
}

# The program for Leste and the MeeGo copy (the same libraries)
BIN=
if [ "$ACTION" = install ]; then
	pkg-config --exists libpulse 2> /dev/null ||
		apt-get install -y -q --no-install-recommends gcc libc6-dev pkg-config libpulse-dev
	BIN=$(mktemp)
	trap 'rm -f "$BIN"' EXIT
	# shellcheck disable=SC2046
	gcc -O2 -Wall -o "$BIN" "$SRC/tools/pro1x-music-sink/pro1x-music-sink.c" \
		$(pkg-config --cflags --libs libpulse)
fi

leste() {   # leste ROOT
	local r=$1 f
	if [ "$ACTION" = restore ]; then
		for f in usr/local/bin/pro1x-music-sink etc/X11/Xsession.post/48pro1x-music-sink \
			etc/udev/rules.d/86-pro1x-music-route.rules usr/local/sbin/pro1x-music-route; do
			back "$r" "$f"
		done
		return 0
	fi
	SRC=/ put "$r" "$BIN" 755 usr/local/bin/pro1x-music-sink
	put "$r" rootfs-overlay/etc/X11/Xsession.post/48pro1x-music-sink 644 \
		etc/X11/Xsession.post/48pro1x-music-sink
	put "$r" rootfs-overlay/etc/udev/rules.d/86-pro1x-music-route.rules 644 \
		etc/udev/rules.d/86-pro1x-music-route.rules
	put "$r" rootfs-overlay/usr/local/sbin/pro1x-music-route 755 usr/local/sbin/pro1x-music-route
}

# with_image IMAGE FUNCTION: the image's root, mounted here unless it is
# already (as 71-power-userspace.sh)
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
	m=$(mktemp -d /mnt/pro1x-audio.XXXX)
	mount -o loop "$img" "$m"
	log "$1"
	"$2" "$m"
	sync
	umount "$m"
	rmdir "$m"
}

log "Leste (running)"
leste /
with_image meego-leste.img leste
log "done ($ACTION); udev and the X session use it at their next start (a reboot)"
