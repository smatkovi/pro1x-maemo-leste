#!/bin/bash
# The standby power of the Linux systems on the phone, unattended, from the
# PC the phone hangs on: for each system, boot it (pro1x-boot next), start
# tools/pro1x-powerlog in it, take the USB power away (the PC's hub port),
# wait, give it back and fetch the log.
#
#   PORT=/sys/bus/usb/devices/7-1:1.0/7-1-port1 MINUTES=30 \
#       bash scripts/68-power-standby.sh leste meego arch luneos
#
# The phone runs Leste (or any of them) when it starts. Per system: HOST_<key>
# to log in to (default root@172.16.42.1), KNOWN_<key> its known hosts file
# (default KNOWN: every system has host keys of its own) and SUDO_<key> in
# front of root's commands (e.g. "sudo -n" for Arch's alarm). The display is switched off where the system has a way
# (mce on Leste and MeeGo, LuneOS's display service), elsewhere it blanks by
# itself; the log says which samples had it off. Logs to OUTDIR
# (default ./power-<date>).
set -uo pipefail
PORT=${PORT:?set PORT=<the sysfs directory of the hub port>}
MINUTES=${MINUTES:-30}
USBIF=${USBIF:-enp19s0u1u1}
OUTDIR=${OUTDIR:-power-$(date +%Y%m%d-%H%M)}
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
mkdir -p "$OUTDIR"
log() { echo "$(date '+%T') >> $*" | tee -a "$OUTDIR/run.log"; }

var() { local v="${1}_$2"; echo "${!v:-${3:-}}"; }
sshc() {    # sshc KEY: the ssh command for that system
	echo "ssh -o BatchMode=yes -o ConnectTimeout=10" \
	    "-o UserKnownHostsFile=$(var KNOWN "$1" "${KNOWN:-$HOME/.ssh/known_hosts}")" \
	    "$(var HOST "$1" root@172.16.42.1)"
}
on() {      # on KEY COMMAND: as root in that system (the command on stdin)
	local k=$1
	shift
	printf '%s\n' "$*" | timeout 60 $(sshc "$k") "$(var SUDO "$k") sh -s"
}
port() { sudo -n sh -c "echo $1 > $PORT/disable"; }   # 0 on, 1 off
# the PC's address on the USB network, which every re-enumeration drops
usbnet() {
	ip link show "$USBIF" > /dev/null 2>&1 || return 0
	ip -4 addr show "$USBIF" | grep -q 172.16.42.2 ||
	    sudo -n ip addr add 172.16.42.2/24 dev "$USBIF" 2> /dev/null || true
}
wait_for() {    # wait_for KEY: until that system answers (10 minutes)
	for i in $(seq 60); do
		usbnet
		[ "$(on "$1" /usr/local/sbin/pro1x-boot current 2> /dev/null)" = "$1" ] && return 0
		sleep 10
	done
	return 1
}
which_runs() {
	for k in leste "$@"; do
		cur=$(on "$k" /usr/local/sbin/pro1x-boot current 2> /dev/null) && { echo "$cur"; return; }
	done
}

current=
for i in $(seq 12); do
	usbnet
	current=$(which_runs "$@")
	[ -n "$current" ] && break
	sleep 10
done
[ -n "$current" ] || { echo "the phone does not answer"; exit 1; }
port 0
for k in "$@"; do
	if [ "$k" != "$current" ]; then
		log "$current -> $k"
		on "$current" "/usr/local/sbin/pro1x-boot next $k && sync && (reboot || systemctl reboot)" || true
		sleep 40
		wait_for "$k" || { log "$k does not come up: stop"; exit 1; }
		current=$k
	fi
	log "$k: logger for $MINUTES min"
	timeout 60 $(sshc "$k") "$(var SUDO "$k") sh -c 'cat > /usr/local/bin/pro1x-powerlog && chmod 755 /usr/local/bin/pro1x-powerlog'" \
	    < "$REPO/tools/pro1x-powerlog/pro1x-powerlog"
	on "$k" "setsid nohup /usr/local/bin/pro1x-powerlog $k $MINUTES 30 > /dev/null 2>&1 < /dev/null &"
	sleep 20
	case $k in
	leste|meego)
		on "$k" "dbus-send --system --type=method_call --dest=com.nokia.mce /com/nokia/mce/request com.nokia.mce.request.req_display_state_off" ;;
	luneos)
		on "$k" "luna-send -n 1 luna://com.palm.display/control/setState '{\"state\":\"off\"}'" ;;
	esac
	sleep 30
	log "$k: USB power off"
	port 1
	sleep $((MINUTES * 60))
	log "$k: USB power on"
	port 0
	sleep 30
	wait_for "$k" || { log "$k does not answer after the run: stop"; exit 1; }
	sleep 30
	f=$(on "$k" "ls -t /host/pro1x-power/$k-*.log 2>/dev/null | head -1")
	on "$k" "cat $f" > "$OUTDIR/$k.log"
	log "$k: $(tail -1 "$OUTDIR/$k.log")"
done
log "done: $OUTDIR"
