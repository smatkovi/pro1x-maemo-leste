#!/bin/bash
# A chroot of Arch Linux ARM (aarch64) on the PC, run by qemu-aarch64-static,
# for the Arch steps' builds without the phone: Arch Linux ARM's generic root
# file system (its signature checked against the Arch Linux ARM build key,
# as 10-make-rootfs.sh does), without its kernel and firmware, updated. The
# steps then run on the PC with MNT on it, as they do in Leste:
#
#   arch/pc-chroot.sh mount        unpack and prepare (the first time),
#                                  mount, the static qemu for aarch64
#   sudo REPO=... MNT=$(arch/pc-chroot.sh root) bash arch/40-hildon.sh
#   arch/pc-chroot.sh shell        a shell in it
#   arch/pc-chroot.sh status       what is mounted, the disks' space
#   arch/pc-chroot.sh umount       its processes stopped, unmounted,
#                                  binfmt as before the mount
#   arch/pc-chroot.sh delete       (unmounted) all of it removed
#
# Needs sudo (without a password: sudo -n). CHROOT on a disk with room
# (about 6 GB for steps 40 to 42 with the parts the Events page needs);
# MIN_FREE_GB (GiB, default 10) must stay free there; mount checks it.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
WORK=${WORK:-$(cd "$REPO/.." && pwd)}
CHROOT=${CHROOT:-$WORK/build/arch-chroot}
DL=${DL:-$WORK/dl/alarm}
URL=${URL:-http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz}
# Arch Linux ARM Build System <builder@archlinuxarm.org>
KEY=68B3537F39A313B3E574D06777193F152BDBE6A6
KEYSERVER=${KEYSERVER:-hkps://keyserver.ubuntu.com}
MIN_FREE_GB=${MIN_FREE_GB:-10}
R=$CHROOT/root
# what mount changed in binfmt_misc, for umount to put back
STATE=$CHROOT/binfmt-changed
BF=/proc/sys/fs/binfmt_misc
log() { echo ">> $*"; }

free_ok() {
	local need=$1 p a
	p=$(readlink -m "$2")
	while [ ! -e "$p" ]; do p=$(dirname "$p"); done
	a=$(df -B1M --output=avail "$p" | tail -1)
	if [ "$a" -lt $((need * 1024)) ]; then
		echo "!! $((a / 1024)) GiB free on the disk of $p, $need wanted" >&2
		return 1
	fi
}

# The aarch64 entry Arch registers may run the dynamically linked qemu,
# whose libraries are not in the chroot: if the chroot does not run, the
# static one while it is mounted, as scripts/leste-chroot.sh does (an entry
# for the static one that is there already is left as it is)
AARCH64_MAGIC='\x7f\x45\x4c\x46\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00'
AARCH64_MASK='\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff'
binfmt_use_static() {
	local changed=
	[ ! -e "$STATE" ] || return 0
	[ -x /usr/bin/qemu-aarch64-static ] || { echo "qemu-aarch64-static missing"; exit 1; }
	if [ -e $BF/qemu-aarch64 ] && grep -qx enabled $BF/qemu-aarch64 &&
	    ! grep -q 'interpreter /usr/bin/qemu-aarch64-static' $BF/qemu-aarch64; then
		sudo -n sh -c "echo 0 > $BF/qemu-aarch64"
		changed=qemu-aarch64
	fi
	if ! grep -lqs 'interpreter /usr/bin/qemu-aarch64-static' $BF/*; then
		sudo -n sh -c "echo ':pro1x-aarch64:M::$AARCH64_MAGIC:$AARCH64_MASK:/usr/bin/qemu-aarch64-static:PF' > $BF/register"
		changed="$changed pro1x-aarch64"
	fi
	echo "$changed" > "$STATE"
}
binfmt_restore() {
	[ -e "$STATE" ] || return 0
	case " $(cat "$STATE") " in *" pro1x-aarch64 "*)
		[ ! -e $BF/pro1x-aarch64 ] || sudo -n sh -c "echo -1 > $BF/pro1x-aarch64" ;;
	esac
	case " $(cat "$STATE") " in *" qemu-aarch64 "*)
		[ ! -e $BF/qemu-aarch64 ] || sudo -n sh -c "echo 1 > $BF/qemu-aarch64" ;;
	esac
	rm -f "$STATE"
}

in_chroot() {
	sudo -n chroot "$R" /usr/bin/env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/bin \
		HOME=/root LANG=C.UTF-8 TERM="${TERM:-dumb}" "$@"
}

kill_chroot() {
	sudo -n sh -c 'for p in /proc/[0-9]*; do
		[ "$(readlink "$p/root" 2> /dev/null)" = "$1" ] && kill -TERM "${p#/proc/}" 2> /dev/null
	done; true' sh "$R"
}

# Arch Linux ARM's root, checked, unpacked, prepared as 20-pro1x-hardware.sh
# begins (pacman without its sandbox, the keys, no kernel), updated
make_root() {
	local t=$DL/${URL##*/} g
	free_ok $((MIN_FREE_GB + 4)) "$CHROOT"
	mkdir -p "$DL"
	[ -f "$t" ] || { log "downloading $URL"; curl -sSfL -o "$t.part" "$URL"; mv "$t.part" "$t"; }
	curl -sSfL -o "$t.sig" "$URL.sig"
	log "checking the signature"
	g=$(mktemp -d)
	gpg --homedir "$g" --batch --quiet --keyserver "$KEYSERVER" --recv-keys "$KEY"
	if ! gpg --homedir "$g" --batch --status-fd 1 --verify "$t.sig" "$t" 2> /dev/null |
	    grep -q "^\[GNUPG:\] VALIDSIG $KEY "; then
		echo "the tarball is not signed by $KEY"
		rm -rf "$g"
		exit 1
	fi
	rm -rf "$g"
	log "unpacking to $R"
	sudo -n rm -rf "$R.part"
	sudo -n mkdir -p "$R.part"
	sudo -n bsdtar --numeric-owner -xpf "$t" -C "$R.part"
	sudo -n mv "$R.part" "$R"
}

prepare_root() {
	[ ! -e "$CHROOT/prepared" ] || return 0
	log "pacman keys, no kernel or firmware, updates"
	sudo -n cp /etc/resolv.conf "$R/etc/resolv.conf.pro1x"
	in_chroot /bin/bash -euo pipefail <<'EOF'
rm -f /etc/resolv.conf
mv /etc/resolv.conf.pro1x /etc/resolv.conf
grep -q '^DisableSandbox' /etc/pacman.conf ||
	sed -i '/^\[options\]/a DisableSandbox' /etc/pacman.conf
pacman-key --init > /dev/null 2>&1
pacman-key --populate archlinuxarm > /dev/null 2>&1
gpgconf --homedir /etc/pacman.d/gnupg --kill all 2> /dev/null || true
for p in linux-aarch64 linux-aarch64-headers mkinitcpio linux-firmware; do
	! pacman -Q "$p" > /dev/null 2>&1 || pacman -Rdd --noconfirm "$p" > /dev/null
done
pacman -Qq | grep '^linux-firmware-' | xargs -r pacman -Rdd --noconfirm > /dev/null
pacman -Syu --noconfirm > /dev/null
pacman -Scc --noconfirm > /dev/null
gpgconf --homedir /etc/pacman.d/gnupg --kill all 2> /dev/null || true
EOF
	sudo -n touch "$CHROOT/prepared"
}

binfmt_check() {
	sudo -n chroot "$R" /usr/bin/true 2> /dev/null && return 0
	binfmt_use_static
	sudo -n chroot "$R" /usr/bin/true || { echo "the chroot does not run (binfmt?)"; exit 1; }
}

do_mount() {
	free_ok "$MIN_FREE_GB" "$CHROOT"
	mkdir -p "$CHROOT"
	[ -d "$R" ] || make_root
	binfmt_check
	# a mount point of its own, which the steps ask for (and pacman's
	# space check)
	mountpoint -q "$R" || sudo -n mount --bind "$R" "$R"
	for m in proc sys dev dev/pts; do
		mountpoint -q "$R/$m" || sudo -n mount --bind "/$m" "$R/$m"
	done
	prepare_root
	for m in dev/pts dev sys proc; do
		! mountpoint -q "$R/$m" || sudo -n umount "$R/$m"
	done
	log "mounted: $R"
}

do_umount() {
	kill_chroot
	sleep 1
	for m in dev/pts dev sys proc ""; do
		! mountpoint -q "$R/$m" || sudo -n umount "$R/$m" || sudo -n umount -l "$R/$m"
	done
	binfmt_restore
}

case ${1:-} in
mount) do_mount ;;
root) echo "$R" ;;
shell)
	mountpoint -q "$R" || { echo "not mounted: $0 mount"; exit 1; }
	for m in proc sys dev dev/pts; do
		mountpoint -q "$R/$m" || sudo -n mount --bind "/$m" "$R/$m"
	done
	in_chroot /bin/bash -l || true
	for m in dev/pts dev sys proc; do
		! mountpoint -q "$R/$m" || sudo -n umount "$R/$m"
	done
	;;
status)
	mount | grep -F " $R" || echo "(not mounted)"
	df -h "$(readlink -m "$WORK")" / | sed 's/^/   /'
	[ ! -d "$R" ] || sudo -n du -sh "$CHROOT" 2> /dev/null | sed 's/^/   chroot: /'
	;;
umount) do_umount ;;
delete)
	! mountpoint -q "$R" || { echo "mounted: $0 umount first"; exit 1; }
	sudo -n rm -rf "$CHROOT"
	rm -rf "$DL"
	log "deleted $CHROOT and $DL"
	;;
*)
	sed -n '2,/^set -/s/^# \{0,1\}//p' "$0" | sed '$d' >&2
	exit 2
	;;
esac
