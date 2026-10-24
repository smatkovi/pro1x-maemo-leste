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

# GPT jeder UFS-LUN: primaer (Sektor 0-33) und Backup (letzte 33 Sektoren)
for d in /sys/block/sd?; do
  dev=$(basename "$d"); sz=$(cat "$d/size")
  echo "dump gpt $dev"
  dd if=/dev/$dev of="$B/gpt-$dev-primary.bin" bs=512 count=34 2>/dev/null
  dd if=/dev/$dev of="$B/gpt-$dev-backup.bin" bs=512 count=33 skip=$((sz-33)) 2>/dev/null
done

# Pruefsummen relativ ablegen, damit sie auch nach dem Spiegeln pruefbar sind
( cd "$B" && sha256sum ./*.img ./*.bin > SHA256SUMS )
cat /proc/cmdline > "$B/cmdline-slot-a.txt"
uname -a > "$B/uname.txt"
date -u > "$B/backup-date.txt"
echo "fertig: $B ($(du -sh "$B" | cut -f1))"
echo "Jetzt vom Geraet wegspiegeln, z.B.:"
echo "  rsync -a $B/ user@host:/pfad/backup-slot-a/"
