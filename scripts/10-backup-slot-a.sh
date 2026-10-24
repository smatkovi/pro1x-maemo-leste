#!/bin/sh
# Backs up everything a dual-boot experiment on the Pro1X could break.
# Read-only. Run on the device as root.
set -e
B=${1:-/userdata/dualboot-backup}
mkdir -p "$B"

for p in boot_a boot_b dtbo_a dtbo_b vbmeta_a vbmeta_b misc devinfo frp keystore; do
  echo "dump $p"
  dd if=/dev/disk/by-partlabel/$p of="$B/$p.img" bs=1M 2>/dev/null
done

# GPT of every UFS LUN: primary (LBA 0-33) and backup (last 33 LBAs).
# Careful: UFS has 4 KiB sectors here, but /sys/block/*/size always counts in
# 512-byte units. With bs=512 the partition entries would be cut off.
for d in /sys/block/sd?; do
  dev=$(basename "$d")
  lbs=$(cat "$d/queue/logical_block_size")
  lbas=$(( $(cat "$d/size") * 512 / lbs ))
  echo "dump gpt $dev (lbs=$lbs)"
  dd if=/dev/$dev of="$B/gpt-$dev-primary.bin" bs="$lbs" count=34 2>/dev/null
  dd if=/dev/$dev of="$B/gpt-$dev-backup.bin" bs="$lbs" count=33 skip=$((lbas-33)) 2>/dev/null
done

# Store checksums with relative paths, so they can still be checked after mirroring
( cd "$B" && sha256sum ./*.img ./*.bin > SHA256SUMS )
cat /proc/cmdline > "$B/cmdline-slot-a.txt"
uname -a > "$B/uname.txt"
date -u > "$B/backup-date.txt"
echo "done: $B ($(du -sh "$B" | cut -f1))"
echo "Now mirror it off the device, e.g.:"
echo "  rsync -a $B/ user@host:/path/backup-slot-a/"
