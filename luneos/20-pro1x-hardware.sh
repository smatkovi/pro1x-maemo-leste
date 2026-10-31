#!/bin/bash
# Pro1X hardware support for the LuneOS image from 10-luneos-image.sh.
#
# Runs as root on the phone, in Maemo Leste. LuneOS boots Leste's kernel (the
# boot menu loop-mounts its image), so Leste provides what the PinePhone
# image has no use for: the kernel modules and firmware, and the modem's
# helpers rmtfs and tqftpserv (pd-mapper is in the kernel), as in Kali.
# LuneOS's own adaptation mechanism (luneos-device-config) gets the display
# rotation, touchscreen and keys from
# /usr/share/luneos/adaptations/pro1x/deviceinfo, named for the device tree's
# "fxtec,pro1x".
#
#   bash luneos/20-pro1x-hardware.sh      # IMG=/host/luneos.img
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
IMG=${IMG:-/host/luneos.img}
MNT=${MNT:-/mnt/luneos}
# the modules of the kernel that runs, or of KVER (in a chroot of a
# Leste image on the PC, whose modules are not the running kernel's)
KVER=${KVER:-$(uname -r)}
log() { echo ">> $*"; }

mkdir -p "$MNT"
mountpoint -q "$MNT" || mount -o loop "$IMG" "$MNT"
for m in proc sys dev; do
	mountpoint -q "$MNT/$m" || mount --bind "/$m" "$MNT/$m"
done
cleanup() {
	for m in dev sys proc; do
		! mountpoint -q "$MNT/$m" || umount "$MNT/$m" || umount -l "$MNT/$m"
	done
	umount "$MNT"
}
trap cleanup EXIT

# --- from Leste: kernel modules, firmware, modem helpers -----------------------
log "kernel modules $KVER and firmware"
find "$MNT/lib/modules" -mindepth 1 -maxdepth 1 -name '*pinephone*' -exec rm -rf {} +
cp -a "/lib/modules/$KVER" "$MNT/lib/modules/"
# The kernel looks in /lib/firmware/updates first; no LuneOS package writes there.
mkdir -p "$MNT/lib/firmware/updates"
cp -a /lib/firmware/. "$MNT/lib/firmware/updates/"
rm -rf "$MNT/lib/firmware/updates/updates" "$MNT/lib/firmware/updates"/regulatory.db*

log "modem helpers, audio, input"
P=$MNT/usr/lib/pro1x
mkdir -p "$P" "$MNT/usr/local/sbin"
# Built for Leste's glibc 2.36; LuneOS's is newer. libudev comes from LuneOS.
cp -a /usr/bin/rmtfs /usr/bin/tqftpserv /usr/lib/aarch64-linux-gnu/libqrtr.so.1* "$P/"
install -m 755 /usr/bin/pro1x-audio-init "$MNT/usr/bin/"
install -m 644 "$REPO/kali/rootfs-overlay/etc/systemd/system/pro1x-audio-init.service" \
	"$MNT/etc/systemd/system/"
# UCM profile (scripts/45): driver sm4250 looks for ucm2/sm4250/sm4250.conf
U=$MNT/usr/share/alsa/ucm2
mkdir -p "$U/sm4250"
cp -a /usr/share/alsa/ucm2/Pro1X "$U/"
ln -sfn ../Pro1X/Pro1X.conf "$U/sm4250/sm4250.conf"
# stable WLAN MAC, double tap to wake
install -m 644 "$REPO/rootfs-overlay/etc/udev/rules.d/82-pro1x-wlan-mac.rules" "$MNT/etc/udev/rules.d/"
install -m 755 "$REPO/rootfs-overlay/usr/local/sbin/pro1x-wlan-mac" "$MNT/usr/local/sbin/"
install -m 644 "$REPO/rootfs-overlay/etc/modprobe.d/pro1x-dt2w.conf" "$MNT/etc/modprobe.d/"
install -m 755 "$REPO/tools/pro1x-boot/pro1x-boot" "$MNT/usr/local/sbin/pro1x-boot"
# the clock across power off (LuneOS started at 1970 without it), its
# offset shared with the other systems
install -m 755 /usr/local/sbin/pro1x-rtc "$REPO/rootfs-overlay/usr/local/sbin/pro1x-rtc-sync" \
	"$MNT/usr/local/sbin/"
install -m 644 "$REPO/kali/rootfs-overlay/etc/systemd/system/pro1x-rtc.service" \
	"$MNT/etc/systemd/system/"
cp -a --no-preserve=ownership "$REPO/luneos/rootfs-overlay/." "$MNT/"

# --- configuration -------------------------------------------------------------
log "configuration"
# The PinePhone's boot partition does not exist; systemd would wait for it.
sed -i '/[[:space:]]\/boot[[:space:]]/d' "$MNT/etc/fstab"
# A line for / gets the initramfs's options (pro1x-loop-root), else
# systemd's remount of / drops lazytime again; write-back every 15 s
awk 'BEGIN { OFS = "\t" } $1 !~ /^#/ && $2 == "/" {
	if ($4 !~ /lazytime/) $4 = $4 ",lazytime"; if ($4 !~ /commit=/) $4 = $4 ",commit=30" } { print }' \
	"$MNT/etc/fstab" > "$MNT/etc/fstab.pro1x" && cat "$MNT/etc/fstab.pro1x" > "$MNT/etc/fstab" && rm "$MNT/etc/fstab.pro1x"
install -D -m 644 "$REPO/rootfs-overlay/etc/sysctl.d/60-pro1x-writeback.conf" "$MNT/etc/sysctl.d/60-pro1x-writeback.conf"
echo luneos > "$MNT/etc/hostname"
# The USB link: the kernel's built-in CDC composite gadget (g_cdc, as under
# Leste) holds the controller from boot on, so LuneOS's configfs gadget
# never binds (its service fails after five attempts). Kali's unit gives the
# gadget's usb0 the phone's address on this link, 172.16.42.1/24.
install -m 644 "$REPO/kali/rootfs-overlay/etc/systemd/system/pro1x-usb0.service" \
	"$MNT/etc/systemd/system/"
# PinePhone line: sets card 0's profile before audiod has loaded any card,
# and PulseAudio exits on the error. audiod loads the devices (see the file).
sed -i 's/^set-card-profile 0 /# Pro1X: no card yet at this point\n#&/' "$MNT/etc/pulse/webos-system.pa"
chroot "$MNT" /bin/sh -e <<'EOF'
# eg25-manager drives the PinePhone's modem over its GPIOs
# serial-getty@ttyS0: the PinePhone's serial console; waits 90 s for it
systemctl mask eg25-manager.service luneos-usb-gadget.service serial-getty@ttyS0.service
systemctl enable pro1x-rmtfs.service pro1x-tqftpserv.service pro1x-audio-init.service \
	pro1x-usb0.service pro1x-rtc.service
EOF
# luneos-device-config writes its results into configd's layers; a stale cache
# from the PinePhone build would outlive the new deviceinfo.
rm -f "$MNT/var/preferences/configd_db.json"
log "done: the boot menu entry \"LuneOS\" needs an initramfs with the current tools/pro1x-boot"
