#!/bin/bash
# Runs on the BUILD HOST, detached in the background.
#
# Tests the Leste boot image from RAM (fastboot boot): no write, no slot
# flags, no risk for Sailfish. Afterwards sets up the PC side of the USB
# gadget link and tests SSH.
#
# If it fails, a normal reboot is enough -- slot A stays active.
set -uo pipefail
IMG=${IMG:?set IMG=/path/boot-leste.img}
PC_ADDR=${PC_ADDR:-172.16.42.2/24}
PHONE=${PHONE:-172.16.42.1}
WAIT_FASTBOOT=${WAIT_FASTBOOT:-300}
WAIT_LINK=${WAIT_LINK:-150}
WAIT_SSH=${WAIT_SSH:-60}

log() { echo "$(date +%H:%M:%S) $*"; }

[ -f "$IMG" ] || { log "ABORT: $IMG missing"; exit 1; }

log "waiting for a fastboot device"
dev=""
for _ in $(seq 1 "$WAIT_FASTBOOT"); do
  dev=$(fastboot devices 2>/dev/null)
  [ -n "$dev" ] && break
  sleep 2
done
[ -n "$dev" ] || { log "ABORT: no fastboot device"; exit 1; }
log "found: $dev"
log "active slot stays: $(fastboot getvar current-slot 2>&1 | head -1)"

log "booting from RAM: $IMG"
fastboot boot "$IMG" 2>&1 | sed 's/^/  /'
rc=${PIPESTATUS[0]}
log "fastboot boot rc=$rc"
if [ "$rc" != 0 ]; then
  log "the ABL rejected the image, or 'fastboot boot' is not supported."
  log "Reboot the device normally, slot A is still active and unchanged."
  exit 1
fi

log "waiting for the USB gadget interface (enx*/usb*)"
IF=""
for _ in $(seq 1 "$WAIT_LINK"); do
  IF=$(ip -o link show 2>/dev/null | awk -F': ' '$2 ~ /^(enx|usb)/ {print $2; exit}')
  [ -n "$IF" ] && break
  sleep 2
done
if [ -z "$IF" ]; then
  log "no gadget interface -- the kernel does not get far."
  log "Check the screen, serial console: screen /dev/ttyACM0 115200"
  exit 1
fi
log "interface: $IF"
sudo -n ip addr add "$PC_ADDR" dev "$IF" 2>/dev/null || log "(address was already set)"
sudo -n ip link set "$IF" up
ip -4 -o addr show dev "$IF" | sed 's/^/  /'

log "waiting for SSH to $PHONE"
for _ in $(seq 1 "$WAIT_SSH"); do
  if ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 -o BatchMode=yes \
       "root@$PHONE" 'echo REACHABLE; uname -a; head -1 /etc/os-release; [ -d /host ] && echo "/host present (backing mount)"' 2>/dev/null; then
    log "Maemo Leste is running (RAM boot) and reachable via SSH."
    log "Next checks: dmesg | grep -iE 'aw9523|keypad|loop0', ip a, df -h"
    exit 0
  fi
  sleep 5
done
log "no SSH, but the gadget is there: the kernel runs, userspace hangs."
log "Serial console: screen /dev/ttyACM0 115200"
exit 1
