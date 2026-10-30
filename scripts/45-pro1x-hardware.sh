#!/bin/bash
# Sets up the Pro1X hardware support in a Maemo Leste rootfs.
#
# Runs as root INSIDE the target system: in the chroot from 40-make-rootfs.sh,
# or directly on the running phone. Needs network access for apt.
#
#   NIXOS=/path/to/fxtec-pro1x-nixos ./45-pro1x-hardware.sh
#
# NIXOS points at a checkout of github.com/nyymi609/fxtec-pro1x-nixos
# (20-fetch-sources.sh puts it under $WORK/build/nixos-pro1x). The UCM profiles
# and audio helpers come from there; that repo has no license file, so the
# files are not checked in here but copied at build time.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
NIXOS=${NIXOS:?set NIXOS=<checkout of fxtec-pro1x-nixos>}
export DEBIAN_FRONTEND=noninteractive

log() { echo ">> $*"; }

# --- Package sources -------------------------------------------------------
# Leste's Mesa 22.3 does not know the Adreno 610 (FD610 only from Mesa 23 on);
# daedalus-backports has 25.0. The same goes for linux-firmware: only the 2025
# snapshot brings qca/cmbtfw12.tlv (WCN3950 Bluetooth).
log "daedalus-backports with pins for mesa, libdrm, firmware-nonfree"
echo "deb https://pkgmaster.devuan.org/merged daedalus-backports main contrib non-free non-free-firmware" \
  > /etc/apt/sources.list.d/pro1x-backports.list
cat > /etc/apt/preferences.d/pro1x <<'EOF'
# Pro1X: Mesa >= 23 knows the Adreno 610 (FD610); Leste's 22.3 does not.
# Leste pins its own packages at 1001, hence 1002 here.
Package: src:mesa src:libdrm
Pin: release n=daedalus-backports
Pin-Priority: 1002

# linux-firmware 2025 snapshot: WCN3950 BT (qca/cmbtfw12) and ath10k data
Package: src:firmware-nonfree
Pin: release n=daedalus-backports
Pin-Priority: 1002

Package: *
Pin: release n=daedalus-backports
Pin-Priority: 100
EOF
apt-get update -q

log "packages: Mesa (GPU), firmware, modem services, Wi-Fi/BT tools"
apt-get install -y -q \
  libgl1-mesa-dri libegl-mesa0 libgbm1 libglx-mesa0 mesa-libgallium libdrm2 \
  firmware-qcom-soc firmware-atheros wireless-regdb \
  rmtfs tqftpserv qrtr-tools \
  iw bluez gdisk mesa-utils \
  xinput debootstrap debian-archive-keyring

# kexec for the boot menu (systems with a kernel of their own: FreeBSD).
# The package would otherwise take over every reboot with kexec (its debconf
# default), bypassing the bootloader.
log "kexec-tools (only for the boot menu)"
printf 'kexec-tools kexec-tools/load_kexec boolean false\nkexec-tools kexec-tools/use_grub_config boolean false\n' |
  debconf-set-selections
apt-get install -y -q --no-install-recommends kexec-tools
grep -qx 'LOAD_KEXEC=false' /etc/default/kexec
# Nor its init scripts: OpenRC takes kexec's "Required-Stop: reboot" for
# "need reboot", so the reboot script starts with it at boot, and leaving
# the default runlevel stops kexec and then reboot, whose stop is
# "reboot -f": every reboot came before sendsigs, umountfs and umountroot,
# with the root image still writable and dsme writing to it after the UFS
# disk was off (journal aborted, image "clean with errors"; 2026-10-29).
rc-update del kexec-load default 2> /dev/null || true
rc-update del kexec default 2> /dev/null || true

# --- Upgrades ----------------------------------------------------------------
# apt upgrade / dist-upgrade must not undo the port: configuration files it
# changed stay (dpkg would otherwise ask, or a front end would replace them),
# and Debian's kernel meta packages go, since the phone boots its own kernel
# from the boot partition and never Debian's.
log "apt: keep changed configuration files, no Debian kernels"
cat > /etc/apt/apt.conf.d/50pro1x-upgrades <<'EOF'
// Pro1X: upgrades keep configuration files changed on the phone and take
// the package's new version of the unchanged ones
Dpkg::Options { "--force-confdef"; "--force-confold"; };
EOF
for p in linux-image-arm64 linux-headers-arm64; do
  if dpkg -s "$p" > /dev/null 2>&1; then apt-get remove -y -q "$p"; fi
done

# upower without battery polling: pro1x-adc-battery measures every 5 s and
# sends a uevent itself whenever capacity or status change (at most every
# 30 s), while upowerd read the ADC again every few seconds. UPower.conf
# belongs to leste-config-common (a diversion), so upowerd gets its own copy
# through a D-Bus service file, which takes precedence over the package's.
log "upower: no battery polling"
sed 's/^NoPollBatteries=.*/NoPollBatteries=true/' /etc/UPower/UPower.conf > /etc/UPower/UPower-pro1x.conf
mkdir -p /usr/local/share/dbus-1/system-services
cat > /usr/local/share/dbus-1/system-services/org.freedesktop.UPower.service <<'EOF'
# Pro1X: upowerd with /etc/UPower/UPower-pro1x.conf (no battery polling);
# this directory takes precedence over /usr/share/dbus-1/system-services
[D-BUS Service]
Name=org.freedesktop.UPower
Exec=/usr/bin/env UPOWER_CONF_FILE_NAME=/etc/UPower/UPower-pro1x.conf /usr/libexec/upowerd
User=root
SystemdService=upower.service
EOF

# --- Overlay: init scripts, mce configuration ------------------------------
log "overlay from $REPO/rootfs-overlay"
cp -a "$REPO/rootfs-overlay/." /
chmod 755 /etc/init.d/pro1x-*
rc-update add pro1x-tqftpserv default
rc-update add pro1x-rmtfs default
# before oFono: the modem's rmnet_ipa0 and QMI services. oFono makes its
# own rmnet links once scripts/49-mobile-data.sh rebuilt it; the stock
# oFono without pro1x-rmnet's rmnet_data links has no modem at all, so 45
# alone (without 49-mobile-data) leaves the phone without calls
rc-update add pro1x-modem-wait default
rc-update add pro1x-cal default
rc-update add bluetooth default

# --- Audio: UCM profile and helpers from the NixOS port --------------------
log "UCM profile Pro1X"
U=/usr/share/alsa/ucm2
install -d "$U/Pro1X" "$U/sm4250"
install -m 644 "$NIXOS"/device/audio/ucm2/Pro1X/*.conf "$U/Pro1X/"
# When opened by card number, alsa-lib 1.2.8 looks under
# ucm2/<driver>/<driver>.conf (driver sm4250); only on that path is ${CardId}
# defined.
ln -sfn ../Pro1X/Pro1X.conf "$U/sm4250/sm4250.conf"
install -m 755 "$NIXOS/device/audio/pro1x-audio-init" /usr/bin/
# The voice session of a call: started by the "Voice Call" UCM verb, which
# sphone's route-pulseaudio module switches to during a call. It follows
# the calls through ofono here (Leste has no ModemManager:
# patches/nixos-audio), and keeps the DSP's VoiceMMode1 PCM open
# (pro1x-voicepcm, which Leste lacked: no call audio)
patch -s -o /usr/bin/pro1x-voice-call "$NIXOS/device/audio/pro1x-voice-call" \
	"$REPO/patches/nixos-audio/0001-voice-call-ofono.patch"
chmod 755 /usr/bin/pro1x-voice-call
apt-get install -y -q --no-install-recommends gcc libc6-dev libasound2-dev
gcc -O2 -Wall -o /usr/bin/pro1x-voicepcm "$NIXOS/device/audio/voicepcm.c" -lasound

# --- Boot menu (initramfs) and pro1x-boot ----------------------------------
# The initramfs hook copies both into the initramfs; pro1x-boot also serves
# the running system (next/default system, slot switch to Sailfish).
log "boot menu"
apt-get install -y -q --no-install-recommends gcc libc6-dev
gcc -O2 -Wall -o /usr/local/sbin/pro1x-bootmenu "$REPO/tools/pro1x-bootmenu/pro1x-bootmenu.c"
install -m 755 "$REPO/tools/pro1x-boot/pro1x-boot" /usr/local/sbin/
# a new kernel's trial start (pro1x-boot trial): confirmed once Hildon runs
install -m 755 "$REPO/tools/pro1x-boot-trial/pro1x-boot-trial" /usr/local/sbin/
install -m 755 "$REPO/tools/pro1x-boot-trial/pro1x-boot-trial.openrc" /etc/init.d/pro1x-boot-trial
rc-update add pro1x-boot-trial default
# FreeBSD's writable ZFS pool in a file on userdata (freebsd/README.md)
install -m 755 "$REPO/tools/pro1x-freebsd-pool/pro1x-freebsd-pool" /usr/local/sbin/

# --- MeeGo Harmattan (meego/) -----------------------------------------------
# The boot menu entry "MeeGo Harmattan" starts this image without Leste's
# desktop (Xsession.d/01pro1x-meego); pro1x-meego then runs Harmattan's UI
# from /host/harmattan.img (built with meego/20..37, see meego/README.md).
log "MeeGo Harmattan session"
install -D -m 755 "$REPO/meego/40-harmattan-session.sh" /usr/lib/pro1x-meego/harmattan-session
mkdir -p /mnt/harmattan
rc-update add pro1x-meego default

log "done"
