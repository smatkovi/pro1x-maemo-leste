#!/bin/bash
# Baut das Android-Boot-Image fuer Maemo Leste (Build-Host).
#
# Header v2 mit Kernel + Initramfs + DTB, Offsets exakt wie im vorhandenen
# boot_a des Sailfish-Images (device/boot_a-params.env). Der stock-ABL des
# Pro1X laedt daraus direkt den Mainline-Kernel -- kein U-Boot dazwischen.
set -euo pipefail
WORK=${WORK:?WORK=<build-verzeichnis> setzen}
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
KVER=${KVER:-7.1.0+}
OUT=$WORK/out
# shellcheck disable=SC1090
. "$REPO/device/boot_a-params.env"

# userdata-Dateisystem, auf dem das Rootfs-Image liegt, und der Pfad darin.
BACKING=${BACKING:-UUID=cffbd472-3eed-4cb3-b73e-d8088ac7fcdd}
IMAGE_PATH=${IMAGE_PATH:-/maemo-leste.img}

CMDLINE="root=/dev/loop0 rootfstype=ext4 rootwait rw"
CMDLINE="$CMDLINE pro1x.backing=$BACKING pro1x.image=$IMAGE_PATH"
# Bringup-Phase: gespraechige Konsole auf Debug-UART und Panel.
CMDLINE="$CMDLINE loglevel=7 ignore_loglevel earlycon=msm_geni_serial,0x4a90000"
CMDLINE="$CMDLINE console=ttyMSM0,115200n8 console=tty1"

for f in "$OUT/Image.gz" "$OUT/initrd.img-$KVER" "$OUT/sm6115-fxtec-pro1x.dtb"; do
  [ -f "$f" ] || { echo "fehlt: $f"; exit 1; }
done

mkbootimg \
  --kernel "$OUT/Image.gz" \
  --ramdisk "$OUT/initrd.img-$KVER" \
  --dtb "$OUT/sm6115-fxtec-pro1x.dtb" \
  --cmdline "$CMDLINE" \
  --base "$BOOT_BASE" \
  --kernel_offset "$BOOT_KERNEL_OFFSET" \
  --ramdisk_offset "$BOOT_RAMDISK_OFFSET" \
  --second_offset "$BOOT_SECOND_OFFSET" \
  --tags_offset "$BOOT_TAGS_OFFSET" \
  --dtb_offset "$BOOT_DTB_OFFSET" \
  --pagesize "$BOOT_PAGESIZE" \
  --header_version "$BOOT_HEADER_VERSION" \
  --output "$OUT/boot-leste.img"

sz=$(stat -c%s "$OUT/boot-leste.img")
max=$((BOOT_PARTITION_SIZE_MIB * 1024 * 1024))
echo "boot-leste.img: $sz Byte ($((sz / 1048576)) MiB) von $BOOT_PARTITION_SIZE_MIB MiB Partitionsgroesse"
[ "$sz" -le "$max" ] || { echo "FEHLER: Image passt nicht in boot_b"; exit 1; }
python3 "$REPO/scripts/lib/bootimg-params.py" "$OUT/boot-leste.img"
