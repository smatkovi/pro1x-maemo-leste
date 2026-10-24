#!/bin/bash
# Builds the Android boot image for Maemo Leste (build host).
#
# The stock ABL of the Pro1X loads the mainline kernel straight from it -- no
# U-Boot in between. What it requires for that was narrowed down in slot B on
# 24.10.2026 (table in docs/HANDOFF.md):
#
# * Header v0 and the DTB **appended to Image.gz**. With an empty dtbo the ABL
#   only looks for an appended DTB ("Single appended DTB found"); it uses the
#   DTB section of header v2 only together with a valid dtbo (SoC DTB + board
#   overlay, that is how Sailfish boots). With v2 + DTB section the device
#   lands back in fastboot 5-9 s after the reboot.
# * Offsets as in Sailfish's boot_a (device/boot_a-params.env), otherwise ignored.
#
# Also applied, untested whether needed: qcom,board-id = <0 0> in the DTB (as
# in the Sailfish DTB), dtbo zeroed and vbmeta with verification disabled.
set -euo pipefail
WORK=${WORK:?set WORK=<build-dir>}
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
KVER=${KVER:-7.1.0+}
OUT=$WORK/out
# shellcheck disable=SC1090
. "$REPO/device/boot_a-params.env"

# userdata filesystem that holds the rootfs image, and the path inside it.
BACKING=${BACKING:-UUID=cffbd472-3eed-4cb3-b73e-d8088ac7fcdd}
IMAGE_PATH=${IMAGE_PATH:-/maemo-leste.img}

CMDLINE="root=/dev/loop0 rootfstype=ext4 rootwait rw"
CMDLINE="$CMDLINE pro1x.backing=$BACKING pro1x.image=$IMAGE_PATH"
# Kernel messages on the debug UART and the panel (tty1 comes last and thus
# becomes /dev/console). Deliberately NOT on ttyGS0, the ACM port of the g_cdc
# gadget: when nobody reads it on the PC, its buffer fills up and every
# console write blocks -- including init's, which then never changes runlevel
# (reboot and shutdown silently did nothing). A getty still runs on ttyGS0.
CMDLINE="$CMDLINE loglevel=4 earlycon=msm_geni_serial,0x4a90000"
CMDLINE="$CMDLINE console=ttyMSM0,115200n8 console=tty1"

for f in "$OUT/Image.gz" "$OUT/initrd.img-$KVER" "$OUT/sm6115-fxtec-pro1x.dtb"; do
  [ -f "$f" ] || { echo "missing: $f"; exit 1; }
done

# DTB with a board id, as the Sailfish DTB carries one; the ABL selects DTBs
# by qcom,msm-id + qcom,board-id. Upstream lacks the board id.
DTB=$OUT/sm6115-fxtec-pro1x-abl.dtb
cp "$OUT/sm6115-fxtec-pro1x.dtb" "$DTB"
fdtget "$DTB" / qcom,board-id >/dev/null 2>&1 || fdtput -t u "$DTB" / qcom,board-id 0 0
cat "$OUT/Image.gz" "$DTB" > "$OUT/Image.gz-dtb"

mkbootimg \
  --kernel "$OUT/Image.gz-dtb" \
  --ramdisk "$OUT/initrd.img-$KVER" \
  --cmdline "$CMDLINE" \
  --base "$BOOT_BASE" \
  --kernel_offset "$BOOT_KERNEL_OFFSET" \
  --ramdisk_offset "$BOOT_RAMDISK_OFFSET" \
  --second_offset "$BOOT_SECOND_OFFSET" \
  --tags_offset "$BOOT_TAGS_OFFSET" \
  --pagesize "$BOOT_PAGESIZE" \
  --header_version 0 \
  --output "$OUT/boot-leste.img"

sz=$(stat -c%s "$OUT/boot-leste.img")
max=$((BOOT_PARTITION_SIZE_MIB * 1024 * 1024))
echo "Payload: $sz bytes ($((sz / 1048576)) MiB) of $BOOT_PARTITION_SIZE_MIB MiB partition size"
# Room for vbmeta (512 B) + footer (64 B) has to remain
[ "$sz" -le "$((max - 65536))" ] || { echo "ERROR: image does not fit into boot_b"; exit 1; }

# Append the AVB hash footer, structurally identical to the working Sailfish
# boot_a (avbtool, algorithm NONE, sha256 hash descriptor for partition "boot").
avbtool add_hash_footer \
  --image "$OUT/boot-leste.img" \
  --partition_name boot \
  --partition_size "$max" \
  --algorithm NONE

echo "--- avbtool info_image ---"
avbtool info_image --image "$OUT/boot-leste.img" 2>&1 | head -18
final=$(stat -c%s "$OUT/boot-leste.img")
echo "boot-leste.img with footer: $final bytes (partition: $max)"
[ "$final" = "$max" ] || { echo "ERROR: unexpected final size"; exit 1; }

# vbmeta for the Leste slot: verification disabled (flags=2), 64 KiB.
python3 -I "$REPO/scripts/lib/mkvbmeta.py" "$OUT/vbmeta-disabled.img"
sha256sum "$OUT/boot-leste.img" "$OUT/vbmeta-disabled.img"
