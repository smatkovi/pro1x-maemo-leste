#!/bin/bash
# The PC's way to the phone on the device day (docs/device-day-2026-10-31.md).
#
#   phone.sh usbnet          the PC's address 172.16.42.2/24 on the USB link
#                            (enp19s0u1u1), which every re-enumeration of
#                            the phone drops: nmcli dev modify, no sudo
#   phone.sh pin SYS         pin SYS's host key once: ssh with accept-new into
#                            $KNOWN_DIR/known_hosts.pro1x-SYS only, then the
#                            fingerprints it holds. Only on the USB cable.
#   phone.sh keys            the pinned files and their fingerprints
#   phone.sh which           the system that answers (pro1x-boot current),
#                            tried with each pinned file
#   phone.sh wait [MIN]      until a system answers (default 10 minutes),
#                            usbnet again every 20 s
#   phone.sh ssh SYS [CMD]   a shell (or CMD) in SYS, with its pinned key
#
# SYS: leste meego kali arch luneos freebsd n950. N950: N950_IP (default
# 192.168.2.15, the PC gets 192.168.2.100 by DHCP).
set -euo pipefail
. "$(dirname "$0")/lib.sh"

case "${1:-}" in
usbnet)
	if ! ip link show "$USBIF" > /dev/null 2>&1; then
		log "$USBIF is not there (phone not on the cable, or still booting)"
		exit 1
	fi
	if ip -4 addr show "$USBIF" | grep -q '172\.16\.42\.2/'; then
		log "$USBIF has 172.16.42.2 already"
	else
		[ "$DRY" = 1 ] && { echo "[dry] nmcli dev modify $USBIF +ipv4.addresses 172.16.42.2/24"; exit 0; }
		nmcli dev modify "$USBIF" +ipv4.addresses 172.16.42.2/24
	fi
	ip -br addr show "$USBIF"
	;;
pin)
	s=${2:?usage: phone.sh pin SYS}
	kf=$(known_file "$s")
	if [ -s "$kf" ]; then
		log "$kf has keys already:"
		ssh-keygen -l -f "$kf"
		log "a changed key is refused from now on; remove the file by hand only if the system was set up anew"
		exit 0
	fi
	[ "$s" = n950 ] || ip -4 addr show "$USBIF" 2> /dev/null | grep -q '172\.16\.42\.2/' ||
	    die "pin only over the USB cable: run phone.sh usbnet first"
	mkdir -p "$KNOWN_DIR"
	[ "$DRY" = 1 ] && { echo "[dry] ssh accept-new into $kf"; exit 0; }
	$SSH -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new \
	    -o UserKnownHostsFile="$kf" "$(sys_user "$s")@$(sys_host "$s")" true
	ssh-keygen -l -f "$kf"
	;;
keys)
	for f in "$KNOWN_DIR"/known_hosts.pro1x-*; do
		[ -f "$f" ] || continue
		echo "== ${f##*/}"
		ssh-keygen -l -f "$f" | sed 's/^/   /'
	done
	;;
which)
	for s in leste kali arch luneos freebsd; do
		[ -s "$(known_file "$s")" ] || continue
		if cur=$(SYS=$s CONNECT_TIMEOUT=5 ph 'cat $R/host/pro1x-boot/current 2>/dev/null || uname -s' 2> /dev/null); then
			echo "$s (pro1x-boot current: ${cur:-?})"
			exit 0
		fi
	done
	echo "none answers (wrong system's key, not up, or usbnet missing)"
	exit 1
	;;
wait)
	min=${2:-10}
	for i in $(seq $((min * 3))); do
		"$0" usbnet > /dev/null 2>&1 || true
		if "$0" which; then exit 0; fi
		sleep 20
	done
	exit 1
	;;
ssh)
	s=${2:?usage: phone.sh ssh SYS [CMD]}
	shift 2
	mapfile -t opts < <(ssh_opts "$s")
	opts=("${opts[@]/BatchMode=yes/BatchMode=no}")
	exec $SSH "${opts[@]}" "$(sys_user "$s")@$(sys_host "$s")" "$@"
	;;
*)
	sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
	exit 1
	;;
esac
