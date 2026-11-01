#!/bin/bash
# FreeBSD from the boot menu N times, unattended, for a fault that comes
# now and then (docs/device-day-2026-10-31.md block 12.6: the hang in
# bufinit). From the PC, with Leste running:
#
#   fbsd-boottest.sh RUN N "CMDLINE"
#
# Each start: CMDLINE (with pro1x.root=ram, which reboots by itself after
# its report) into /host/freebsd/cmdline, the RAM console cleared, one
# FreeBSD start, then either Leste again (the RAM console says how far it
# got) or 900e, the APSS watchdog's bite: then a dump with both halves of
# the first DDR rank (DDRCS0_0 and DDRCS0_1, PA 0x40000000-0x13fffffff,
# where FreeBSD's buffer headers are), which resets the phone into Leste.
# Logs in $RUNS/RUN-<i>; the old cmdline comes back at the end.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
run=${1:?usage: fbsd-boottest.sh RUN N CMDLINE}
n=${2:?usage: fbsd-boottest.sh RUN N CMDLINE}
cmd=${3:?usage: fbsd-boottest.sh RUN N CMDLINE}
SYS=leste
PHONE_SH="$(dirname "$0")/phone.sh"
QDL_FILTER='OCIMEM*,CODERAM*,DATARAM*,MSGRAM*,PMIC*,PMON*,RST*,FSM*,*.cmm,DDRCS0_0*,DDRCS0_1*'
stamp() { date +%H:%M:%S; }
up() { bash "$PHONE_SH" usbnet > /dev/null 2>&1 || true; CONNECT_TIMEOUT=5 ph true 2> /dev/null; }

old=$(ph 'cat /host/freebsd/cmdline')
ph "printf '%s\n' '$cmd' > /host/freebsd/cmdline && sync"
log "$(stamp) cmdline: $cmd (was: $old)"
ok=0 bit=0 other=0
for i in $(seq "$n"); do
	d=$RUNS/$run-$i
	mkdir -p "$d"
	ph 'python3 /root/devday/ramconsole.py --clear > /dev/null 2>&1; /usr/local/sbin/pro1x-boot next freebsd && sync; reboot' || true
	log "$(stamp) start $i of $n"
	sleep 20
	res=
	for t in $(seq 120); do
		if lsusb -d 05c6:900e > /dev/null 2>&1; then
			log "$(stamp) start $i: 900e, dump into $d"
			sudo -n qdl ramdump -o "$d" "$QDL_FILTER" > "$d/qdl.log" 2>&1 ||
			    die "qdl ramdump failed; the phone is still in 900e"
			res=bitten
			break
		fi
		if up; then
			res=back
			break
		fi
		sleep 5
	done
	[ -n "$res" ] || die "start $i: neither Leste nor 900e in 10 minutes"
	for t in $(seq 60); do up && break; sleep 5; done
	ph 'cd /root/devday && python3 ramconsole.py' > "$d/ramconsole.txt" 2>&1 || true
	if [ "$res" = bitten ]; then
		bit=$((bit + 1))
	elif grep -aq 'qcom_wdt0: \|GEOM: new disk' "$d/ramconsole.txt"; then
		ok=$((ok + 1))
		res="back, past bufinit (qcom_wdt0 or GEOM in the console)"
	else
		other=$((other + 1))
		res="back, RAM root not seen"
	fi
	log "$(stamp) start $i: $res ($(tail -c 300 "$d/ramconsole.txt" | tr -d '\r\0' | tail -n 1))"
done
ph "printf '%s\n' '$old' > /host/freebsd/cmdline && sync"
log "$(stamp) $n starts: $ok got past bufinit, $bit bitten, $other otherwise; cmdline back to: $old"
