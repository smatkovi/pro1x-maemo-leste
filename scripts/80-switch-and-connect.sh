#!/bin/bash
# Runs on the BUILD HOST, detached in the background.
#
# Waits for the device in the bootloader, sets slot B active, boots, sets up
# the PC side of the USB gadget link and tests SSH into Maemo Leste.
#
# Background: the controlling session runs on the phone itself and ends with
# the reboot. Everything that has to happen afterwards is therefore in here.
#
# Way back at any time:  ./70-switch-slot.sh a
set -uo pipefail
SLOT=${SLOT:-b}
PC_ADDR=${PC_ADDR:-172.16.42.2/24}
PHONE=${PHONE:-172.16.42.1}
WAIT_FASTBOOT=${WAIT_FASTBOOT:-300}   # x2 s
WAIT_LINK=${WAIT_LINK:-150}           # x2 s
WAIT_SSH=${WAIT_SSH:-60}              # x5 s

log() { echo "$(date +%H:%M:%S) $*"; }

log "waiting for a fastboot device (up to $((WAIT_FASTBOOT * 2)) s)"
dev=""
for _ in $(seq 1 "$WAIT_FASTBOOT"); do
  dev=$(fastboot devices 2>/dev/null)
  [ -n "$dev" ] && break
  sleep 2
done
[ -n "$dev" ] || { log "ABORT: no fastboot device found"; exit 1; }
log "found: $dev"

log "current slot: $(fastboot getvar current-slot 2>&1 | head -1)"
fastboot --set-active="$SLOT" 2>&1 | sed 's/^/  /'
log "now active:   $(fastboot getvar current-slot 2>&1 | head -1)"
fastboot reboot 2>&1 | sed 's/^/  /'
log "reboot triggered -- the device should now boot slot $SLOT"

log "waiting for the USB gadget interface (enx*/usb*)"
IF=""
for _ in $(seq 1 "$WAIT_LINK"); do
  IF=$(ip -o link show 2>/dev/null | awk -F': ' '$2 ~ /^(enx|usb)/ {print $2; exit}')
  [ -n "$IF" ] && break
  sleep 2
done
if [ -z "$IF" ]; then
  log "no gadget interface showed up. Either slot $SLOT does not boot,"
  log "or the gadget does not come up. Check the screen; way back:"
  log "  fastboot --set-active=a"
  exit 1
fi
log "interface: $IF"

sudo -n ip addr add "$PC_ADDR" dev "$IF" 2>/dev/null || log "(address was already set)"
sudo -n ip link set "$IF" up
ip -4 -o addr show dev "$IF" | sed 's/^/  /'

log "waiting for SSH to $PHONE"
for _ in $(seq 1 "$WAIT_SSH"); do
  if ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 -o BatchMode=yes \
       "root@$PHONE" 'echo REACHABLE; uname -a; cat /etc/os-release | head -1' 2>/dev/null; then
    log "Maemo Leste is running and reachable via SSH."
    log "Things to check: dmesg | grep -iE 'aw9523|keypad', ip a, /host (Sailfish rootfs)"
    exit 0
  fi
  sleep 5
done
log "no SSH. The interface is there, so something boots -- it probably hangs"
log "earlier (loop root?). Serial console: screen /dev/ttyACM0 115200"
exit 1
