#!/bin/bash
# Btrfs for the Arch Linux ARM image, with a snapshot before and after
# every pacman transaction (snapper, snap-pac), each of which the boot menu
# offers ("Arch Linux ARM: snapshots"; pro1x-boot next arch@<number>).
#
# Runs as root on the phone, in Maemo Leste, with the image of steps 10 to
# 30 unmounted: it copies the ext4 image into a new Btrfs image with the
# subvolumes
#
#   @           the root          @snapshots  /.snapshots (snapper)
#   @home       /home             @log        /var/log
#   @pkg        /var/cache/pacman/pkg
#
# so that going back to a snapshot keeps the homes, the logs and pacman's
# downloads. The ext4 image stays as /host/arch-ext4.img until it is
# removed by hand. The boot menu reads the snapshots from
# /host/pro1x-boot/snapshots/arch (pro1x-snapshot-menu writes it whenever
# /.snapshots changes) and mounts Btrfs roots with the initramfs of
# tools/pro1x-boot and initramfs/ in this repository (a new boot image).
#
#   bash arch/15-btrfs.sh
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
IMG=${IMG:-/host/arch.img}
NEW=${NEW:-/host/arch-btrfs.img}
OLD=${OLD:-/host/arch-ext4.img}
SIZE=${SIZE:-16G}
SRC=/mnt/arch-ext4
TOP=/mnt/arch-btrfs
ROOT=/mnt/arch-root
# lazytime: timestamps written lazily (as the initramfs mounts the root)
OPTS=noatime,lazytime,compress=zstd:1
log() { echo ">> $*"; }

[ "$(blkid -o value -s TYPE "$IMG")" = ext4 ] || { echo "$IMG is not ext4"; exit 1; }
[ ! -e "$NEW" ] || { echo "$NEW exists"; exit 1; }
[ ! -e "$OLD" ] || { echo "$OLD exists"; exit 1; }
! losetup -j "$IMG" | grep -q . || { echo "$IMG is in use (mounted?)"; exit 1; }
for t in mkfs.btrfs:btrfs-progs rsync:rsync; do
	command -v "${t%%:*}" > /dev/null ||
		DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends "${t#*:}" > /dev/null
done
modprobe btrfs

mkdir -p "$SRC" "$TOP" "$ROOT"
cleanup() {
	for m in dev/pts dev sys proc host var/cache/pacman/pkg var/log .snapshots home; do
		! mountpoint -q "$ROOT/$m" 2> /dev/null || umount "$ROOT/$m" || umount -l "$ROOT/$m"
	done
	! mountpoint -q "$ROOT" || umount "$ROOT"
	! mountpoint -q "$TOP" || umount "$TOP"
	! mountpoint -q "$SRC" || umount "$SRC"
}
trap cleanup EXIT

log "ext4 image, read-only"
mount -o loop,ro "$IMG" "$SRC"

log "Btrfs image $NEW ($SIZE)"
truncate -s "$SIZE" "$NEW"
mkfs.btrfs -q -L arch "$NEW"
mount -o "loop,$OPTS" "$NEW" "$TOP"
for s in @ @home @snapshots @log @pkg; do
	btrfs -q subvolume create "$TOP/$s"
done

log "copying"
rsync -aHAXx --numeric-ids --exclude='/home/*' --exclude='/var/log/*' \
	--exclude='/var/cache/pacman/pkg/*' "$SRC/" "$TOP/@/"
rsync -aHAX --numeric-ids "$SRC/home/" "$TOP/@home/"
rsync -aHAX --numeric-ids "$SRC/var/log/" "$TOP/@log/"
rsync -aHAX --numeric-ids "$SRC/var/cache/pacman/pkg/" "$TOP/@pkg/"
mkdir -p "$TOP/@/.snapshots" "$TOP/@/home" "$TOP/@/var/log" "$TOP/@/var/cache/pacman/pkg"
umount "$SRC"

# The root is mounted by the initramfs (subvolume @ or a snapshot), so the
# table has no line for / (a remount with Btrfs options would fail on a
# snapshot's overlay); the other subvolumes are mounted from the same image.
log "fstab"
sed -i '\|^/dev/loop0[[:space:]]\+/[[:space:]]|d' "$TOP/@/etc/fstab"
cat >> "$TOP/@/etc/fstab" <<EOF
/dev/loop0 /home btrfs subvol=/@home,$OPTS 0 0
/dev/loop0 /.snapshots btrfs subvol=/@snapshots,$OPTS 0 0
/dev/loop0 /var/log btrfs subvol=/@log,$OPTS 0 0
/dev/loop0 /var/cache/pacman/pkg btrfs subvol=/@pkg,$OPTS 0 0
EOF

log "snapper and snap-pac"
# the root subvolume mounted by itself: the chroot's / is then a mount
# point, which pacman needs for its free space check; it and the other
# subvolumes from the same loop device (a second one on the same file
# would show Btrfs the same file system twice)
R=$ROOT
dev=$(findmnt -no SOURCE "$TOP")
mount -o "subvol=/@,$OPTS" "$dev" "$R"
for s in home:home snapshots:.snapshots log:var/log pkg:var/cache/pacman/pkg; do
	mount -o "subvol=/@${s%%:*},$OPTS" "$dev" "$R/${s#*:}"
done
# each a slave of the system's: unmounting dev/pts here would otherwise take
# the system's /dev/pts along (a bind of a shared /dev is its peer)
for m in proc sys dev dev/pts; do mount --bind "/$m" "$R/$m"; mount --make-rslave "$R/$m"; done
mkdir -p "$R/host" && mount --bind /host "$R/host" && mount --make-rslave "$R/host"
rm -f "$R/etc/resolv.conf" && cp /etc/resolv.conf "$R/etc/resolv.conf"
cp -a "$REPO/arch/rootfs-overlay/usr/local/sbin/pro1x-snapshot-menu" \
	"$REPO/arch/rootfs-overlay/usr/local/sbin/pro1x-rollback" "$R/usr/local/sbin/"
cp -a "$REPO/arch/rootfs-overlay/etc/systemd/system/pro1x-snapshot-menu.service" \
	"$REPO/arch/rootfs-overlay/etc/systemd/system/pro1x-snapshot-menu.path" \
	"$R/etc/systemd/system/"
chroot "$R" /bin/bash -euo pipefail <<'EOF'
pacman -S --noconfirm --needed btrfs-progs snapper snap-pac > /dev/null
gpgconf --homedir /etc/pacman.d/gnupg --kill all 2> /dev/null || true
# snapper's root config by hand: /.snapshots is its own subvolume already
sed -e 's/^FSTYPE=.*/FSTYPE="btrfs"/' -e 's/^ALLOW_GROUPS=.*/ALLOW_GROUPS="wheel"/' \
	-e 's/^NUMBER_LIMIT=.*/NUMBER_LIMIT="50"/' -e 's/^NUMBER_LIMIT_IMPORTANT=.*/NUMBER_LIMIT_IMPORTANT="10"/' \
	-e 's/^TIMELINE_CREATE=.*/TIMELINE_CREATE="no"/' \
	/usr/share/snapper/config-templates/default > /etc/snapper/configs/root
grep -q '^SUBVOLUME=' /etc/snapper/configs/root || echo 'SUBVOLUME="/"' >> /etc/snapper/configs/root
sed -i 's/^SNAPPER_CONFIGS=.*/SNAPPER_CONFIGS="root"/' /etc/conf.d/snapper
chmod 750 /.snapshots
snapper --no-dbus -c root create -d "Btrfs, as installed" > /dev/null
systemctl enable snapper-cleanup.timer pro1x-snapshot-menu.path pro1x-snapshot-menu.service
/usr/local/sbin/pro1x-snapshot-menu
EOF
cat "$R/host/pro1x-boot/snapshots/arch"
cleanup
trap - EXIT

log "the Btrfs image becomes $IMG ($OLD keeps the ext4 one)"
mv "$IMG" "$OLD"
mv "$NEW" "$IMG"
log "done: Arch boots from Btrfs with a boot image whose initramfs knows it"
