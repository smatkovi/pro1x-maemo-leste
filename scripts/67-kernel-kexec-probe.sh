#!/bin/bash
# A new kernel started once with kexec from the running Leste, before it is
# flashed (65-kernel-update.sh flash), from the build host. boot_b keeps the
# old kernel: a new one that hangs ends in the watchdog's reset and boot_b's
# Leste again, not in slot a (Sailfish OS, which waits for its LUKS code
# before anyone can reach the phone).
#
#   PHONE="ssh ... root@172.16.42.1" WORK=... OUT=$WORK/out-pro1x.2 \
#       bash scripts/67-kernel-kexec-probe.sh
#
# After 65-kernel-update.sh prepare (the modules in /host/pro1x-modules,
# the initramfs made in Leste). The device tree is the new kernel's (the
# one 50-build-bootimg.sh appends), with the memory banks the bootloader
# fills in, taken from the running system's /sys/firmware/fdt: besides
# /chosen, which kexec writes itself, that is all the bootloader changes.
# The command line is the running one with what the new boot image adds,
# and for the probe panic=10 and watchdog.open_timeout=300: the SoC's
# watchdog is armed before the jump (about 32 s), the new kernel's qcom_wdt
# feeds it until dsme opens it, for at most 5 minutes.
#
# Reboots the phone (kexec); then waits for it and prints which kernel
# runs. EXTRA adds to the command line (ignore_loglevel: the whole kernel
# log goes to the RAM console, which the next boot shows in /sys/fs/pstore). The measurements of a kernel belong to a cold start, not to this
# one: the first kernel's firmware state (remote processors, RPM votes)
# carries over a kexec.
set -euo pipefail
WORK=${WORK:?set WORK=<build dir>}
PHONE=${PHONE:?set PHONE=<ssh command for root on the phone in Leste>}
OUT=${OUT:-$WORK/out}
# the PC's side of the USB network, which the phone's re-enumeration drops
USBIF=${USBIF:-enp19s0u1u1}
log() { echo ">> $*"; }
ph() { $PHONE "$@" < /dev/null; }

KVER=$(cat "$OUT/kernelrelease")
running=$(ph uname -r)
[ "$KVER" != "$running" ] || { echo "the phone runs $KVER already"; exit 1; }
# DTB: another device tree to try with the kernel (default: the boot image's)
DTB=${DTB:-$OUT/sm6115-fxtec-pro1x-abl.dtb}
for f in "$OUT/Image.gz" "$DTB" "$OUT/initrd.img-$KVER" \
    "$OUT/boot_b-$KVER.img"; do
	[ -f "$f" ] || { echo "missing: $f (65-kernel-update.sh prepare first)"; exit 1; }
done
ph "[ -f /host/pro1x-modules/$KVER/modules.dep ]" ||
    { echo "/host/pro1x-modules/$KVER is missing: 65-kernel-update.sh prepare first"; exit 1; }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
log "device tree: $KVER's, with the bootloader's memory banks"
ph "cat /sys/firmware/fdt" > "$T/live.dtb"
dtc -q -I dtb -O dts "$T/live.dtb" -o "$T/live.dts"
dtc -q -I dtb -O dts "$DTB" -o "$T/new.dts"
python3 - "$T/live.dts" "$T/new.dts" "$T/probe.dts" <<'EOF'
import re, sys
live, new, out = (open(sys.argv[1]).read(), open(sys.argv[2]).read(), sys.argv[3])
node = lambda s: re.search(r'\n\tmemory@[0-9a-f]+ \{\n(.*?)\n\t\};', s, re.S)
l, n = node(live), node(new)
if not l or not n:
    sys.exit('no /memory node')
open(out, 'w').write(new[:n.start(1)] + l.group(1) + new[n.end(1):])
EOF
dtc -q -I dts -O dtb "$T/probe.dts" -o "$T/probe.dtb"

# what the new image's command line adds to the flashed one's
args() { python3 -c "
import sys; d = open(sys.argv[1], 'rb').read(4096)
print((d[64:576].split(b'\0')[0] + d[608:1632].split(b'\0')[0]).decode())" "$1"; }
ph "dd if=/dev/disk/by-partlabel/boot_b bs=4096 count=1 status=none" > "$T/flashed.hdr"
add=$(comm -13 <(args "$T/flashed.hdr" | tr ' ' '\n' | sort -u) \
    <(args "$OUT/boot_b-$KVER.img" | tr ' ' '\n' | sort -u) | paste -sd' ' -)
# EXTRA: more for the probe, e.g. ignore_loglevel: the whole kernel log in
# the RAM console (/sys/fs/pstore/console-ramoops-0 after the next boot)
cmdline="$(ph cat /proc/cmdline) $add panic=10 watchdog.open_timeout=300 ${EXTRA:-}"
log "command line: $cmdline"

log "kernel, initramfs, device tree to /host/kernel-update"
ph "mkdir -p /host/kernel-update"
$PHONE "cat > /host/kernel-update/Image-$KVER.gz" < "$OUT/Image.gz"
$PHONE "cat > /host/kernel-update/probe-$KVER.dtb" < "$T/probe.dtb"
$PHONE "cat > /host/kernel-update/initrd.img-$KVER" < "$OUT/initrd.img-$KVER"

log "kexec into $KVER (the phone reboots)"
ph "set -e; cd /host/kernel-update
	kexec -l Image-$KVER.gz --initrd=initrd.img-$KVER --dtb=probe-$KVER.dtb --command-line='$cmdline'
	sync
	# the APSS watchdog for its longest time (about 32 s), as the boot menu
	# arms it for FreeBSD: a kernel that hangs on the way resets the phone
	busybox devmem 0x0f017008 32 0; busybox devmem 0x0f017004 32 1
	busybox devmem 0x0f017010 32 0xfffff; busybox devmem 0x0f017014 32 0xfffff
	busybox devmem 0x0f017008 32 1
	echo s > /proc/sysrq-trigger; echo u > /proc/sysrq-trigger; sleep 3
	nohup kexec -e > /dev/null 2>&1 &" || true

log "waiting for the phone (at most 10 minutes)"
sleep 30
for i in $(seq 60); do
	if ip link show "$USBIF" > /dev/null 2>&1 && ! ip -4 addr show "$USBIF" | grep -q 172.16.42.2; then
		sudo -n ip addr add 172.16.42.2/24 dev "$USBIF" 2> /dev/null || true
	fi
	now=$(timeout 15 $PHONE uname -r < /dev/null 2> /dev/null) && break
	sleep 10
done
case "${now:-}" in
"$KVER") log "up with $KVER: flash it with 65-kernel-update.sh flash" ;;
"")	log "not reachable after 10 minutes"; exit 1 ;;
*)	log "back with $now: $KVER did not come up (see /sys/fs/pstore, the watchdog reset)"; exit 1 ;;
esac
