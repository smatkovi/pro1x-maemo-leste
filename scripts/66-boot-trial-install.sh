#!/bin/bash
# The parts of a kernel's trial start that live in the systems: pro1x-boot
# (Leste, the MeeGo copy) and pro1x-boot-trial with its boot service in
# every Linux system the boot menu starts, and the watchdog for systemd's
# systems (tools/pro1x-boot-trial). Installs only: nothing runs without a
# trial (pro1x-boot trial, which 65-kernel-update.sh flash starts). Also
# Leste's pro1x-rtc service where it is installed (docs/alarm-power-on.md).
#
# Runs on the phone as root in Leste, the repository (or its tools/ part)
# in $REPO; 65-kernel-update.sh prepare runs it. Images that are mounted
# already (Arch's for its build steps) are written where they are.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
T=$REPO/tools/pro1x-boot-trial
log() { echo ">> $*"; }

# install_into ROOT openrc|systemd
install_into() {
	local root=$1 init=$2
	install -D -m 755 "$T/pro1x-boot-trial" "$root/usr/local/sbin/pro1x-boot-trial"
	case $init in
	openrc)
		install -D -m 755 "$REPO/tools/pro1x-boot/pro1x-boot" "$root/usr/local/sbin/pro1x-boot"
		# the clock's service as this repository has it (its shutdown no
		# longer sets a one-shot choice for an alarm: the boot menu sees
		# the alarm, docs/alarm-power-on.md); only where it is installed
		if [ -f "$root/etc/init.d/pro1x-rtc" ] &&
		    [ -f "$REPO/rootfs-overlay/etc/init.d/pro1x-rtc" ]; then
			install -m 755 "$REPO/rootfs-overlay/etc/init.d/pro1x-rtc" "$root/etc/init.d/pro1x-rtc"
		fi
		install -D -m 755 "$T/pro1x-boot-trial.openrc" "$root/etc/init.d/pro1x-boot-trial"
		mkdir -p "$root/etc/runlevels/default"
		ln -sfn /etc/init.d/pro1x-boot-trial "$root/etc/runlevels/default/pro1x-boot-trial"
		;;
	systemd)
		install -D -m 644 "$T/pro1x-boot-trial.service" \
		    "$root/etc/systemd/system/pro1x-boot-trial.service"
		mkdir -p "$root/etc/systemd/system/multi-user.target.wants"
		ln -sfn /etc/systemd/system/pro1x-boot-trial.service \
		    "$root/etc/systemd/system/multi-user.target.wants/pro1x-boot-trial.service"
		[ -f "$root/etc/systemd/system.conf.d/pro1x-watchdog.conf" ] ||
		    install -D -m 644 "$T/pro1x-watchdog.conf" \
		        "$root/etc/systemd/system.conf.d/pro1x-watchdog.conf"
		;;
	esac
}

# with_image IMAGE INIT: the image's root (Btrfs: subvolume @), mounted
# here unless it is already
with_image() {
	local img=/host/$1 init=$2 m dev
	[ -f "$img" ] || { log "$1: not there"; return 0; }
	dev=$(losetup -j "$img" | cut -d: -f1 | head -1)
	m=
	[ -z "$dev" ] || m=$(findmnt -n -o TARGET -S "$dev" | head -1)
	if [ -n "$m" ]; then
		# read only (e.g. an overlay's lower layer for a build): left as
		# it is, to be done again once it is free
		if findmnt -n -o OPTIONS -S "$dev" | head -1 | tr , '\n' | grep -qx ro; then
			log "$1 (mounted read only on $m): left out, run this again once it is free"
			return 0
		fi
		log "$1 (mounted on $m)"
		install_into "$m" "$init"
		return 0
	fi
	m=$(mktemp -d /mnt/pro1x-trial.XXXX)
	if [ "$(blkid -o value -s TYPE "$img")" = btrfs ]; then
		mount -o loop,subvol=@ "$img" "$m"
	else
		mount -o loop "$img" "$m"
	fi
	log "$1"
	install_into "$m" "$init"
	sync
	umount "$m"
	rmdir "$m"
}

log "Leste (running)"
install_into / openrc
# the image of the running system is the one mounted on /; the others:
with_image meego-leste.img openrc
with_image kali.img systemd
with_image luneos.img systemd
with_image arch.img systemd
log "done (no trial started)"
