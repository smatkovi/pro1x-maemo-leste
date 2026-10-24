#!/bin/sh
# Sichert alles, was ein Dualboot-Experiment am Pro1X kaputtmachen koennte.
# Rein lesend. Auf dem Geraet als root ausfuehren.
set -e
B=${1:-/userdata/dualboot-backup}
mkdir -p "$B"

for p in boot_a boot_b dtbo_a dtbo_b vbmeta_a vbmeta_b misc devinfo frp keystore; do
  echo "dump $p"
  dd if=/dev/disk/by-partlabel/$p of="$B/$p.img" bs=1M 2>/dev/null
done

# GPT jeder UFS-LUN: primaer (LBA 0-33) und Backup (letzte 33 LBAs).
# Achtung: UFS hat hier 4-KiB-Sektoren, /sys/block/*/size zaehlt aber immer in
# 512-Byte-Einheiten. Mit bs=512 waeren die Partitionseintraege abgeschnitten.
for d in /sys/block/sd?; do
  dev=$(basename "$d")
  lbs=$(cat "$d/queue/logical_block_size")
  lbas=$(( $(cat "$d/size") * 512 / lbs ))
  echo "dump gpt $dev (lbs=$lbs)"
  dd if=/dev/$dev of="$B/gpt-$dev-primary.bin" bs="$lbs" count=34 2>/dev/null
  dd if=/dev/$dev of="$B/gpt-$dev-backup.bin" bs="$lbs" count=33 skip=$((lbas-33)) 2>/dev/null
done

# Pruefsummen relativ ablegen, damit sie auch nach dem Spiegeln pruefbar sind
( cd "$B" && sha256sum ./*.img ./*.bin > SHA256SUMS )
cat /proc/cmdline > "$B/cmdline-slot-a.txt"
uname -a > "$B/uname.txt"
date -u > "$B/backup-date.txt"
echo "fertig: $B ($(du -sh "$B" | cut -f1))"
echo "Jetzt vom Geraet wegspiegeln, z.B.:"
echo "  rsync -a $B/ user@host:/pfad/backup-slot-a/"
