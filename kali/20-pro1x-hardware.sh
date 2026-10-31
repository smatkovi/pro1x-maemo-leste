#!/bin/bash
# Pro1X hardware support and desktop for the Kali image from 10-make-rootfs.sh.
#
# Runs as root on the phone, in Maemo Leste, with the image mounted on $MNT.
# Kali shares Leste's kernel (the boot menu loop-mounts its image the same
# way), so Leste provides what Kali has no package for: the kernel modules
# and firmware, and the Pro1X stack built in scripts/46 (hexagonrpcd,
# libssc, iio-sensor-proxy with SSC support, pro1x-gnss, pro1x-steps). Their libqmi is
# older than Kali's, so they get a private library directory
# (/usr/lib/pro1x) and Kali's ModemManager keeps its own.
#
#   SSH_PUBKEY=/path/key.pub bash kali/20-pro1x-hardware.sh
#
# SSH_PUBKEY: key allowed to log in as kali (optional; without it, the
# password only works over the USB link).
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
MNT=${MNT:-/mnt/kali}
SSH_PUBKEY=${SSH_PUBKEY:-}
# the modules of the kernel that runs, or of KVER (in a chroot of a
# Leste image on the PC, whose modules are not the running kernel's)
KVER=${KVER:-$(uname -r)}
export DEBIAN_FRONTEND=noninteractive

mountpoint -q "$MNT" || { echo "$MNT is not mounted"; exit 1; }
[ -z "$SSH_PUBKEY" ] || [ -f "$SSH_PUBKEY" ] || { echo "$SSH_PUBKEY missing"; exit 1; }
log() { echo ">> $*"; }

for m in proc sys dev dev/pts; do
	mountpoint -q "$MNT/$m" || mount --bind "/$m" "$MNT/$m"
done
cleanup() {
	rm -f "$MNT/usr/sbin/policy-rc.d"
	for m in dev/pts dev sys proc; do
		! mountpoint -q "$MNT/$m" || umount "$MNT/$m" || umount -l "$MNT/$m"
	done
}
trap cleanup EXIT
cp /etc/resolv.conf "$MNT/etc/resolv.conf"
# No services start inside the chroot.
printf '#!/bin/sh\nexit 101\n' > "$MNT/usr/sbin/policy-rc.d"
chmod 755 "$MNT/usr/sbin/policy-rc.d"

# --- Kali packages -------------------------------------------------------------
log "Kali packages"
chroot "$MNT" /bin/bash -euo pipefail <<'EOF'
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
# Desktop and Kali's default tool set; the modem (rmtfs, tqftpserv, qrtr),
# mobile data (ModemManager + NetworkManager; the APN database for
# NetworkManager's gsm.auto-config, qmicli for looking at the modem; both
# only came as Recommends), GPS (gpsd); the libraries the Pro1X stack from
# Leste links against.
apt-get install -y -q kali-desktop-xfce kali-linux-default \
	network-manager modemmanager rmtfs tqftpserv qrtr-tools \
	mobile-broadband-provider-info libqmi-utils \
	bluez gpsd alsa-utils alsa-ucm-conf wireless-regdb mesa-utils xinput \
	libprotobuf-c1 libqrtr-glib0 libgudev-1.0-0 libpolkit-gobject-1-0 polkitd
apt-get clean
EOF

# --- from Leste: kernel modules, firmware, the Pro1X stack -------------------
log "kernel modules $KVER and firmware"
mkdir -p "$MNT/lib/modules"
cp -a "/lib/modules/$KVER" "$MNT/lib/modules/"
# Leste's firmware works with this hardware. The kernel looks in
# /lib/firmware/updates first, and no Kali package writes there.
mkdir -p "$MNT/lib/firmware/updates"
cp -a /lib/firmware/. "$MNT/lib/firmware/updates/"
rm -rf "$MNT/lib/firmware/updates/updates" "$MNT/lib/firmware/updates"/regulatory.db*

log "Pro1X sensor and GNSS stack"
P=$MNT/usr/lib/pro1x
mkdir -p "$P" "$MNT/usr/local/sbin" "$MNT/usr/share/pro1x"
cp -a /usr/local/lib/aarch64-linux-gnu/libssc.so* /usr/local/lib/aarch64-linux-gnu/libqmi-glib.so* \
	/usr/local/lib/aarch64-linux-gnu/libhexagonrpc.so* "$P/"
cp -a /usr/local/bin/hexagonrpcd /usr/local/bin/ssccli /usr/local/bin/monitor-sensor \
	/usr/local/bin/pro1x-gnss /usr/local/sbin/pro1x-steps "$P/"
# Leste's /usr/sbin/iio-sensor-proxy is a link into /usr/local/libexec
cp -L /usr/sbin/iio-sensor-proxy "$P/iio-sensor-proxy"
install -m 644 /etc/dbus-1/system.d/net.hadess.SensorProxy.conf \
	"$REPO/rootfs-overlay/etc/dbus-1/system.d/org.pro1x.Steps.conf" "$MNT/etc/dbus-1/system.d/"
install -D -m 644 /usr/share/polkit-1/actions/net.hadess.SensorProxy.policy \
	"$MNT/usr/share/polkit-1/actions/net.hadess.SensorProxy.policy"
cp -a /usr/share/pro1x/hexagonfs "$MNT/usr/share/pro1x/"
install -m 755 /usr/bin/pro1x-audio-init /usr/bin/pro1x-voice-call /usr/bin/pro1x-voicepcm "$MNT/usr/bin/"
install -m 755 "$REPO/tools/pro1x-boot/pro1x-boot" "$MNT/usr/local/sbin/pro1x-boot"
# the clock across power off, its offset shared with the other systems
install -m 755 /usr/local/sbin/pro1x-rtc "$REPO/rootfs-overlay/usr/local/sbin/pro1x-rtc-sync" \
	"$MNT/usr/local/sbin/"
# Kali's sudo looks no further than /usr/sbin (secure_path): the tools
# people run with sudo there too
for t in pro1x-boot pro1x-rtc; do ln -sfn "../local/sbin/$t" "$MNT/usr/sbin/$t"; done
# UCM profile (scripts/45): driver sm4250 looks for ucm2/sm4250/sm4250.conf
U=$MNT/usr/share/alsa/ucm2
mkdir -p "$U/sm4250"
cp -a /usr/share/alsa/ucm2/Pro1X "$U/"
ln -sfn ../Pro1X/Pro1X.conf "$U/sm4250/sm4250.conf"
# udev: SSC sensor types and mount matrix, stable WLAN MAC, Wi-Fi power save
install -m 644 "$REPO/rootfs-overlay/etc/udev/rules.d/81-pro1x-sensors.rules" \
	"$REPO/rootfs-overlay/etc/udev/rules.d/82-pro1x-wlan-mac.rules" \
	"$REPO/rootfs-overlay/etc/udev/rules.d/85-pro1x-wlan-powersave.rules" "$MNT/etc/udev/rules.d/"
install -m 755 "$REPO/rootfs-overlay/usr/local/sbin/pro1x-wlan-mac" \
	"$REPO/rootfs-overlay/usr/local/sbin/pro1x-wlan-powersave" "$MNT/usr/local/sbin/"
install -m 644 "$REPO/rootfs-overlay/etc/modprobe.d/pro1x-dt2w.conf" "$MNT/etc/modprobe.d/"
# write-back every 15 s instead of 5 (the root's commit=30 below)
install -D -m 644 "$REPO/rootfs-overlay/etc/sysctl.d/60-pro1x-writeback.conf" "$MNT/etc/sysctl.d/60-pro1x-writeback.conf"
# After the packages, whose maintainer scripts write /etc/default/gpsd.
cp -a --no-preserve=ownership "$REPO/kali/rootfs-overlay/." "$MNT/"

# --- configuration -------------------------------------------------------------
log "configuration"
chroot "$MNT" /bin/bash -euo pipefail <<'EOF'
echo kali > /etc/hostname
grep -q '127.0.1.1' /etc/hosts || echo '127.0.1.1	kali' >> /etc/hosts
sed -i 's/^# *\(en_US.UTF-8\)/\1/; s/^# *\(de_AT.UTF-8\)/\1/' /etc/locale.gen
locale-gen > /dev/null
echo LANG=en_US.UTF-8 > /etc/default/locale
ln -sfn /usr/share/zoneinfo/Europe/Vienna /etc/localtime
echo Europe/Vienna > /etc/timezone
# The Pro1X keyboard has a US layout.
[ ! -f /etc/default/keyboard ] ||
	sed -i 's/^XKBMODEL=.*/XKBMODEL="pc105"/; s/^XKBLAYOUT=.*/XKBLAYOUT="us"/' /etc/default/keyboard

# Kali's default user (kali/kali), in the groups that exist
groups=
for g in sudo adm audio video input render netdev plugdev bluetooth dialout; do
	getent group "$g" > /dev/null && groups=$groups${groups:+,}$g
done
shell=$(command -v zsh || echo /bin/bash)
id kali > /dev/null 2>&1 || useradd -m -s "$shell" -G "$groups" kali
echo kali:kali | chpasswd

# root on the loop device the initramfs sets up; /host is moved in by it
# with the initramfs's options (pro1x-loop-root), else systemd's remount
# of / drops lazytime again
grep -q '^/dev/loop0' /etc/fstab || echo '/dev/loop0 / ext4 defaults,noatime,lazytime,commit=30 0 0' >> /etc/fstab
mkdir -p /host
# Bluetooth off until switched on (as on Leste, scripts/45): BlueZ powered
# every adapter at its start (AutoEnable)
if [ -f /etc/bluetooth/main.conf ]; then
	sed -i 's/^#\?AutoEnable=.*/AutoEnable=false/' /etc/bluetooth/main.conf
	grep -q '^AutoEnable=false' /etc/bluetooth/main.conf ||
		printf '\n[Policy]\nAutoEnable=false\n' >> /etc/bluetooth/main.conf
fi

systemctl enable pro1x-rtc.service pro1x-hexagonrpcd.service pro1x-sensors.service pro1x-gnss.service pro1x-steps.service \
	pro1x-audio-init.service pro1x-usb0.service rmtfs.service tqftpserv.service \
	NetworkManager.service ModemManager.service bluetooth.service ssh.service
# GPS on demand: gpsd started by its socket for the first client, the GNSS
# engine (pro1x-gnss, WantedBy=gpsd.service) only with it; Debian's package
# enables gpsd.service itself, whose disable takes the socket along (Also=)
systemctl disable gpsd.service 2> /dev/null || true
systemctl enable gpsd.socket
# replaced by pro1x-sensors.service (libssc)
systemctl mask iio-sensor-proxy.service
systemctl set-default graphical.target
EOF

if [ -n "$SSH_PUBKEY" ]; then
	uid=$(chroot "$MNT" id -u kali)
	install -d -m 700 -o "$uid" -g "$uid" "$MNT/home/kali/.ssh"
	install -m 600 -o "$uid" -g "$uid" "$SSH_PUBKEY" "$MNT/home/kali/.ssh/authorized_keys"
fi
log "done"
