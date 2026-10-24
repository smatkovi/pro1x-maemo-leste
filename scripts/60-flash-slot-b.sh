#!/bin/sh
# Writes the Maemo Leste boot image into the INACTIVE boot slot.
# Runs on the device as root. Sailfish OS in slot A stays untouched.
#
# Writing action -- requires the word "flash" as its argument.
# A backup of the target slot has to exist first (scripts/10-backup-slot-a.sh).
#
# Runs under Sailfish (userdata under /userdata) and under Leste itself
# (userdata under /host, then set TARGET=b -- that is the update path for a
# new Leste boot image; the running kernel does not get in the way).
set -e
DATA=${DATA:-$( [ -d /userdata/dualboot-backup ] && echo /userdata || echo /host )}
IMG=${IMG:-$DATA/boot-leste.img}
VBMETA=${VBMETA:-$DATA/vbmeta-disabled.img}  # leave empty: vbmeta unchanged
ERASE_DTBO=${ERASE_DTBO:-1}   # mainline brings its DTB inside the boot image;
                              # foreign overlays from the target slot's dtbo get in the way.

[ "$1" = flash ] || {
  echo "Usage: $0 flash"
  echo "Writes $IMG into the inactive boot slot."
  exit 1
}
[ -f "$IMG" ] || { echo "missing: $IMG"; exit 1; }
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

# Read the active slot from the kernel cmdline, the target is the other one.
# Under Leste slot_suffix is missing from the cmdline (the ABL does not append
# it for a v0 image) -- then the target has to be set as TARGET=a|b.
ACTIVE=$(sed -n 's/.*androidboot\.slot_suffix=\(_[ab]\).*/\1/p' /proc/cmdline)
case "${TARGET:-}:$ACTIVE" in
  :_a) TARGET=b ;;
  :_b) TARGET=a ;;
  a:_a|b:_b) echo "TARGET=$TARGET is the running slot -- aborted"; exit 1 ;;
  a:*|b:*) ;;
  *)  echo "active slot not recognised (slot_suffix='$ACTIVE'), set TARGET=a|b"; exit 1 ;;
esac
ACTIVE=${ACTIVE:-unknown}
BOOT=/dev/disk/by-partlabel/boot_$TARGET
DTBO=/dev/disk/by-partlabel/dtbo_$TARGET
[ -b "$BOOT" ] || { echo "missing: $BOOT"; exit 1; }

# Check size and format before anything is written.
psz=$(( $(cat "/sys/class/block/$(basename "$(readlink -f "$BOOT")")/size") * 512 ))
isz=$(stat -c%s "$IMG")
[ "$isz" -le "$psz" ] || { echo "image ($isz bytes) larger than partition ($psz bytes)"; exit 1; }
head -c 8 "$IMG" | grep -q 'ANDROID!' || { echo "$IMG is not an Android boot image"; exit 1; }

echo "active slot: $ACTIVE  ->  target: slot _$TARGET"
echo "  $IMG ($isz bytes) -> $BOOT"
[ "$ERASE_DTBO" = 1 ] && echo "  dtbo_$TARGET will be zeroed"

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
  { echo "ERROR: boot_$TARGET differs from the image after writing"; exit 1; }
echo "boot_$TARGET verified."
echo "done. Switch from the PC:  fastboot --set-active=$TARGET"
echo "Way back:                  fastboot --set-active=${ACTIVE#_}"
