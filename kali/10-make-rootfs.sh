#!/bin/bash
# Kali Linux rootfs image for the Pro1X boot menu (/host/kali.img).
#
# Runs as root on the phone, in Maemo Leste (native arm64, no emulation):
# debootstrap of kali-rolling into an ext4 image, which the initramfs
# (pro1x-loop-root) mounts as root like Leste's. The Pro1X hardware setup
# follows in 20-pro1x-hardware.sh, inside the image.
#
#   bash kali/10-make-rootfs.sh            # IMG=/host/kali.img SIZE=16G
set -euo pipefail
IMG=${IMG:-/host/kali.img}
SIZE=${SIZE:-16G}
MNT=${MNT:-/mnt/kali}
MIRROR=${MIRROR:-http://http.kali.org/kali}
KEYRING_DEB=kali-archive-keyring_2025.3_all.deb
export DEBIAN_FRONTEND=noninteractive

[ -e "$IMG" ] && { echo "$IMG exists"; exit 1; }

apt-get install -y -q --no-install-recommends debootstrap e2fsprogs curl binutils xz-utils > /dev/null

# Kali's archive key, from its own package
K=$(mktemp -d)
curl -sfL -o "$K/keyring.deb" "$MIRROR/pool/main/k/kali-archive-keyring/$KEYRING_DEB"
(cd "$K" && ar x keyring.deb && tar -xf data.tar.*)
KEYRING=$K/usr/share/keyrings/kali-archive-keyring.gpg

truncate -s "$SIZE" "$IMG"
mkfs.ext4 -q -L kali "$IMG"
mkdir -p "$MNT"
mount -o loop "$IMG" "$MNT"

debootstrap --arch=arm64 --keyring="$KEYRING" \
	--include=ca-certificates,locales,sudo,openssh-server,systemd-sysv,dbus \
	kali-rolling "$MNT" "$MIRROR"
install -D -m 644 "$KEYRING" "$MNT/usr/share/keyrings/kali-archive-keyring.gpg"
echo "deb $MIRROR kali-rolling main contrib non-free non-free-firmware" > "$MNT/etc/apt/sources.list"
rm -rf "$K"
echo ">> debootstrap done: $IMG mounted on $MNT"
