#!/bin/bash
# Baut das Maemo-Leste-Rootfs-Image fuer den Pro1X (Build-Host, braucht sudo).
#
# Nimmt das offizielle arm64-generic-Image als Basis, legt den selbst gebauten
# 7.1-Kernel samt Modulen und die Pro1X-Firmware hinein, verdrahtet das
# Loop-Root-Initramfs und zieht initrd + vmlinuz nach $WORK/out heraus.
set -euo pipefail
WORK=${WORK:?WORK=<build-verzeichnis> setzen}
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
SRC_IMG=${SRC_IMG:-$WORK/dl/leste.img}
IMG=${IMG:-$WORK/out/maemo-leste.img}
IMG_SIZE=${IMG_SIZE:-24G}
KVER=${KVER:-7.1.0+}
ROOT_PW=${ROOT_PW:-pro1x-leste}
SSH_PUBKEY=${SSH_PUBKEY:-$HOME/.ssh/id_ed25519.pub}

SRC_MNT=/mnt/leste-src
DST_MNT=/mnt/leste-dst

# Das auf Arch registrierte binfmt zeigt auf das dynamisch gelinkte
# qemu-aarch64, dessen Host-Bibliotheken im Chroot fehlen. Wir registrieren
# darum fuer die Dauer des Builds die statische Variante und stellen danach
# den Originalzustand wieder her.
BINFMT_SWITCHED=0
AARCH64_MAGIC='\x7f\x45\x4c\x46\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00'
AARCH64_MASK='\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff'

binfmt_use_static() {
  [ -x /usr/bin/qemu-aarch64-static ] || { echo "qemu-aarch64-static fehlt"; exit 1; }
  if [ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] &&
     grep -q 'interpreter /usr/bin/qemu-aarch64-static' /proc/sys/fs/binfmt_misc/qemu-aarch64; then
    return 0   # schon statisch, nichts zu tun
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

# In der Zielumgebung ausfuehren; PATH explizit, weil das Chroot den Host-PATH erbt.
in_target() {
  sudo -n chroot "$DST_MNT" /usr/bin/env \
    PATH=/usr/sbin:/usr/bin:/sbin:/bin DEBIAN_FRONTEND=noninteractive \
    /bin/bash -c "$1"
}

cleanup() {
  set +e
  binfmt_restore
  for m in "$DST_MNT/dev/pts" "$DST_MNT/dev" "$DST_MNT/proc" "$DST_MNT/sys" "$DST_MNT" "$SRC_MNT"; do
    mountpoint -q "$m" && sudo -n umount -l "$m"
  done
}
trap cleanup EXIT

[ -f "$WORK/out/Image.gz" ] || { echo "Kernel fehlt, erst 30-build-kernel.sh"; exit 1; }
[ -d "$WORK/out/modules/lib/modules/$KVER" ] || { echo "Module fuer $KVER fehlen"; exit 1; }

# Firmware-Blobs (proprietaer, aus dem sm6115-mainline-Projekt)
FW=$WORK/build/firmware-fxtec-qx1050
[ -d "$FW" ] || git clone -q --depth 1 \
  https://github.com/sm6115-mainline/firmware-fxtec-qx1050.git "$FW"

echo ">> leeres Image anlegen ($IMG_SIZE, sparse)"
mkdir -p "$(dirname "$IMG")"
rm -f "$IMG"
truncate -s "$IMG_SIZE" "$IMG"
mkfs.ext4 -q -L maemo-leste -E lazy_itable_init=0,lazy_journal_init=0 "$IMG"

sudo -n mkdir -p "$SRC_MNT" "$DST_MNT"
sudo -n mount -o ro,loop,offset=$((264192 * 512)) "$SRC_IMG" "$SRC_MNT"
sudo -n mount -o loop "$IMG" "$DST_MNT"

echo ">> Rootfs kopieren"
sudo -n rsync -aHAX --numeric-ids "$SRC_MNT/" "$DST_MNT/"

echo ">> Kernel, Module, Firmware einsetzen"
sudo -n cp "$WORK/out/Image.gz" "$DST_MNT/boot/vmlinuz-$KVER"
sudo -n cp "$WORK/build/linux/.config" "$DST_MNT/boot/config-$KVER"
sudo -n rsync -a "$WORK/out/modules/lib/modules/$KVER" "$DST_MNT/lib/modules/"
sudo -n rsync -a "$FW/lib/firmware/" "$DST_MNT/lib/firmware/"

echo ">> Initramfs-Hooks installieren"
sudo -n install -d "$DST_MNT/etc/initramfs-tools/hooks" \
                   "$DST_MNT/etc/initramfs-tools/scripts/local-top" \
                   "$DST_MNT/etc/initramfs-tools/scripts/init-bottom"
sudo -n install -m 0755 "$REPO/initramfs/hooks/pro1x-loop" \
  "$DST_MNT/etc/initramfs-tools/hooks/"
sudo -n install -m 0755 "$REPO/initramfs/local-top/pro1x-loop-root" \
  "$DST_MNT/etc/initramfs-tools/scripts/local-top/"
sudo -n install -m 0755 "$REPO/initramfs/init-bottom/pro1x-move-host" \
  "$DST_MNT/etc/initramfs-tools/scripts/init-bottom/"

echo ">> fstab anpassen (kein eigenes /boot auf diesem Geraet)"
sudo -n sed -i 's|^/dev/mmcblk0p1 /boot|#&  # Pro1X: /boot liegt im Rootfs-Image|' \
  "$DST_MNT/etc/fstab"

echo ">> Zugang einrichten"
binfmt_use_static
in_target "echo 'root:$ROOT_PW' | chpasswd"
if [ -f "$SSH_PUBKEY" ]; then
  sudo -n install -d -m 0700 "$DST_MNT/root/.ssh"
  sudo -n tee "$DST_MNT/root/.ssh/authorized_keys" < "$SSH_PUBKEY" >/dev/null
  sudo -n chmod 0600 "$DST_MNT/root/.ssh/authorized_keys"
fi
# Serielle Konsole fuer die Bringup-Phase (Pro1X: ttyMSM0 ueber USB-Debug)
grep -q ttyMSM0 "$DST_MNT/etc/inittab" || \
  echo 'T0:23:respawn:/sbin/agetty -L 115200 ttyMSM0 vt100' | \
  sudo -n tee -a "$DST_MNT/etc/inittab" >/dev/null

echo ">> Initramfs bauen (chroot via qemu-aarch64/binfmt)"
sudo -n mount -t proc proc "$DST_MNT/proc"
sudo -n mount -t sysfs sys "$DST_MNT/sys"
sudo -n mount --bind /dev "$DST_MNT/dev"
sudo -n mount --bind /dev/pts "$DST_MNT/dev/pts"
in_target "depmod $KVER && update-initramfs -c -k $KVER"

sudo -n cp "$DST_MNT/boot/initrd.img-$KVER" "$WORK/out/initrd.img-$KVER"
sudo -n chown "$(id -u):$(id -g)" "$WORK/out/initrd.img-$KVER"
sudo -n sh -c "df -h $DST_MNT | tail -1"

cleanup
trap - EXIT
echo "fertig:"
ls -la "$IMG" "$WORK/out/initrd.img-$KVER"
du -h --apparent-size "$IMG" | cut -f1 | sed 's/^/  apparent: /'
du -h "$IMG" | cut -f1 | sed 's/^/  belegt:   /'
