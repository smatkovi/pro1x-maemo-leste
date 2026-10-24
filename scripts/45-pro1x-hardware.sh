#!/bin/bash
# Richtet die Pro1X-Hardwareunterstuetzung in einem Maemo-Leste-Rootfs ein.
#
# Laeuft als root IM Zielsystem: im Chroot aus 40-make-rootfs.sh, oder direkt
# auf dem laufenden Telefon. Braucht Netz fuer apt.
#
#   NIXOS=/pfad/zu/fxtec-pro1x-nixos ./45-pro1x-hardware.sh
#
# NIXOS zeigt auf einen Checkout von github.com/nyymi609/fxtec-pro1x-nixos
# (20-fetch-sources.sh legt ihn unter $WORK/build/nixos-pro1x an). Daraus
# kommen die UCM-Profile und Audio-Helfer; das Repo hat keine Lizenzdatei,
# darum werden die Dateien nicht hier eingecheckt, sondern beim Bau kopiert.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
NIXOS=${NIXOS:?NIXOS=<checkout von fxtec-pro1x-nixos> setzen}
export DEBIAN_FRONTEND=noninteractive

log() { echo ">> $*"; }

# --- Paketquellen ----------------------------------------------------------
# Lestes Mesa 22.3 kennt den Adreno 610 nicht (FD610 erst ab Mesa 23);
# daedalus-backports hat 25.0. Dasselbe gilt fuer linux-firmware: erst der
# Stand 2025 bringt qca/cmbtfw12.tlv (WCN3950-Bluetooth) mit.
log "daedalus-backports mit Pins fuer mesa, libdrm, firmware-nonfree"
echo "deb https://pkgmaster.devuan.org/merged daedalus-backports main contrib non-free non-free-firmware" \
  > /etc/apt/sources.list.d/pro1x-backports.list
cat > /etc/apt/preferences.d/pro1x <<'EOF'
# Pro1X: Mesa >= 23 kennt den Adreno 610 (FD610); Lestes 22.3 nicht.
# Leste pinnt die eigenen Pakete auf 1001, darum hier 1002.
Package: src:mesa src:libdrm
Pin: release n=daedalus-backports
Pin-Priority: 1002

# linux-firmware-Stand 2025: WCN3950-BT (qca/cmbtfw12) und ath10k-Daten
Package: src:firmware-nonfree
Pin: release n=daedalus-backports
Pin-Priority: 1002

Package: *
Pin: release n=daedalus-backports
Pin-Priority: 100
EOF
apt-get update -q

log "Pakete: Mesa (GPU), Firmware, Modemdienste, WLAN/BT-Werkzeuge"
apt-get install -y -q \
  libgl1-mesa-dri libegl-mesa0 libgbm1 libglx-mesa0 mesa-libgallium libdrm2 \
  firmware-qcom-soc firmware-atheros wireless-regdb \
  rmtfs tqftpserv qrtr-tools \
  iw bluez gdisk mesa-utils

# --- Overlay: Init-Skripte, mce-Konfiguration ------------------------------
log "Overlay aus $REPO/rootfs-overlay"
cp -a "$REPO/rootfs-overlay/." /
chmod 755 /etc/init.d/pro1x-*
rc-update add pro1x-tqftpserv default
rc-update add pro1x-rmtfs default
rc-update add bluetooth default

# --- Audio: UCM-Profil und Helfer aus dem NixOS-Port -----------------------
log "UCM-Profil Pro1X"
U=/usr/share/alsa/ucm2
install -d "$U/Pro1X" "$U/sm4250"
install -m 644 "$NIXOS"/device/audio/ucm2/Pro1X/*.conf "$U/Pro1X/"
# alsa-lib 1.2.8 sucht bei Kartennummer unter ucm2/<Treiber>/<Treiber>.conf
# (Treiber sm4250); nur auf diesem Weg ist ${CardId} definiert.
ln -sfn ../Pro1X/Pro1X.conf "$U/sm4250/sm4250.conf"
install -m 755 "$NIXOS/device/audio/pro1x-audio-init" /usr/bin/
install -m 755 "$NIXOS/device/audio/pro1x-voice-call" /usr/bin/

log "fertig"
