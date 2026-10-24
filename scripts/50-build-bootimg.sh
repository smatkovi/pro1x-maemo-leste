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
# ttyGS0 ist die ACM-Konsole des eingebauten g_cdc-Gadgets -- ueber USB
# erreichbar, im Gegensatz zum Debug-UART. tty1 steht zuletzt und wird damit
# /dev/console, die Ausgabe bleibt also auf dem Panel.
CMDLINE="$CMDLINE console=ttyMSM0,115200n8 console=ttyGS0,115200 console=tty1"

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
echo "Payload: $sz Byte ($((sz / 1048576)) MiB) von $BOOT_PARTITION_SIZE_MIB MiB Partitionsgroesse"
# Platz fuer vbmeta (512 B) + Footer (64 B) muss uebrig bleiben
[ "$sz" -le "$((max - 65536))" ] || { echo "FEHLER: Image passt nicht in boot_b"; exit 1; }

# AVB-Hash-Footer anhaengen. Ohne ihn weist der stock-ABL des Pro1X das Image
# ab: Slot B verbrannte alle sechs Retries und wurde unbootable markiert.
# Das funktionierende Sailfish-boot_a traegt genau diese Struktur -- avbtool
# 1.1.0, algorithm NONE (unsigniert), sha256-Hash-Deskriptor fuer Partition
# "boot", vbmeta direkt hinter dem Payload, Footer bei Partitionsgroesse-64.
avbtool add_hash_footer \
  --image "$OUT/boot-leste.img" \
  --partition_name boot \
  --partition_size "$max" \
  --algorithm NONE

echo "--- avbtool info_image ---"
avbtool info_image --image "$OUT/boot-leste.img" 2>&1 | head -18
final=$(stat -c%s "$OUT/boot-leste.img")
echo "boot-leste.img mit Footer: $final Byte (Partition: $max)"
[ "$final" = "$max" ] || { echo "FEHLER: unerwartete Endgroesse"; exit 1; }
