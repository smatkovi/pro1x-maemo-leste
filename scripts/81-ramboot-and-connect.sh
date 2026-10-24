#!/bin/bash
# Laeuft auf dem BUILD-HOST, losgelöst im Hintergrund.
#
# Testet das Leste-Boot-Image aus dem RAM (fastboot boot): kein Schreibvorgang,
# keine Slot-Flags, kein Risiko fuer Sailfish. Danach die PC-Seite des
# USB-Gadget-Links einrichten und SSH testen.
#
# Scheitert es, reicht ein normaler Neustart -- Slot A bleibt aktiv.
set -uo pipefail
IMG=${IMG:?IMG=/pfad/boot-leste.img setzen}
PC_ADDR=${PC_ADDR:-172.16.42.2/24}
PHONE=${PHONE:-172.16.42.1}
WAIT_FASTBOOT=${WAIT_FASTBOOT:-300}
WAIT_LINK=${WAIT_LINK:-150}
WAIT_SSH=${WAIT_SSH:-60}

log() { echo "$(date +%H:%M:%S) $*"; }

[ -f "$IMG" ] || { log "ABBRUCH: $IMG fehlt"; exit 1; }

log "warte auf fastboot-Geraet"
dev=""
for _ in $(seq 1 "$WAIT_FASTBOOT"); do
  dev=$(fastboot devices 2>/dev/null)
  [ -n "$dev" ] && break
  sleep 2
done
[ -n "$dev" ] || { log "ABBRUCH: kein fastboot-Geraet"; exit 1; }
log "gefunden: $dev"
log "aktiver Slot bleibt: $(fastboot getvar current-slot 2>&1 | head -1)"

log "booten aus dem RAM: $IMG"
fastboot boot "$IMG" 2>&1 | sed 's/^/  /'
rc=${PIPESTATUS[0]}
log "fastboot boot rc=$rc"
if [ "$rc" != 0 ]; then
  log "ABL hat das Image abgelehnt, oder 'fastboot boot' wird nicht unterstuetzt."
  log "Geraet normal neu starten, Slot A ist unveraendert aktiv."
  exit 1
fi

log "warte auf USB-Gadget-Interface (enx*/usb*)"
IF=""
for _ in $(seq 1 "$WAIT_LINK"); do
  IF=$(ip -o link show 2>/dev/null | awk -F': ' '$2 ~ /^(enx|usb)/ {print $2; exit}')
  [ -n "$IF" ] && break
  sleep 2
done
if [ -z "$IF" ]; then
  log "kein Gadget-Interface -- der Kernel kommt nicht weit."
  log "Bildschirm pruefen, serielle Konsole: screen /dev/ttyACM0 115200"
  exit 1
fi
log "Interface: $IF"
sudo -n ip addr add "$PC_ADDR" dev "$IF" 2>/dev/null || log "(Adresse war schon gesetzt)"
sudo -n ip link set "$IF" up
ip -4 -o addr show dev "$IF" | sed 's/^/  /'

log "warte auf SSH nach $PHONE"
for _ in $(seq 1 "$WAIT_SSH"); do
  if ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 -o BatchMode=yes \
       "root@$PHONE" 'echo ERREICHBAR; uname -a; head -1 /etc/os-release; [ -d /host ] && echo "/host vorhanden (Backing-Mount)"' 2>/dev/null; then
    log "Maemo Leste laeuft (RAM-Boot) und ist per SSH erreichbar."
    log "Naechste Checks: dmesg | grep -iE 'aw9523|keypad|loop0', ip a, df -h"
    exit 0
  fi
  sleep 5
done
log "kein SSH, aber das Gadget ist da: Kernel laeuft, Userspace haengt."
log "Serielle Konsole: screen /dev/ttyACM0 115200"
exit 1
