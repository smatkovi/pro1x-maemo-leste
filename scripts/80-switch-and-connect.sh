#!/bin/bash
# Laeuft auf dem BUILD-HOST, losgelöst im Hintergrund.
#
# Wartet auf das Geraet im Bootloader, schaltet Slot B aktiv, bootet, richtet
# die PC-Seite des USB-Gadget-Links ein und testet SSH nach Maemo Leste.
#
# Hintergrund: die Steuer-Session laeuft auf dem Telefon selbst und endet mit
# dem Reboot. Alles, was danach passieren muss, steht deshalb hier.
#
# Rueckweg jederzeit:  ./70-switch-slot.sh a
set -uo pipefail
SLOT=${SLOT:-b}
PC_ADDR=${PC_ADDR:-172.16.42.2/24}
PHONE=${PHONE:-172.16.42.1}
WAIT_FASTBOOT=${WAIT_FASTBOOT:-300}   # x2 s
WAIT_LINK=${WAIT_LINK:-150}           # x2 s
WAIT_SSH=${WAIT_SSH:-60}              # x5 s

log() { echo "$(date +%H:%M:%S) $*"; }

log "warte auf fastboot-Geraet (bis $((WAIT_FASTBOOT * 2)) s)"
dev=""
for _ in $(seq 1 "$WAIT_FASTBOOT"); do
  dev=$(fastboot devices 2>/dev/null)
  [ -n "$dev" ] && break
  sleep 2
done
[ -n "$dev" ] || { log "ABBRUCH: kein fastboot-Geraet gefunden"; exit 1; }
log "gefunden: $dev"

log "aktueller Slot: $(fastboot getvar current-slot 2>&1 | head -1)"
fastboot --set-active="$SLOT" 2>&1 | sed 's/^/  /'
log "jetzt aktiv:    $(fastboot getvar current-slot 2>&1 | head -1)"
fastboot reboot 2>&1 | sed 's/^/  /'
log "reboot ausgeloest -- das Geraet sollte jetzt Slot $SLOT booten"

log "warte auf USB-Gadget-Interface (enx*/usb*)"
IF=""
for _ in $(seq 1 "$WAIT_LINK"); do
  IF=$(ip -o link show 2>/dev/null | awk -F': ' '$2 ~ /^(enx|usb)/ {print $2; exit}')
  [ -n "$IF" ] && break
  sleep 2
done
if [ -z "$IF" ]; then
  log "kein Gadget-Interface aufgetaucht. Entweder bootet Slot $SLOT nicht,"
  log "oder der Gadget kommt nicht hoch. Bildschirm pruefen; Rueckweg:"
  log "  fastboot --set-active=a"
  exit 1
fi
log "Interface: $IF"

sudo -n ip addr add "$PC_ADDR" dev "$IF" 2>/dev/null || log "(Adresse war schon gesetzt)"
sudo -n ip link set "$IF" up
ip -4 -o addr show dev "$IF" | sed 's/^/  /'

log "warte auf SSH nach $PHONE"
for _ in $(seq 1 "$WAIT_SSH"); do
  if ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 -o BatchMode=yes \
       "root@$PHONE" 'echo ERREICHBAR; uname -a; cat /etc/os-release | head -1' 2>/dev/null; then
    log "Maemo Leste laeuft und ist per SSH erreichbar."
    log "Pruefpunkte: dmesg | grep -iE 'aw9523|keypad', ip a, /host (Sailfish-Rootfs)"
    exit 0
  fi
  sleep 5
done
log "kein SSH. Interface ist da, also bootet etwas -- vermutlich haengt es"
log "frueher (Loop-Root?). Serielle Konsole: screen /dev/ttyACM0 115200"
exit 1
