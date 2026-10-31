#!/bin/bash
# Builds the Maemo Leste rootfs image for the Pro1X (build host, needs sudo).
#
# Takes the official arm64-generic image as the base, puts the self-built
# 7.1 kernel with its modules and the Pro1X firmware into it, wires up the
# loop-root initramfs and pulls the initrd out into $KOUT.
#
#   KOUT=dir   the kernel: 30-build-kernel.sh's OUT (Image.gz, modules/,
#              kernelrelease, config-<version>); default $WORK/out
#   HIDPI_DEBS=dir, MOBILE_DATA_DEBS=dir   the packages of the releases
#              (leste-hildon-*, leste-mobile-data-*) for 49-hidpi.sh and
#              49-mobile-data.sh (their DEBS=) instead of building them
#              under qemu; default: built
#   UPDATE=0   without pro1x-update at the end (this repository's own
#              packages that only come from the releases, docs/updating.md)
set -euo pipefail
WORK=${WORK:?set WORK=<build-dir>}
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
SRC_IMG=${SRC_IMG:-$WORK/dl/leste.img}
IMG=${IMG:-$WORK/out/maemo-leste.img}
IMG_SIZE=${IMG_SIZE:-24G}
KOUT=${KOUT:-$WORK/out}
KVER=${KVER:-$(cat "$KOUT/kernelrelease" 2> /dev/null || echo 7.1.0+)}
# a kernel built with KLOCAL has its configuration next to it
KCONFIG=$KOUT/config-$KVER
[ -f "$KCONFIG" ] || KCONFIG=$WORK/build/linux/.config
ROOT_PW=${ROOT_PW:-pro1x-leste}
SSH_PUBKEY=${SSH_PUBKEY:-$HOME/.ssh/id_ed25519.pub}
HIDPI_DEBS=${HIDPI_DEBS:-}
MOBILE_DATA_DEBS=${MOBILE_DATA_DEBS:-}
UPDATE=${UPDATE:-1}

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

# Run inside the target; PATH set explicitly because the chroot inherits the
# host PATH. /usr/local/chroot-bin first: its udevadm while guard_on.
in_target() {
  sudo -n chroot "$DST_MNT" /usr/bin/env \
    PATH=/usr/local/chroot-bin:/usr/sbin:/usr/bin:/sbin:/bin DEBIAN_FRONTEND=noninteractive \
    /bin/bash -c "$1"
}

# No services started and no udev events triggered by the packages' scripts
# while the chroot runs: its /dev is the PC's (as scripts/leste-chroot.sh)
guard_on() {
  printf '#!/bin/sh\nexit 101\n' | sudo -n tee "$DST_MNT/usr/sbin/policy-rc.d" > /dev/null
  sudo -n chmod 755 "$DST_MNT/usr/sbin/policy-rc.d"
  sudo -n mkdir -p "$DST_MNT/usr/local/chroot-bin"
  printf '#!/bin/sh\nexit 0\n' | sudo -n tee "$DST_MNT/usr/local/chroot-bin/udevadm" > /dev/null
  sudo -n chmod 755 "$DST_MNT/usr/local/chroot-bin/udevadm"
}
guard_off() {
  sudo -n rm -f "$DST_MNT/usr/sbin/policy-rc.d" "$DST_MNT/usr/local/chroot-bin/udevadm"
  [ ! -d "$DST_MNT/usr/local/chroot-bin" ] || sudo -n rmdir "$DST_MNT/usr/local/chroot-bin"
}

cleanup() {
  set +e
  mountpoint -q "$DST_MNT" && guard_off
  binfmt_restore
  for m in "$DST_MNT/mnt/pro1x-repo" "$DST_MNT/mnt/nixos-pro1x" \
           "$DST_MNT/dev/pts" "$DST_MNT/dev" "$DST_MNT/proc" "$DST_MNT/sys" "$DST_MNT" "$SRC_MNT"; do
    mountpoint -q "$m" && sudo -n umount -l "$m"
  done
}
trap cleanup EXIT

[ -f "$KOUT/Image.gz" ] || { echo "kernel missing, run 30-build-kernel.sh first"; exit 1; }
[ -d "$KOUT/modules/lib/modules/$KVER" ] || { echo "modules for $KVER missing"; exit 1; }
[ -f "$KCONFIG" ] || { echo "no configuration for $KVER"; exit 1; }
for d in "$HIDPI_DEBS" "$MOBILE_DATA_DEBS"; do
  [ -z "$d" ] || ls "$d"/*.deb > /dev/null || { echo "no packages in $d"; exit 1; }
done

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
sudo -n cp "$KOUT/Image.gz" "$DST_MNT/boot/vmlinuz-$KVER"
sudo -n cp "$KCONFIG" "$DST_MNT/boot/config-$KVER"
# owned by root, not by the user who built them (uid 1000 is Leste's "user")
sudo -n rsync -a --chown=0:0 "$KOUT/modules/lib/modules/$KVER" "$DST_MNT/lib/modules/"
sudo -n rsync -a --chown=0:0 "$FW/lib/firmware/" "$DST_MNT/lib/firmware/"

echo ">> installing initramfs hooks"
sudo -n install -d "$DST_MNT/etc/initramfs-tools/hooks" \
                   "$DST_MNT/etc/initramfs-tools/scripts/local-top" \
                   "$DST_MNT/etc/initramfs-tools/scripts/init-bottom"
sudo -n install -m 0755 "$REPO/initramfs/hooks/pro1x-loop" \
  "$DST_MNT/etc/initramfs-tools/hooks/"
sudo -n install -m 0755 "$REPO/initramfs/local-top/pro1x-loop-root" \
  "$DST_MNT/etc/initramfs-tools/scripts/local-top/"
sudo -n install -m 0755 "$REPO/initramfs/init-bottom/pro1x-move-host" \
  "$REPO/initramfs/init-bottom/pro1x-overlay-root" \
  "$REPO/initramfs/init-bottom/pro1x-shared-modules" \
  "$DST_MNT/etc/initramfs-tools/scripts/init-bottom/"

echo ">> adjusting fstab (no separate /boot on this device)"
sudo -n sed -i 's|^/dev/mmcblk0p1 /boot|#&  # Pro1X: /boot lives in the rootfs image|' \
  "$DST_MNT/etc/fstab"
# The root's options as the initramfs mounts it (pro1x-loop-root: journal
# commits every 30 s, timestamps written lazily): checkroot.sh remounts /
# with the fstab's options, "defaults" without a line, which would drop
# lazytime again. Pass 0: the image is checked by nobody at boot.
grep -q '^/dev/loop0[[:space:]]' "$DST_MNT/etc/fstab" ||
  echo '/dev/loop0 / ext4 rw,lazytime,commit=30 0 0' | sudo -n tee -a "$DST_MNT/etc/fstab" > /dev/null

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
# a slave, so that unmounting dev/pts here leaves the PC's /dev/pts mounted
# (as a peer of the PC's /dev it took it along)
sudo -n mount --make-rslave "$DST_MNT/dev"
sudo -n mount --bind /dev/pts "$DST_MNT/dev/pts"
guard_on

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
  # the releases' packages, where given, for the steps that take DEBS=
  P=/root/pro1x-debs
  sudo -n rm -rf "$DST_MNT$P"
  for d in hidpi:"$HIDPI_DEBS" mobile-data:"$MOBILE_DATA_DEBS"; do
    [ -z "${d#*:}" ] || { sudo -n mkdir -p "$DST_MNT$P/${d%%:*}"; sudo -n cp "${d#*:}"/*.deb "$DST_MNT$P/${d%%:*}/"; }
  done
  # and in SRC, where 49-hidpi.sh's build leaves them: the later steps
  # take their -dev packages from there (libgtk2.0-dev for 49-alarmboot.sh)
  if [ -n "$HIDPI_DEBS" ]; then
    sudo -n mkdir -p "$DST_MNT/root/src"
    sudo -n cp "$HIDPI_DEBS"/*.deb "$DST_MNT/root/src/"
  fi
  for s in 45-pro1x-hardware 46-sensor-stack 47-libcal 48-camera-stack 49-apps 49-hidpi 49-standby 49-alarmboot 49-mobile-data; do
    debs=
    case $s in
      49-hidpi) [ -z "$HIDPI_DEBS" ] || debs="DEBS=$P/hidpi" ;;
      49-mobile-data) [ -z "$MOBILE_DATA_DEBS" ] || debs="DEBS=$P/mobile-data" ;;
    esac
    in_target "REPO=/mnt/pro1x-repo NIXOS=/mnt/nixos-pro1x SRC=/root/src $debs bash /mnt/pro1x-repo/scripts/$s.sh"
  done
  sudo -n rm -rf "$DST_MNT$P"
  # this repository's packages that no step builds (pro1x-events,
  # pro1x-volume-osd, the pro1x-look package) and newer releases of the
  # rest: as on the phone (docs/updating.md); held like 49-hidpi.sh's
  if [ "$UPDATE" = 1 ]; then
    echo ">> pro1x-update (the releases' packages)"
    in_target "/usr/local/sbin/pro1x-update --yes && rm -rf /var/cache/pro1x-update"
  fi
  sudo -n umount "$DST_MNT/mnt/pro1x-repo" "$DST_MNT/mnt/nixos-pro1x"
fi
guard_off

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

sudo -n cp "$DST_MNT/boot/initrd.img-$KVER" "$KOUT/initrd.img-$KVER"
sudo -n chown "$(id -u):$(id -g)" "$KOUT/initrd.img-$KVER"
sudo -n sh -c "df -h $DST_MNT | tail -1"

cleanup
trap - EXIT
echo "done:"
ls -la "$IMG" "$KOUT/initrd.img-$KVER"
du -h --apparent-size "$IMG" | cut -f1 | sed 's/^/  apparent: /'
du -h "$IMG" | cut -f1 | sed 's/^/  used:     /'
