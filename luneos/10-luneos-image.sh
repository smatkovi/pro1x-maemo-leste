#!/bin/bash
# LuneOS rootfs image for the Pro1X boot menu (/host/luneos.img).
#
# Runs as root on the phone, in Maemo Leste. LuneOS publishes no generic
# arm64 root filesystem, but its PinePhone image is plain aarch64 userland
# for a mainline kernel, with a Mesa that includes freedreno, the Pro1X's GPU
# driver. Its root partition becomes the image, which the initramfs
# (pro1x-loop-root) mounts with Leste's kernel like Kali's;
# 20-pro1x-hardware.sh adds the Pro1X's parts.
#
#   bash luneos/10-luneos-image.sh        # IMG=/host/luneos.img SIZE=8G
#
# The download (1.6 GB) is kept in $DL and checked against its SHA-256.
set -euo pipefail
IMG=${IMG:-/host/luneos.img}
SIZE=${SIZE:-8G}
DL=${DL:-/host/luneos-dl}
RELEASE=${RELEASE:-20260911-LuneOS-Testing-Builds}
FILE=${FILE:-luneos-dev-image-pinephone.rootfs-wrynose-20260924170559.wic.gz}
SHA256=${SHA256:-880cc10b4e402fc35d040c2ab773e279dbd5e34f11ada083bf99a5b89eb10f3e}
URL=https://github.com/webOS-ports/luneos-testing/releases/download/$RELEASE/$FILE
PART_NAME=luneos-rootfs

[ -e "$IMG" ] && { echo "$IMG exists"; exit 1; }
log() { echo ">> $*"; }
mkdir -p "$DL"
W=$DL/luneos-pinephone.wic.gz
if [ ! -f "$W" ]; then
	log "downloading $FILE"
	curl -sSfL --retry 5 -o "$W.part" "$URL"
	mv "$W.part" "$W"
fi
echo "$SHA256  $W" | sha256sum -c --quiet -

# The partition table is in the first sectors; reading stops early on purpose.
log "partition $PART_NAME"
(gunzip -c "$W" 2>/dev/null || true) | head -c 4194304 > "$DL/gpt.bin"
nr=$(sgdisk -p "$DL/gpt.bin" 2>/dev/null | awk -v n="$PART_NAME" '$NF == n { print $1 }')
[ -n "$nr" ] || { echo "no partition $PART_NAME"; exit 1; }
first=$(sgdisk -i "$nr" "$DL/gpt.bin" 2>/dev/null | awk '/^First sector:/ { print $3 }')
last=$(sgdisk -i "$nr" "$DL/gpt.bin" 2>/dev/null | awk '/^Last sector:/ { print $3 }')
rm -f "$DL/gpt.bin"

log "extracting sectors $first-$last into $IMG"
(gunzip -c "$W" 2>/dev/null || true) |
	dd of="$IMG" bs=4M iflag=skip_bytes,count_bytes,fullblock status=none \
	   skip=$((first * 512)) count=$(((last - first + 1) * 512))
e2fsck -fy "$IMG" > /dev/null
log "growing to $SIZE (room for Leste's kernel modules and firmware)"
truncate -s "$SIZE" "$IMG"
resize2fs -p "$IMG" > /dev/null
e2fsck -fn "$IMG" | tail -1
log "$IMG done: luneos/20-pro1x-hardware.sh next"
