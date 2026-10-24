#!/bin/sh
# Schreibt das Maemo-Leste-Boot-Image in den INAKTIVEN Boot-Slot.
# Laeuft auf dem Geraet als root. Sailfish OS in Slot A bleibt unangetastet.
#
# Schreibende Aktion -- verlangt als Argument das Wort "flash".
# Vorher muss ein Backup des Zielslots existieren (scripts/10-backup-slot-a.sh).
#
# Laeuft unter Sailfish (userdata unter /userdata) und unter Leste selbst
# (userdata unter /host, dann TARGET=b setzen -- das ist der Update-Weg fuer
# ein neues Leste-Boot-Image, der laufende Kernel stoert dabei nicht).
set -e
DATA=${DATA:-$( [ -d /userdata/dualboot-backup ] && echo /userdata || echo /host )}
IMG=${IMG:-$DATA/boot-leste.img}
VBMETA=${VBMETA:-$DATA/vbmeta-disabled.img}  # leer lassen: vbmeta unveraendert
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
# Unter Leste fehlt slot_suffix in der Cmdline (der ABL haengt ihn an ein
# v0-Image nicht an) -- dann muss das Ziel als TARGET=a|b gesetzt sein.
ACTIVE=$(sed -n 's/.*androidboot\.slot_suffix=\(_[ab]\).*/\1/p' /proc/cmdline)
case "${TARGET:-}:$ACTIVE" in
  :_a) TARGET=b ;;
  :_b) TARGET=a ;;
  a:_a|b:_b) echo "TARGET=$TARGET ist der laufende Slot -- abgebrochen"; exit 1 ;;
  a:*|b:*) ;;
  *)  echo "aktiven Slot nicht erkannt (slot_suffix='$ACTIVE'), TARGET=a|b setzen"; exit 1 ;;
esac
ACTIVE=${ACTIVE:-unbekannt}
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
if [ -n "$VBMETA" ] && [ -f "$VBMETA" ]; then
  echo "  $VBMETA -> vbmeta_$TARGET"
  dd if="$VBMETA" of="/dev/disk/by-partlabel/vbmeta_$TARGET" bs=64k conv=fsync
fi
sync
echo 3 > /proc/sys/vm/drop_caches
[ "$(sha256sum < "$BOOT" | cut -c1-64)" = "$(sha256sum < "$IMG" | cut -c1-64)" ] ||
  { echo "FEHLER: boot_$TARGET weicht nach dem Schreiben vom Image ab"; exit 1; }
echo "boot_$TARGET geprueft."
echo "fertig. Umschalten vom PC aus:  fastboot --set-active=$TARGET"
echo "Rueckweg:                       fastboot --set-active=${ACTIVE#_}"
