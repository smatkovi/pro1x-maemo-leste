#!/bin/bash
# Switches the boot slot -- runs on the PC, the device sits in the bootloader on USB.
#
# This port has no bootctl installed, so switching only works via fastboot.
# Bring the device into the bootloader first with "reboot bootloader" (or the
# key combination).
set -euo pipefail
SLOT=${1:-}
case "$SLOT" in
  a|b) ;;
  *) echo "Usage: $0 a|b"; exit 1 ;;
esac

echo ">> current state"
fastboot getvar current-slot 2>&1 | head -2
echo ">> setting slot $SLOT active"
fastboot --set-active="$SLOT"
fastboot getvar current-slot 2>&1 | head -1
echo ">> reboot"
fastboot reboot
