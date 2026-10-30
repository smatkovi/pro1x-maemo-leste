#!/bin/bash
# The notify_rpm test from the build host, with the phone's owner there
# (docs/power-management.md, "The notify_rpm test"): copies
# tools/pro1x-rpmtest (and pro1x-wakeups) to the phone, starts one test
# detached from the SSH session, waits until the phone answers again and
# fetches the log.
#
#   PHONE="ssh ... root@<address>" bash scripts/69-rpm-test.sh [SECONDS] [rpm|norpm] [bt-down|bt-up] [sensors] [btwake]
#
# The arguments are pro1x-rpmtest's (default 60 rpm bt-down). Over Wi-Fi
# with the cable out: before kernel patch 0032 (pro1x.10) the USB gadget did
# not come back from a suspend, so a test over USB lost the connection until
# a reboot. Logs to OUTDIR
# (default ~/ps/pro1x-power/<date>). If the phone does not answer a few
# minutes after the alarm, it did not wake up: holding the power key
# restarts it (notify_rpm is off after any restart), and the log of the
# test up to the suspend is on the phone (/host/pro1x-power/rpmtest-*.log):
# fetch it with FETCH=1 after the restart.
set -uo pipefail
PHONE=${PHONE:?set PHONE=<ssh command for root on the phone>}
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
SECS=${1:-60}
MODE=${2:-rpm}
BT=${3:-bt-down}
EXTRA="${*:4}"
OUTDIR=${OUTDIR:-$HOME/ps/pro1x-power/$(date +%F)}
mkdir -p "$OUTDIR"
ph() { timeout "${TO:-30}" $PHONE "$@"; }
log() { echo "$(date '+%T') >> $*" | tee -a "$OUTDIR/rpmtest-run.log"; }

fetch() {
	local f
	f=$(ph "ls -t /host/pro1x-power/rpmtest-*.log /var/log/pro1x-power/rpmtest-*.log 2>/dev/null | head -1")
	[ -n "$f" ] || { log "no rpmtest log on the phone"; return 1; }
	ph "cat $f" > "$OUTDIR/$(basename "$f")"
	log "$(basename "$f"): $(tail -2 "$OUTDIR/$(basename "$f")" | head -1)"
}

if [ "${FETCH:-0}" = 1 ]; then
	fetch
	exit
fi

for t in pro1x-rpmtest pro1x-wakeups; do
	ph "cat > /usr/local/bin/$t && chmod 755 /usr/local/bin/$t" < "$REPO/tools/$t/$t" ||
		{ log "the phone does not answer"; exit 1; }
done
log "kernel $(ph uname -r), pro1x-rpmtest $SECS $MODE $BT $EXTRA"
ph "setsid nohup /usr/local/bin/pro1x-rpmtest $SECS $MODE $BT $EXTRA > /run/pro1x-rpmtest.out 2>&1 < /dev/null &"
log "suspending for $SECS s (the RTC alarm wakes it)"
sleep $((SECS + 15))
for i in $(seq 24); do
	if TO=10 ph "! pgrep -f '[p]ro1x-rpmtest' > /dev/null" 2> /dev/null; then
		fetch
		exit
	fi
	sleep 10
done
log "the phone does not answer $((SECS + 255)) s after the start: it did not wake up."
log "Hold the power key until it restarts (notify_rpm is off then), then: FETCH=1 PHONE=... bash $0"
exit 1
