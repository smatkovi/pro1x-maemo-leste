#!/bin/bash
# Whether an initramfs carries what arms a kernel's trial start
# (docs/device-day-2026-10-31.md, "Rollback"), on the PC, before the flash:
#
#   bash tools/devday/check-initrd.sh $OUT/initrd.img-$KVER [alarm]
#
# The boot menu (local-top/pro1x-loop-root with trial-check, pro1x-boot
# with trial and trial-check, pro1x-bootmenu, sgdisk for the slot bits),
# kexec (FreeBSD, KolibriOS), the watchdog's module (qcom-wdt.ko) and
# busybox's devmem (the initramfs arms the APSS watchdog with it; both
# are skipped silently when missing, so a trial would run without a
# watchdog). "alarm": also the alarm power-on's alarm-check (pro1x.10).
# Reads the image only (xz, zstd or gzip, one cpio).
set -uo pipefail
img=${1:?usage: check-initrd.sh INITRD [alarm]}
case $(file -b "$img") in
XZ*) cat=(xz -dc) ;;
Zstandard*) cat=(zstd -dc) ;;
gzip*) cat=(gzip -dc) ;;
*) echo "unknown compression: $(file -b "$img")"; exit 1 ;;
esac
tmp=$(mktemp -d /tmp/pro1x-check-initrd.XXXXXX)
trap 'rm -f "$tmp"/list "$tmp"/f; rmdir "$tmp"' EXIT
"${cat[@]}" "$img" | cpio -t --quiet > "$tmp/list" 2> /dev/null
# member FILE: FILE's content from the image into $tmp/f
member() { "${cat[@]}" "$img" | cpio -i --quiet --to-stdout "$1" > "$tmp/f" 2> /dev/null; }
fails=0
res() { if [ "$2" = 0 ]; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
has() { grep -qx "$1" "$tmp/list"; }

for f in scripts/local-top/pro1x-loop-root usr/sbin/pro1x-boot usr/sbin/pro1x-bootmenu \
    usr/sbin/sgdisk usr/sbin/kexec; do
	has "$f"; res "$f" $?
done
grep -q '/qcom-wdt\.ko$' "$tmp/list"; res "qcom-wdt.ko" $?
member scripts/local-top/pro1x-loop-root
grep -q 'trial-check' "$tmp/f" && grep -q 'modprobe qcom_wdt' "$tmp/f"; res "pro1x-loop-root: trial-check, the watchdog" $?
member usr/sbin/pro1x-boot
grep -q '^trial-check)' "$tmp/f" && grep -q '^trial)' "$tmp/f"; res "pro1x-boot: trial, trial-check" $?
if [ "${2:-}" = alarm ]; then
	grep -q 'alarm-check' "$tmp/f"; res "pro1x-boot: alarm-check" $?
fi
# devmem: busybox's applet, linked into the image (usr/sbin/devmem); the
# script finds it with command -v
grep -q -E '^(usr/)?s?bin/devmem$' "$tmp/list"; res "devmem (busybox)" $?
echo
[ "$fails" = 0 ] && echo "the trial's parts are there" || { echo "$fails missing: do not flash"; exit 1; }
