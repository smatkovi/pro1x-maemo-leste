#!/bin/bash
# The backup before anything is flashed on the device day
# (docs/device-day-2026-10-31.md, block 2): from the running Leste, read
# only, straight into the PC (nothing is written on the phone, not even
# /tmp):
#
#   - boot_a, boot_b, dtbo_a, dtbo_b, vbmeta_a, vbmeta_b, vbmeta_system_a/b,
#     misc, devinfo (dd of the partition, sha256 here and there)
#   - the GPT of every UFS LUN: primary (LBA 0-33) and backup (the last 33
#     LBAs) at the LUN's own block size (4 KiB here: 512-byte dumps cut the
#     entries off), the partition list (sgdisk -p) and the A/B attributes
#     of boot_a/b, dtbo_a/b, vbmeta_a/b (sgdisk -i): the slot state at the
#     time of the backup
#   - the small state on userdata: /host/pro1x-boot (menu choices, trial,
#     last-boot, the GPT backups pro1x-boot made), /host/pro1x-rtc-offset,
#     /host/pro1x-display.conf, the cmdline files of FreeBSD and KolibriOS,
#     the pool's label checksums
#   - lists only (these are big and the PC has them): /host, /host/freebsd,
#     /host/kolibri, /host/pro1x-modules, and the sha256 of every
#     /host/boot_b-*.img, matched against the PC's $WORK/out-*/ copies
#   - the running kernel's /proc/cmdline and the cmdlines inside the
#     boot_a/boot_b images
#
#   bash tools/devday/backup.sh [DIR]     default ~/ps/pro1x-backups/2026-10-31
#   DRY=1 bash tools/devday/backup.sh     the space check and the commands only
#                                         (empty files in /tmp/pro1x-devday-dry/backup)
#
# Needs the phone in Leste (SYS=leste), reachable as root. Refuses when the
# PC's disk would keep less than MIN_FREE_GB (10) GiB. A second run into the
# same DIR refuses; give another DIR.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
if [ "$DRY" = 1 ]; then OUT=${1:-/tmp/pro1x-devday-dry/backup}; else OUT=${1:-$HOME/ps/pro1x-backups/$DAY}; fi
PARTS="boot_a boot_b dtbo_a dtbo_b vbmeta_a vbmeta_b vbmeta_system_a vbmeta_system_b misc devinfo"
SLOTPARTS="boot_a boot_b dtbo_a dtbo_b vbmeta_a vbmeta_b"
# From the GPT dumps of 2026-10-24 (backup-slot-a/gpt-sd?-primary.bin):
# boot 96 MiB, dtbo 24 MiB, vbmeta* 64 KiB, misc 1 MiB, devinfo 4 KiB;
# six LUNs' GPTs at 4 KiB blocks (2 x 34 blocks), the lists and the state:
# about 245 MiB. The phone's own sizes are checked again below.
EXPECT_MB=260

[ ! -e "$OUT/SHA256SUMS" ] || die "$OUT has a backup already: give another DIR"
need_space "$OUT" "$EXPECT_MB"
log "backup into $OUT"

# the system must be Leste, with userdata on /host
cur=$(ph 'cat $R/host/pro1x-boot/current 2>/dev/null; true')
[ "$DRY" = 1 ] || [ "$cur" = leste ] || [ "$cur" = meego ] ||
    die "the phone runs '${cur:-?}', not Leste: boot Leste first"

# sizes on the phone (bytes), and the space again with them
total=0
for p in $PARTS; do
	sz=$(ph 'd=$(readlink -f $R/dev/disk/by-partlabel/'"$p"') && [ -n "$d" ] && if [ -b "$d" ]; then echo $(( $(cat $R/sys/class/block/${d##*/}/size) * 512 )); else stat -c %s "$d"; fi')
	[ "$DRY" = 1 ] && continue
	[ -n "$sz" ] || die "no partition $p on the phone"
	total=$((total + sz))
done
[ "$DRY" = 1 ] || need_space "$OUT" $((total / 1048576 + 20))

# 1. the partitions
for p in $PARTS; do
	log "partition $p"
	if [ "$DRY" = 1 ]; then ph "dd if=\$R/dev/disk/by-partlabel/$p bs=1M"; continue; fi
	ph "dd if=\$R/dev/disk/by-partlabel/$p bs=1M status=none" > "$OUT/$p.img"
	there=$(ph "sha256sum < \$R/dev/disk/by-partlabel/$p" | cut -c1-64)
	here=$(sha "$OUT/$p.img")
	[ "$here" = "$there" ] || die "$p: the copy here differs from the phone's ($here / $there)"
done

# 2. the GPTs, each LUN at its block size, and the slot attributes
log "GPTs"
luns=$(ph 'for d in $R/sys/block/sd?; do [ -e "$d" ] && echo ${d##*/}; done; true')
[ "$DRY" = 1 ] && luns=sda
for dev in $luns; do
	lbs=$(ph "cat \$R/sys/block/$dev/queue/logical_block_size")
	n=$(ph "cat \$R/sys/block/$dev/size")
	[ "$DRY" = 1 ] && { lbs=4096 n=0; }
	lbas=$((n * 512 / lbs))
	ph "dd if=\$R/dev/$dev bs=$lbs count=34 status=none" > "$OUT/gpt-$dev-primary.bin"
	ph "dd if=\$R/dev/$dev bs=$lbs count=33 skip=$((lbas - 33)) status=none" > "$OUT/gpt-$dev-backup.bin"
	ph "sgdisk -p \$R/dev/$dev" > "$OUT/gpt-$dev.txt" 2>&1 || true
	echo "$dev logical_block_size=$lbs lbas=$lbas" >> "$OUT/luns.txt"
done
for p in $SLOTPARTS; do
	ph 'd=$(readlink -f $R/dev/disk/by-partlabel/'"$p"'); n=${d##*/}; disk=/dev/$(basename "$(readlink -f $R/sys/class/block/$n/..)"); echo "== '"$p"' ($disk partition $(cat $R/sys/class/block/$n/partition))"; sgdisk -i "$(cat $R/sys/class/block/$n/partition)" "$R$disk"' \
	    >> "$OUT/slots.txt" 2>&1 || true
done
# the attributes in a line each (Qualcomm layout: 48-49 priority, 50
# active, 51-53 tries, 54 successful, 55 unbootable)
[ "$DRY" = 1 ] || python3 -I - "$OUT/slots.txt" > "$OUT/slots-decoded.txt" <<'EOF' || true
import re, sys
name = None
for line in open(sys.argv[1]):
    m = re.match(r'== (\S+)', line)
    if m:
        name = m.group(1)
    m = re.match(r'Attribute flags: ([0-9a-fA-F]+)', line)
    if m and name:
        a = int(m.group(1), 16)
        print(f"{name:9s} {a:016x}  prio {(a >> 48) & 3}  active {(a >> 50) & 1}  "
              f"tries {(a >> 51) & 7}  successful {(a >> 54) & 1}  unbootable {(a >> 55) & 1}")
EOF

# 3. the small state on userdata (tar to stdout: nothing written there)
log "state on /host"
ph 'cd $R/host && l=; for f in pro1x-boot pro1x-rtc-offset pro1x-display.conf freebsd/cmdline freebsd/cmdline.dsp-adopt freebsd/pool-labels-before.txt freebsd/pool.map kolibri/cmdline; do [ -e "$f" ] && l="$l $f"; done; tar -cf - $l' \
    > "$OUT/host-state.tar"
[ "$DRY" = 1 ] || tar -tvf "$OUT/host-state.tar" > "$OUT/host-state.list"

# 4. lists of what is big, and where the PC has the same boot images
log "lists"
ph 'ls -la $R/host/ $R/host/freebsd/ $R/host/kolibri/ $R/host/pro1x-modules/ 2>&1; df -h $R/host 2>&1; true' > "$OUT/host-ls.txt"
ph 'for f in $R/host/boot_b-*.img $R/host/boot-leste.img; do [ -f "$f" ] && echo "$(sha256sum < "$f" | cut -c1-64)  ${f##*/}"; done; true' > "$OUT/host-boot-images.sha256"
if [ "$DRY" != 1 ]; then
	: > "$OUT/host-boot-images-on-pc.txt"
	while read -r h name; do
		found=
		for f in "$WORK"/out-*/boot_b-*.img "$WORK"/out/boot-leste.img; do
			[ -f "$f" ] || continue
			[ "$(stat -c %s "$f")" = 100663296 ] || continue
			if [ "$(sha "$f")" = "$h" ]; then found="$f"; break; fi
		done
		echo "$name  ${found:-NOT ON THE PC}" >> "$OUT/host-boot-images-on-pc.txt"
	done < "$OUT/host-boot-images.sha256"
	# which saved image is the running boot_b
	bb=$(sha "$OUT/boot_b.img")
	echo "boot_b now: $bb $(grep "^$bb" "$OUT/host-boot-images.sha256" | cut -c67- | tr '\n' ' ')" >> "$OUT/host-boot-images-on-pc.txt"
fi

# 5. command lines and what runs
ph 'cat $R/proc/cmdline; uname -a; cat $R/host/pro1x-boot/last-boot 2>/dev/null; /usr/local/sbin/pro1x-boot list 2>&1; true' > "$OUT/running.txt"
[ "$DRY" = 1 ] || for p in boot_a boot_b; do
	python3 -I - "$OUT/$p.img" > "$OUT/cmdline-$p.txt" <<'EOF' || true
import struct, sys
d = open(sys.argv[1], 'rb').read(4096)
if d[:8] != b'ANDROID!':
    print('not an Android boot image'); sys.exit()
ver = struct.unpack_from('<I', d, 40)[0]
cmd = d[64:576].split(b'\0')[0] + d[608:1632].split(b'\0')[0]
print(f'header v{ver}: {cmd.decode(errors="replace")}')
EOF
done

if [ "$DRY" = 1 ]; then log "dry run: nothing written but empty files in $OUT"; exit 0; fi
( cd "$OUT" && sha256sum -- *.img *.bin *.tar > SHA256SUMS )
du -sh "$OUT" | tee "$OUT/size.txt"
log "done: $OUT; check with: (cd $OUT && sha256sum -c SHA256SUMS)"
cat "$OUT/slots-decoded.txt" "$OUT/host-boot-images-on-pc.txt" 2>/dev/null
