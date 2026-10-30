#!/bin/bash
# Pro1X hardware support and the phone shell (Phosh) for the Arch Linux ARM
# image from 10-make-rootfs.sh.
#
# Runs as root on the phone, in Maemo Leste, with the image mounted on $MNT.
# Arch shares Leste's kernel (the boot menu loop-mounts its image the same
# way), so Arch Linux ARM's own kernel package goes, and Leste provides
# what Arch has no package for: the kernel modules and firmware, the Pro1X
# stack built in scripts/46 (hexagonrpcd, libssc, iio-sensor-proxy with
# SSC support, pro1x-gnss, pro1x-steps) and, where Arch Linux ARM lacks
# them, the modem's helpers (rmtfs, tqftpserv, qrtr). They get a private
# library directory (/usr/lib/pro1x), as in Kali.
#
#   SSH_PUBKEY=/path/key.pub bash arch/20-pro1x-hardware.sh
#
# SSH_PUBKEY: key allowed to log in as alarm (optional; without it, the
# password only works over the USB link).
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
MNT=${MNT:-/mnt/arch}
SSH_PUBKEY=${SSH_PUBKEY:-}
# the lock screen's keypad has digits only
PIN=${PIN:-147147}
KVER=$(uname -r)

mountpoint -q "$MNT" || { echo "$MNT is not mounted"; exit 1; }
[ -z "$SSH_PUBKEY" ] || [ -f "$SSH_PUBKEY" ] || { echo "$SSH_PUBKEY missing"; exit 1; }
log() { echo ">> $*"; }

for m in proc sys dev dev/pts; do
	mountpoint -q "$MNT/$m" && continue
	mount --bind "/$m" "$MNT/$m"
	# a slave of the system's: unmounting dev/pts here would otherwise
	# take the system's /dev/pts along (a bind of a shared /dev is its peer)
	mount --make-rslave "$MNT/$m"
done
cleanup() {
	# the gpg-agent pacman starts to check signatures keeps /dev busy
	chroot "$MNT" gpgconf --homedir /etc/pacman.d/gnupg --kill all 2> /dev/null || true
	for m in dev/pts dev sys proc; do
		! mountpoint -q "$MNT/$m" || umount "$MNT/$m" || umount -l "$MNT/$m"
	done
}
trap cleanup EXIT
rm -f "$MNT/etc/resolv.conf"
cp /etc/resolv.conf "$MNT/etc/resolv.conf"

# --- Arch packages -------------------------------------------------------------
log "Arch Linux ARM packages"
chroot "$MNT" /bin/bash -euo pipefail <<'EOF'
# pacman's download sandbox needs Landlock, which Leste's kernel (the one
# Arch runs with) does not have
grep -q '^DisableSandbox' /etc/pacman.conf ||
	sed -i '/^\[options\]/a DisableSandbox' /etc/pacman.conf
pacman-key --init > /dev/null
pacman-key --populate archlinuxarm > /dev/null
# its gpg-agent would keep /dev busy
gpgconf --homedir /etc/pacman.d/gnupg --kill all 2> /dev/null || true
# Leste's kernel boots this image: no kernel or initramfs of Arch's own
for p in linux-aarch64 linux-aarch64-headers mkinitcpio; do
	! pacman -Q "$p" > /dev/null 2>&1 || pacman -Rdd --noconfirm "$p" > /dev/null
done
pacman -Syu --noconfirm --needed
# The phone shell (Phosh, its compositor phoc, the on-screen keyboard), the
# modem and mobile data (ModemManager + NetworkManager), Bluetooth, audio
# (PipeWire with Leste's UCM profile), GPS (gpsd), and what the Pro1X stack
# from Leste links against.
pacman -S --noconfirm --needed \
	phosh phoc squeekboard gnome-console \
	networkmanager modemmanager bluez bluez-utils \
	pipewire pipewire-pulse pipewire-alsa wireplumber alsa-ucm-conf alsa-utils \
	gpsd iw wireless-regdb mesa sudo openssh polkit \
	protobuf-c libgudev noto-fonts
# phone apps where Arch Linux ARM has them
for p in gnome-calls chatty phosh-mobile-settings portfolio-file-manager \
	gnome-clocks gnome-weather gnome-maps epiphany; do
	pacman -Si "$p" > /dev/null 2>&1 && pacman -S --noconfirm --needed "$p" > /dev/null || true
done
pacman -Scc --noconfirm > /dev/null
EOF

# --- from Leste: kernel modules, firmware, the Pro1X stack -------------------
log "kernel modules $KVER and firmware"
mkdir -p "$MNT/usr/lib/modules"
cp -a "/lib/modules/$KVER" "$MNT/usr/lib/modules/"
# Leste's firmware works with this hardware. The kernel looks in
# /lib/firmware/updates first, and no Arch package writes there.
mkdir -p "$MNT/usr/lib/firmware/updates"
cp -a /lib/firmware/. "$MNT/usr/lib/firmware/updates/"
rm -rf "$MNT/usr/lib/firmware/updates/updates" "$MNT/usr/lib/firmware/updates"/regulatory.db*

log "Pro1X sensor and GNSS stack"
P=$MNT/usr/lib/pro1x
mkdir -p "$P" "$MNT/usr/local/sbin" "$MNT/usr/share/pro1x"
cp -a /usr/local/lib/aarch64-linux-gnu/libssc.so* /usr/local/lib/aarch64-linux-gnu/libqmi-glib.so* \
	/usr/local/lib/aarch64-linux-gnu/libhexagonrpc.so* "$P/"
cp -a /usr/local/bin/hexagonrpcd /usr/local/bin/ssccli /usr/local/bin/monitor-sensor \
	/usr/local/bin/pro1x-gnss /usr/local/sbin/pro1x-steps "$P/"
# Leste's /usr/sbin/iio-sensor-proxy is a link into /usr/local/libexec
cp -L /usr/sbin/iio-sensor-proxy "$P/iio-sensor-proxy"
install -D -m 644 /etc/dbus-1/system.d/net.hadess.SensorProxy.conf \
	"$MNT/usr/share/dbus-1/system.d/net.hadess.SensorProxy.conf"
install -m 644 "$REPO/rootfs-overlay/etc/dbus-1/system.d/org.pro1x.Steps.conf" \
	"$MNT/usr/share/dbus-1/system.d/"
install -D -m 644 /usr/share/polkit-1/actions/net.hadess.SensorProxy.policy \
	"$MNT/usr/share/polkit-1/actions/net.hadess.SensorProxy.policy"
cp -a /usr/share/pro1x/hexagonfs "$MNT/usr/share/pro1x/"
# (the units run them from /usr/bin, as in Kali)
install -m 755 /usr/bin/pro1x-audio-init /usr/bin/pro1x-voice-call /usr/bin/pro1x-voicepcm "$MNT/usr/bin/"
install -m 755 "$REPO/tools/pro1x-boot/pro1x-boot" "$MNT/usr/local/sbin/pro1x-boot"
# the clock across power off, its offset shared with the other systems
install -m 755 /usr/local/sbin/pro1x-rtc "$REPO/rootfs-overlay/usr/local/sbin/pro1x-rtc-sync" \
	"$MNT/usr/local/sbin/"

# The modem's helpers, from Leste where Arch Linux ARM has none (they talk
# to the modem's remote filesystem and file server over QRTR)
for t in rmtfs tqftpserv qrtr-ns; do
	chroot "$MNT" sh -c "command -v $t" > /dev/null 2>&1 && continue
	log "$t from Leste"
	install -m 755 "/usr/bin/$t" "$P/$t"
	ln -sfn "../lib/pro1x/$t" "$MNT/usr/local/bin/$t"
done
cp -a /usr/lib/aarch64-linux-gnu/libqrtr.so.1* "$P/"

# UCM profile (scripts/45): driver sm4250 looks for ucm2/sm4250/sm4250.conf
U=$MNT/usr/share/alsa/ucm2
mkdir -p "$U/sm4250"
cp -a /usr/share/alsa/ucm2/Pro1X "$U/"
ln -sfn ../Pro1X/Pro1X.conf "$U/sm4250/sm4250.conf"
# udev: SSC sensor types and mount matrix, stable WLAN MAC
install -m 644 "$REPO/rootfs-overlay/etc/udev/rules.d/60-pro1x-ufs.rules" \
	"$REPO/rootfs-overlay/etc/udev/rules.d/81-pro1x-sensors.rules" \
	"$REPO/rootfs-overlay/etc/udev/rules.d/82-pro1x-wlan-mac.rules" "$MNT/etc/udev/rules.d/"
install -m 755 "$REPO/rootfs-overlay/usr/local/sbin/pro1x-wlan-mac" "$MNT/usr/local/sbin/"
install -D -m 644 "$REPO/rootfs-overlay/etc/modprobe.d/pro1x-dt2w.conf" "$MNT/etc/modprobe.d/pro1x-dt2w.conf"

# systemd units: Kali's Pro1X units (the same for any systemd system), the
# modem helpers' units where they came from Leste, then Arch's own files
cp -a "$REPO/kali/rootfs-overlay/etc/systemd/system/." "$MNT/etc/systemd/system/"
cp -a "$REPO/kali/rootfs-overlay/usr/lib/pro1x/hexagonrpcd-prepare" "$P/"
# the rmtfs and ModemManager drop-ins wait with it (docs/kali-mobile-data.md;
# without qrtr-lookup, which Arch lacks, its ModemManager wait is a no-op)
cp -a "$REPO/kali/rootfs-overlay/usr/lib/pro1x/pro1x-modem-wait" "$P/"
for t in rmtfs tqftpserv qrtr-ns; do
	[ -f "$P/$t" ] || continue
	install -m 644 "/lib/systemd/system/$t.service" "$MNT/etc/systemd/system/$t.service"
	sed -i "s|/usr/bin/$t|/usr/lib/pro1x/$t|" "$MNT/etc/systemd/system/$t.service"
	mkdir -p "$MNT/etc/systemd/system/$t.service.d"
	printf '[Service]\nEnvironment=LD_LIBRARY_PATH=/usr/lib/pro1x\n' \
		> "$MNT/etc/systemd/system/$t.service.d/50-pro1x-libs.conf"
done
[ ! -f "$P/rmtfs" ] || sed -i 's|^ExecStart=/usr/bin/rmtfs|ExecStart=/usr/lib/pro1x/rmtfs|' \
	"$MNT/etc/systemd/system/rmtfs.service.d/pro1x.conf"
cp -a "$REPO/arch/rootfs-overlay/." "$MNT/"
install -m 644 "$REPO/kali/rootfs-overlay/etc/ssh/sshd_config.d/pro1x.conf" \
	"$MNT/etc/ssh/sshd_config.d/pro1x.conf"
install -m 644 "$REPO/kali/rootfs-overlay/etc/default/gpsd" "$MNT/etc/default/gpsd"

# --- configuration -------------------------------------------------------------
log "configuration"
chroot "$MNT" /bin/bash -euo pipefail -s "$PIN" <<'EOF'
PIN=$1
echo arch > /etc/hostname
grep -q '127.0.1.1' /etc/hosts || echo '127.0.1.1	arch' >> /etc/hosts
sed -i 's/^#\(en_US.UTF-8\)/\1/; s/^#\(de_AT.UTF-8\)/\1/' /etc/locale.gen
locale-gen > /dev/null
echo LANG=en_US.UTF-8 > /etc/locale.conf
ln -sfn /usr/share/zoneinfo/Europe/Vienna /etc/localtime
# The Pro1X keyboard has a US layout.
printf 'XKBLAYOUT=us\nXKBMODEL=pc105\n' > /etc/vconsole.conf

# Arch Linux ARM's user alarm: a numeric password for the lock screen
for g in wheel audio video input render network uucp lp; do
	getent group "$g" > /dev/null && usermod -aG "$g" alarm
done
echo "alarm:$PIN" | chpasswd
install -d -m 750 /etc/sudoers.d
echo '%wheel ALL=(ALL:ALL) ALL' > /etc/sudoers.d/10-wheel
chmod 440 /etc/sudoers.d/10-wheel

# root on the loop device the initramfs sets up; /host is moved in by it
grep -q '^/dev/loop0' /etc/fstab || echo '/dev/loop0 / ext4 defaults,noatime 0 0' >> /etc/fstab
mkdir -p /host

systemctl enable pro1x-rtc.service pro1x-hexagonrpcd.service pro1x-sensors.service \
	pro1x-gnss.service pro1x-steps.service pro1x-audio-init.service pro1x-usb0.service \
	rmtfs.service tqftpserv.service gpsd.service \
	NetworkManager.service ModemManager.service bluetooth.service sshd.service phosh.service
# until the first boot is confirmed (pro1x-boot-ok), an unreachable boot
# reboots into the boot menu's default system after 15 minutes
systemctl enable pro1x-boot-guard.timer
# NetworkManager has the interfaces, not Arch Linux ARM's systemd-networkd
systemctl disable systemd-networkd.service systemd-networkd.socket 2> /dev/null || true
# replaced by pro1x-sensors.service (libssc)
! systemctl list-unit-files iio-sensor-proxy.service > /dev/null 2>&1 ||
	systemctl mask iio-sensor-proxy.service
systemctl set-default graphical.target
EOF

if [ -n "$SSH_PUBKEY" ]; then
	uid=$(chroot "$MNT" id -u alarm)
	install -d -m 700 -o "$uid" -g "$uid" "$MNT/home/alarm/.ssh"
	install -m 600 -o "$uid" -g "$uid" "$SSH_PUBKEY" "$MNT/home/alarm/.ssh/authorized_keys"
fi
log "done"
