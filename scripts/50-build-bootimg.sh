#!/bin/bash
# Baut das Android-Boot-Image fuer Maemo Leste (Build-Host).
#
# Der stock-ABL des Pro1X laedt daraus direkt den Mainline-Kernel -- kein
# U-Boot dazwischen. Was er dafuer verlangt, wurde am 24.10.2026 in Slot B
# eingegrenzt (Tabelle in docs/UEBERGABE-ARCHBANG.md):
#
# * Header v0 und das DTB **an Image.gz angehaengt**. Bei leerem dtbo sucht der
#   ABL ausschliesslich ein angehaengtes DTB ("Single appended DTB found"); die
#   DTB-Sektion von Header v2 nutzt er nur zusammen mit einem gueltigen dtbo
#   (SoC-DTB + Board-Overlay, so bootet Sailfish). Mit v2 + DTB-Sektion landet
#   das Geraet 5-9 s nach dem Reboot wieder im Fastboot.
# * Offsets wie im Sailfish-boot_a (device/boot_a-params.env), sonst ignoriert.
#
# Nebenbei, ungetestet ob noetig: qcom,board-id = <0 0> im DTB (wie im
# Sailfish-DTB), dtbo genullt und vbmeta mit abgeschalteter Verifikation.
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

# DTB mit Board-ID, wie sie das Sailfish-DTB traegt; der ABL waehlt DTBs ueber
# qcom,msm-id + qcom,board-id aus. Upstream fehlt die Board-ID.
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
echo "Payload: $sz Byte ($((sz / 1048576)) MiB) von $BOOT_PARTITION_SIZE_MIB MiB Partitionsgroesse"
# Platz fuer vbmeta (512 B) + Footer (64 B) muss uebrig bleiben
[ "$sz" -le "$((max - 65536))" ] || { echo "FEHLER: Image passt nicht in boot_b"; exit 1; }

# AVB-Hash-Footer anhaengen, strukturgleich zum funktionierenden Sailfish-boot_a
# (avbtool, algorithm NONE, sha256-Hash-Deskriptor fuer Partition "boot").
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

# vbmeta fuer den Leste-Slot: Verifikation abgeschaltet (flags=2), 64 KiB.
python3 -I "$REPO/scripts/lib/mkvbmeta.py" "$OUT/vbmeta-disabled.img"
sha256sum "$OUT/boot-leste.img" "$OUT/vbmeta-disabled.img"
