#!/bin/bash
# Probing a kernel with a flash that undoes itself (docs/crash-dumps.md,
# "Probing a kernel"): the phone's pro1x-probe service (tools/devday/
# pro1x-probe, OpenRC sysinit in Leste) puts the saved boot_b back about
# 10 s into the candidate's first start and ends its trial, so whatever the
# candidate does later, the next start is the saved kernel again, with the
# candidate's console in the pstore. From the PC, SYS=leste:
#
#   probe.sh install        the service into Leste (sysinit), /host/pro1x-probe,
#                           pro1x-probe-heartbeat into /usr/local/sbin
#   probe.sh arm KVER [S [stopwd]]
#                           boot_b now (the running kernel's image) saved as
#                           /host/pro1x-probe/restore.img; the service puts it
#                           back when KVER runs. With S: a heartbeat in the
#                           kernel log for S seconds at the next start; with
#                           stopwd it also stops the trial's watchdog once
#                           /dev/watchdog is there. Then flash KVER (65-kernel-
#                           update.sh flash, with its trial) and reboot. KVER =
#                           the running kernel is the dry run: the same bytes
#                           go back at the next start.
#   probe.sh status         arm, the log, the starts kept
#   probe.sh off            no arm, the service out of sysinit (logs stay)
#   probe.sh watch RUN KVER [MIN]
#                           after the reboot: until the phone is in 900e
#                           (qdl ramdump, which resets it) or a system answers.
#                           KVER up: watched MIN minutes (default 10), then
#                           collected and rebooted (into the saved kernel).
#                           Logs: collect.sh RUN (with the probe log; each
#                           start's pstore stays in /host/pro1x-probe/boot-*).
#                           Exit 0 KVER stayed up, 2 crashed into 900e, 3
#                           came back without KVER being seen, 1 no answer
#                           within 20 minutes.
#
# The way back the service cannot cover: a candidate that hangs before
# sysinit takes the trial's tries (3), then the bootloader starts slot a,
# Sailfish OS (its LUKS code needs the owner).
set -euo pipefail
. "$(dirname "$0")/lib.sh"
P=/host/pro1x-probe
QDL_FILTER=${QDL_FILTER:-'OCIMEM*,CODERAM*,DATARAM*,MSGRAM*,PMIC*,PMON*,RST*,FSM*,*.cmm'}
PHONE_SH="$(dirname "$0")/phone.sh"
stamp() { date +%H:%M:%S; }

case "${1:-}" in
install)
	ph "set -e; mkdir -p $P; cat > /etc/init.d/pro1x-probe.new; chmod 755 /etc/init.d/pro1x-probe.new
		mv /etc/init.d/pro1x-probe.new /etc/init.d/pro1x-probe
		rc-update add pro1x-probe sysinit > /dev/null; rc-update show sysinit | grep pro1x-probe" \
	    < "$(dirname "$0")/pro1x-probe"
	ph "cat > /usr/local/sbin/pro1x-probe-heartbeat.new && chmod 755 /usr/local/sbin/pro1x-probe-heartbeat.new &&
		mv /usr/local/sbin/pro1x-probe-heartbeat.new /usr/local/sbin/pro1x-probe-heartbeat" \
	    < "$(dirname "$0")/pro1x-probe-heartbeat"
	;;
arm)
	k=${2:?usage: probe.sh arm KVER [SECONDS [stopwd]]}
	hb=${3:-}
	case $hb in ''|*[!0-9]*) [ -z "$hb" ] || die "SECONDS: a number" ;; esac
	case ${4:-} in '') ;; stopwd) [ -n "$hb" ] || die "stopwd needs SECONDS"; hb="$hb stopwd" ;; *) die "4th argument: stopwd" ;; esac
	ph "set -e; [ -x /etc/init.d/pro1x-probe ] || { echo 'not installed: probe.sh install'; exit 1; }
		rc-update show sysinit | grep -q pro1x-probe || { echo 'not in sysinit'; exit 1; }
		[ ! -f /host/pro1x-boot/trial ] || { echo 'a trial runs: not arming'; exit 1; }
		dev=/dev/disk/by-partlabel/boot_b
		dd if=\$dev of=$P/restore.img.new bs=1M status=none; sync
		cmp -s $P/restore.img.new \$dev || { echo 'boot_b copy differs'; exit 1; }
		mv $P/restore.img.new $P/restore.img
		echo '$k $P/restore.img '\$(sha256sum < $P/restore.img | cut -d' ' -f1) > $P/arm
		rm -f $P/heartbeat; [ -z '$hb' ] || echo '$hb' > $P/heartbeat; sync
		echo \"armed: \$(cat $P/arm) (running \$(uname -r))${hb:+, heartbeat $hb s}\""
	;;
status)
	ph "cat $P/arm 2> /dev/null || echo 'not armed'; rc-update show sysinit | grep pro1x-probe || echo 'service not in sysinit'
		tail -n 12 $P/log 2> /dev/null; ls -d $P/boot-* 2> /dev/null | tail -n 5"
	;;
off)
	ph "rm -f $P/arm; rc-update del pro1x-probe sysinit > /dev/null 2>&1 || true; sync; echo off"
	;;
watch)
	run=${2:?usage: probe.sh watch RUN KVER [MIN]}
	k=${3:?usage: probe.sh watch RUN KVER [MIN]}
	min=${4:-10}
	seen=0 up_since=
	for i in $(seq 120); do
		if lsusb -d 05c6:900e > /dev/null 2>&1; then
			d=$RUNS/$run-ramdump
			log "$(stamp) 900e: dump into $d (resets the phone)"
			mkdir -p "$d"
			sudo -n qdl ramdump -o "$d" "$QDL_FILTER" > "$d/qdl.log" 2>&1 ||
			    die "qdl ramdump failed; the phone is still in 900e"
			log "$(stamp) waiting for the saved kernel"
			bash "$PHONE_SH" wait 10 > /dev/null || die "no system after the dump"
			bash "$(dirname "$0")/collect.sh" "$run" $P/log
			exit 2
		fi
		bash "$PHONE_SH" usbnet > /dev/null 2>&1 || true
		if r=$(CONNECT_TIMEOUT=5 ph uname -r 2> /dev/null); then
			if [ "$r" = "$k" ]; then
				[ "$seen" = 1 ] || log "$(stamp) $k up"
				seen=1
				up_since=${up_since:-$i}
				if [ $(( (i - up_since) * 10 )) -ge $(( min * 60 )) ]; then
					log "$(stamp) $k up for $min minutes: collecting, then the saved kernel"
					bash "$(dirname "$0")/collect.sh" "$run" $P/log
					ph "sync; reboot" || true
					exit 0
				fi
			else
				log "$(stamp) $r answers ($k seen: $seen): collecting"
				bash "$(dirname "$0")/collect.sh" "$run" $P/log
				exit 3
			fi
		fi
		sleep 10
	done
	die "no answer for 20 minutes"
	;;
*)
	sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
	exit 1
	;;
esac
