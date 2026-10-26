#!/bin/bash
# Builds the Maemo Leste rootfs image for the Pro1X (build host, needs sudo).
#
# Takes the official arm64-generic image as the base, puts the self-built
# 7.1 kernel with its modules and the Pro1X firmware into it, wires up the
# loop-root initramfs and pulls initrd + vmlinuz out into $WORK/out.
set -euo pipefail
WORK=${WORK:?set WORK=<build-dir>}
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
SRC_IMG=${SRC_IMG:-$WORK/dl/leste.img}
IMG=${IMG:-$WORK/out/maemo-leste.img}
IMG_SIZE=${IMG_SIZE:-24G}
KVER=${KVER:-7.1.0+}
ROOT_PW=${ROOT_PW:-pro1x-leste}
SSH_PUBKEY=${SSH_PUBKEY:-$HOME/.ssh/id_ed25519.pub}

SRC_MNT=/mnt/leste-src
DST_MNT=/mnt/leste-dst

# The binfmt registered on Arch points at the dynamically linked
# qemu-aarch64, whose host libraries are missing inside the chroot. So for the
# duration of the build we register the static variant and restore the
# original state afterwards.
BINFMT_SWITCHED=0
AARCH64_MAGIC='\x7f\x45\x4c\x46\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00'
AARCH64_MASK='\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff'

binfmt_use_static() {
  [ -x /usr/bin/qemu-aarch64-static ] || { echo "qemu-aarch64-static missing"; exit 1; }
  if [ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] &&
     grep -q 'interpreter /usr/bin/qemu-aarch64-static' /proc/sys/fs/binfmt_misc/qemu-aarch64; then
    return 0   # already static, nothing to do
  fi
  [ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] &&
    sudo -n sh -c 'echo 0 > /proc/sys/fs/binfmt_misc/qemu-aarch64'
  sudo -n sh -c "echo ':pro1x-aarch64:M::$AARCH64_MAGIC:$AARCH64_MASK:/usr/bin/qemu-aarch64-static:PF' > /proc/sys/fs/binfmt_misc/register"
  BINFMT_SWITCHED=1
}

binfmt_restore() {
  [ "$BINFMT_SWITCHED" = 1 ] || return 0
  [ -e /proc/sys/fs/binfmt_misc/pro1x-aarch64 ] &&
    sudo -n sh -c 'echo -1 > /proc/sys/fs/binfmt_misc/pro1x-aarch64'
  [ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] &&
    sudo -n sh -c 'echo 1 > /proc/sys/fs/binfmt_misc/qemu-aarch64'
  BINFMT_SWITCHED=0
}

# Run inside the target; PATH set explicitly because the chroot inherits the host PATH.
in_target() {
  sudo -n chroot "$DST_MNT" /usr/bin/env \
    PATH=/usr/sbin:/usr/bin:/sbin:/bin DEBIAN_FRONTEND=noninteractive \
    /bin/bash -c "$1"
}

cleanup() {
  set +e
  binfmt_restore
  for m in "$DST_MNT/mnt/pro1x-repo" "$DST_MNT/mnt/nixos-pro1x" \
           "$DST_MNT/dev/pts" "$DST_MNT/dev" "$DST_MNT/proc" "$DST_MNT/sys" "$DST_MNT" "$SRC_MNT"; do
    mountpoint -q "$m" && sudo -n umount -l "$m"
  done
}
trap cleanup EXIT

[ -f "$WORK/out/Image.gz" ] || { echo "kernel missing, run 30-build-kernel.sh first"; exit 1; }
[ -d "$WORK/out/modules/lib/modules/$KVER" ] || { echo "modules for $KVER missing"; exit 1; }

# Firmware blobs (proprietary, from the sm6115-mainline project)
FW=$WORK/build/firmware-fxtec-qx1050
[ -d "$FW" ] || git clone -q --depth 1 \
  https://github.com/sm6115-mainline/firmware-fxtec-qx1050.git "$FW"

echo ">> creating empty image ($IMG_SIZE, sparse)"
mkdir -p "$(dirname "$IMG")"
rm -f "$IMG"
truncate -s "$IMG_SIZE" "$IMG"
mkfs.ext4 -q -L maemo-leste -E lazy_itable_init=0,lazy_journal_init=0 "$IMG"

sudo -n mkdir -p "$SRC_MNT" "$DST_MNT"
sudo -n mount -o ro,loop,offset=$((264192 * 512)) "$SRC_IMG" "$SRC_MNT"
sudo -n mount -o loop "$IMG" "$DST_MNT"

echo ">> copying rootfs"
sudo -n rsync -aHAX --numeric-ids "$SRC_MNT/" "$DST_MNT/"

echo ">> installing kernel, modules, firmware"
sudo -n cp "$WORK/out/Image.gz" "$DST_MNT/boot/vmlinuz-$KVER"
sudo -n cp "$WORK/build/linux/.config" "$DST_MNT/boot/config-$KVER"
sudo -n rsync -a "$WORK/out/modules/lib/modules/$KVER" "$DST_MNT/lib/modules/"
sudo -n rsync -a "$FW/lib/firmware/" "$DST_MNT/lib/firmware/"

echo ">> installing initramfs hooks"
sudo -n install -d "$DST_MNT/etc/initramfs-tools/hooks" \
                   "$DST_MNT/etc/initramfs-tools/scripts/local-top" \
                   "$DST_MNT/etc/initramfs-tools/scripts/init-bottom"
sudo -n install -m 0755 "$REPO/initramfs/hooks/pro1x-loop" \
  "$DST_MNT/etc/initramfs-tools/hooks/"
sudo -n install -m 0755 "$REPO/initramfs/local-top/pro1x-loop-root" \
  "$DST_MNT/etc/initramfs-tools/scripts/local-top/"
sudo -n install -m 0755 "$REPO/initramfs/init-bottom/pro1x-move-host" \
  "$DST_MNT/etc/initramfs-tools/scripts/init-bottom/"

echo ">> adjusting fstab (no separate /boot on this device)"
sudo -n sed -i 's|^/dev/mmcblk0p1 /boot|#&  # Pro1X: /boot lives in the rootfs image|' \
  "$DST_MNT/etc/fstab"

echo ">> setting up access"
binfmt_use_static
in_target "echo 'root:$ROOT_PW' | chpasswd"
if [ -f "$SSH_PUBKEY" ]; then
  sudo -n install -d -m 0700 "$DST_MNT/root/.ssh"
  sudo -n tee "$DST_MNT/root/.ssh/authorized_keys" < "$SSH_PUBKEY" >/dev/null
  sudo -n chmod 0600 "$DST_MNT/root/.ssh/authorized_keys"
fi
# No getty on ttyMSM0: the debug UART (uart4) is disabled in the device tree,
# so the line only made init respawn a failing agetty every few minutes
# (an earlier image has it: removed). The consoles are tty1 and USB (ttyGS0).
sudo -n sed -i '/ttyMSM0/d' "$DST_MNT/etc/inittab"

sudo -n mount -t proc proc "$DST_MNT/proc"
sudo -n mount -t sysfs sys "$DST_MNT/sys"
sudo -n mount --bind /dev "$DST_MNT/dev"
sudo -n mount --bind /dev/pts "$DST_MNT/dev/pts"

# Hardware support (45..49): apt, source builds, overlay. Under qemu this
# takes a long time; WITH_HARDWARE=0 builds the bare rootfs and the same
# scripts can run on the phone later.
if [ "${WITH_HARDWARE:-1}" = 1 ]; then
  echo ">> hardware support (scripts 45-49 in the chroot)"
  NIXOS=$WORK/build/nixos-pro1x
  [ -d "$NIXOS" ] || git clone -q --depth 1 \
    https://github.com/nyymi609/fxtec-pro1x-nixos.git "$NIXOS"
  sudo -n mkdir -p "$DST_MNT/mnt/pro1x-repo" "$DST_MNT/mnt/nixos-pro1x"
  sudo -n mount --bind "$REPO" "$DST_MNT/mnt/pro1x-repo"
  sudo -n mount --bind "$NIXOS" "$DST_MNT/mnt/nixos-pro1x"
  sudo -n cp -L /etc/resolv.conf "$DST_MNT/etc/resolv.conf"
  for s in 45-pro1x-hardware 46-sensor-stack 47-libcal 48-camera-stack 49-apps 49-hidpi; do
    in_target "REPO=/mnt/pro1x-repo NIXOS=/mnt/nixos-pro1x SRC=/root/src bash /mnt/pro1x-repo/scripts/$s.sh"
  done
  sudo -n umount "$DST_MNT/mnt/pro1x-repo" "$DST_MNT/mnt/nixos-pro1x"
fi

echo ">> building initramfs (chroot via qemu-aarch64/binfmt)"
# Everything needed to reach the root filesystem (UFS, ext4, loop, USB
# gadget) is built into the kernel, so no modules go into the initramfs.
# With all of them it was 11 MiB; the ABL then still booted, but a small
# initramfs keeps kernel + ramdisk well inside what the stock image uses.
sudo -n mkdir -p "$DST_MNT/etc/initramfs-tools/conf.d"
sudo -n tee "$DST_MNT/etc/initramfs-tools/conf.d/pro1x" >/dev/null <<'EOF'
# Pro1X: everything to reach the root filesystem is built into the kernel.
MODULES=list
COMPRESS=xz
EOF
in_target "depmod $KVER && update-initramfs -c -k $KVER"

sudo -n cp "$DST_MNT/boot/initrd.img-$KVER" "$WORK/out/initrd.img-$KVER"
sudo -n chown "$(id -u):$(id -g)" "$WORK/out/initrd.img-$KVER"
sudo -n sh -c "df -h $DST_MNT | tail -1"

cleanup
trap - EXIT
echo "done:"
ls -la "$IMG" "$WORK/out/initrd.img-$KVER"
du -h --apparent-size "$IMG" | cut -f1 | sed 's/^/  apparent: /'
du -h "$IMG" | cut -f1 | sed 's/^/  used:     /'
