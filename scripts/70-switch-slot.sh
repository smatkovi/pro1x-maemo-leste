#!/bin/bash
# Boot-Slot umschalten -- laeuft auf dem PC, Geraet haengt im Bootloader am USB.
#
# Auf diesem Port ist kein bootctl installiert, darum geht das Umschalten nur
# per fastboot. Geraet vorher per "reboot bootloader" (oder Tastenkombination)
# in den Bootloader bringen.
set -euo pipefail
SLOT=${1:-}
case "$SLOT" in
  a|b) ;;
  *) echo "Nutzung: $0 a|b"; exit 1 ;;
esac

echo ">> aktueller Zustand"
fastboot getvar current-slot 2>&1 | head -2
echo ">> setze Slot $SLOT aktiv"
fastboot --set-active="$SLOT"
fastboot getvar current-slot 2>&1 | head -1
echo ">> reboot"
fastboot reboot
