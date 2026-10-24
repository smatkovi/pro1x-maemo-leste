#!/bin/sh
# Schreibt das Maemo-Leste-Boot-Image in den INAKTIVEN Boot-Slot.
# Laeuft auf dem Geraet als root. Sailfish OS in Slot A bleibt unangetastet.
#
# Schreibende Aktion -- verlangt als Argument das Wort "flash".
# Vorher muss ein Backup des Zielslots existieren (scripts/10-backup-slot-a.sh).
set -e
IMG=${IMG:-/userdata/boot-leste.img}
ERASE_DTBO=${ERASE_DTBO:-1}   # Mainline bringt seinen DTB im Boot-Image mit;
                              # fremde Overlays aus dtbo des Zielslots stoeren.

[ "$1" = flash ] || {
  echo "Nutzung: $0 flash"
  echo "Schreibt $IMG in den inaktiven Boot-Slot."
  exit 1
}
[ -f "$IMG" ] || { echo "fehlt: $IMG"; exit 1; }
[ "$(id -u)" = 0 ] || { echo "als root ausfuehren"; exit 1; }

# Aktiven Slot aus der Kernel-Cmdline lesen, Ziel ist der jeweils andere.
ACTIVE=$(sed -n 's/.*androidboot\.slot_suffix=\(_[ab]\).*/\1/p' /proc/cmdline)
case "$ACTIVE" in
  _a) TARGET=b ;;
  _b) TARGET=a ;;
  *)  echo "aktiven Slot nicht erkannt (slot_suffix='$ACTIVE')"; exit 1 ;;
esac
BOOT=/dev/disk/by-partlabel/boot_$TARGET
DTBO=/dev/disk/by-partlabel/dtbo_$TARGET
[ -b "$BOOT" ] || { echo "fehlt: $BOOT"; exit 1; }

# Groessen- und Formatpruefung, bevor irgendwas geschrieben wird.
psz=$(( $(cat "/sys/class/block/$(basename "$(readlink -f "$BOOT")")/size") * 512 ))
isz=$(stat -c%s "$IMG")
[ "$isz" -le "$psz" ] || { echo "Image ($isz Byte) groesser als Partition ($psz Byte)"; exit 1; }
head -c 8 "$IMG" | grep -q 'ANDROID!' || { echo "$IMG ist kein Android-Boot-Image"; exit 1; }

echo "aktiver Slot: $ACTIVE  ->  Ziel: Slot _$TARGET"
echo "  $IMG ($isz Byte) -> $BOOT"
[ "$ERASE_DTBO" = 1 ] && echo "  dtbo_$TARGET wird genullt"

dd if="$IMG" of="$BOOT" bs=1M conv=fsync
if [ "$ERASE_DTBO" = 1 ]; then
  dd if=/dev/zero of="$DTBO" bs=1M conv=fsync 2>/dev/null || true
fi
sync
echo "fertig. Umschalten vom PC aus:  fastboot --set-active=$TARGET"
echo "Rueckweg:                       fastboot --set-active=${ACTIVE#_}"
