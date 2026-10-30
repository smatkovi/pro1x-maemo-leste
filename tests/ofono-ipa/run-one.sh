#!/bin/bash
# One case of the oFono IPA test, as root inside `unshare -m` (run.sh
# starts it). QRTR sockets exist only in the initial network namespace,
# so the dummy rmnet_ipa0 lives there (deleted at the end, with the rmnet
# links oFono made on it); the mount namespace keeps the private bus, the
# tmpfs for oFono's state and the bind mount into a chroot to this case.
#
# Environment (all set by run.sh):
#   W        the case's result directory (written here)
#   T        this directory (fakemodem.py, bus.conf)
#   SC       fakemodem.py's scenario
#   OFONOD   the ofonod to run (a path inside ROOT when ROOT is set)
#   ROOT     a chroot of Leste's image to run ofonod in, or empty
#   SHIM     sysfs-shim.so as ofonod sees it
#   FLAGS_TOOL  tools/pro1x-rmnet-flags
#   STALE=1  four rmnet_data links on rmnet_ipa0 before oFono starts
#   OFONO_IPA_WDA, OFONO_IPA_DPM, OFONO_IPA_RMNET_FLAGS  passed to ofonod
set -u
mount --make-rprivate /
ip link add rmnet_ipa0 type dummy || exit 1
PIDS=
cleanup() {
	# shellcheck disable=SC2086
	[ -z "$PIDS" ] || kill $PIDS 2> /dev/null
	[ ! -s "$W/dbus.pid" ] || kill "$(cat "$W/dbus.pid")" 2> /dev/null
	sleep 1
	ip link del rmnet_ipa0 2> /dev/null
}
trap cleanup EXIT

if [ "${STALE:-}" = 1 ]; then
	for n in 0 1 2 3; do
		ip link add link rmnet_ipa0 name "rmnet_data$n" type rmnet mux_id $((n + 1))
	done
fi

# what the mainline IPA driver has in sysfs on the Pro1X (IPA v4.2):
# ipa_sysfs.c, ipa_data-v4.2.c (AP_MODEM_RX endpoint 9, AP_MODEM_TX 1)
F=$W/fakedev
mkdir -p "$F/modem" "$F/feature"
echo 9 > "$F/modem/rx_endpoint_id"
echo 1 > "$F/modem/tx_endpoint_id"
echo MAPv4 > "$F/feature/rx_offload"
echo MAPv4 > "$F/feature/tx_offload"

# oFono keeps per-SIM state there; nothing of the PC's
if [ -n "$ROOT" ]; then
	mkdir -p "$ROOT$W" "$ROOT/var/lib/ofono"
	mount --bind "$W" "$ROOT$W"
	mount -t tmpfs tmpfs "$ROOT/var/lib/ofono"
else
	mount -t tmpfs tmpfs /var/lib
fi

sed "s|@SOCKET@|$W/bus|" "$T/bus.conf" > "$W/bus.conf"
dbus-daemon --config-file="$W/bus.conf" --fork --print-pid > "$W/dbus.pid"
sleep 0.5
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=$W/bus

python3 -I "$T/fakemodem.py" "$SC" > "$W/fake.log" 2>&1 &
PIDS="$PIDS $!"
sleep 1

envs=(OFONO_QMI_DEBUG=1 "LD_PRELOAD=$SHIM" "FAKE_DEVICE=$F"
	"DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SYSTEM_BUS_ADDRESS")
for v in OFONO_IPA_WDA OFONO_IPA_DPM OFONO_IPA_RMNET_FLAGS; do
	[ -z "${!v:-}" ] || envs+=("$v=${!v}")
done
cmd=(/usr/bin/env "${envs[@]}" "$OFONOD" -n -p udevng
	-d '*qrtrqmi*:*rmnet*:*udevng*')
if [ -n "$ROOT" ]; then
	chroot "$ROOT" "${cmd[@]}" > "$W/ofonod.log" 2>&1 &
else
	"${cmd[@]}" > "$W/ofonod.log" 2>&1 &
fi
PIDS="$PIDS $!"

sleep 3
dbus-send --system --print-reply --dest=org.ofono / \
	org.ofono.Manager.GetModems > "$W/modems.txt" 2>&1
dbus-send --system --print-reply --dest=org.ofono /qrtrsoc_0 \
	org.ofono.Modem.SetProperty string:Powered variant:boolean:true > "$W/power.txt" 2>&1
case $SC in silent) sleep 26 ;; *) sleep 6 ;; esac
dbus-send --system --print-reply --dest=org.ofono /qrtrsoc_0 \
	org.ofono.Modem.GetProperties > "$W/props.txt" 2>&1
python3 -I "$FLAGS_TOOL" > "$W/rmnet.txt" 2>&1
ip -br link > "$W/links.txt" 2>&1
