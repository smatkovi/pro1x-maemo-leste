#!/bin/bash
# The logs of one device-day run, from the phone into
# ~/ps/pro1x-runs/2026-10-31/<run-name>/ with a summary
# (docs/device-day-2026-10-31.md).
#
#   bash tools/devday/collect.sh RUN-NAME [REMOTE-FILE...]
#       SYS=leste (default), meego, kali, arch, luneos: the running Linux
#       system; SYS=freebsd: FreeBSD over its USB network (pm runs 10/11);
#       SYS=n950: the N950's Leste.
#   DRY=1 ...    the commands only
#
# From a Linux system on the Pro1X: uname, /proc/cmdline, dmesg, the
# journal of this boot and the one before (systemd systems), syslog /
# messages, the kernel's pstore (the console of the boot before: what a
# hang or a reset left), the boot menu's state (pro1x-boot list, last-boot,
# the trial, the slots), the RAM console FreeBSD or KolibriOS left at
# 0x5ca00000 (tools/devday/ramconsole.py, copied over when missing: read
# only; CLEAR=1 clears it after it was saved here), a screenshot of the X
# display (xwd, converted to PNG here when ImageMagick is there), and every
# REMOTE-FILE given (e.g. /host/audio-trace.txt; directories as tar).
# summary.txt: what ran, the trial and the slots, the RAM console's marker
# and end, and the error lines of the kernel log.
#
# A RUN-NAME that exists already gets -2, -3, ... appended.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
name=${1:?usage: collect.sh RUN-NAME [REMOTE-FILE...]}
shift
case $name in */*|.*) die "RUN-NAME: no slashes, no leading dot" ;; esac
[ "$DRY" = 1 ] && RUNS=/tmp/pro1x-devday-dry/runs
D=$RUNS/$name
i=2
while [ -e "$D" ]; do D=$RUNS/$name-$i; i=$((i + 1)); done
need_space "$RUNS" 200
mkdir -p "$D"
log "collecting into $D (SYS=$SYS)"
# fetch FILE CMD: CMD's output on the phone into FILE here (errors there too)
fetch() { local f=$1; shift; ph "$@" > "$D/$f" 2>&1 || echo "(exit $?)" >> "$D/$f"; }

date -Iseconds > "$D/collected-at.txt"
case $SYS in
freebsd)
	fetch meta.txt 'uname -a; sysctl -n kern.boottime; kenv 2>/dev/null | grep -E "^(pro1x|hint)\."; uptime'
	fetch dmesg.txt 'dmesg -a'
	fetch messages.txt 'cat $R/var/log/messages'
	fetch logs.tar.txt 'ls -la $R/var/log $R/tmp'
	;;
*)
	fetch meta.txt 'uname -a; echo; cat $R/proc/cmdline; echo; uptime; cat $R/host/pro1x-boot/current 2>/dev/null; df -h / $R/host 2>/dev/null; true'
	fetch dmesg.txt 'dmesg'
	if [ "$SYS" != n950 ]; then
		fetch boot-menu.txt 'p=/usr/local/sbin/pro1x-boot; [ -x $p ] || p=pro1x-boot; $p list; echo "== last-boot"; cat $R/host/pro1x-boot/last-boot; for f in trial trial-ok trial-wait default next timeout; do [ -e $R/host/pro1x-boot/$f ] && echo "== $f: $(cat $R/host/pro1x-boot/$f)"; done; true'
		fetch slots.txt 'for p in boot_a boot_b; do d=$(readlink -f $R/dev/disk/by-partlabel/$p) || continue; n=${d##*/}; disk=/dev/$(basename "$(readlink -f $R/sys/class/block/$n/..)"); echo "== $p"; sgdisk -i "$(cat $R/sys/class/block/$n/partition)" "$R$disk" | grep -E "name|Attribute"; done; true'
	fi
	fetch pstore.txt 'for f in $R/sys/fs/pstore/*; do [ -f "$f" ] && { echo "==== ${f##*/}"; cat "$f"; }; done; true'
	if ph 'command -v journalctl > /dev/null' 2> /dev/null && [ "$DRY" != 1 ]; then
		fetch journal.txt 'journalctl -b --no-pager -o short-monotonic'
		fetch journal-previous.txt 'journalctl -b -1 --no-pager -o short-monotonic'
	fi
	fetch syslog.txt 'for f in $R/var/log/syslog $R/var/log/messages; do [ -f "$f" ] && { echo "==== $f"; tail -c 4000000 "$f"; }; done; true'
	# the RAM console (FreeBSD's, KolibriOS's), read only
	if [ "$SYS" != n950 ]; then
		if [ "$DRY" != 1 ] && ! ph '[ -f $R/root/devday/ramconsole.py ]' 2> /dev/null; then
			ph 'mkdir -p $R/root/devday'
			put "$REPO/tools/devday/ramconsole.py" /root/devday/ramconsole.py
		fi
		fetch ramconsole.txt 'cd $R/root/devday && python3 ramconsole.py --shot $R/root/devday/shot.bin'
		if [ "$DRY" != 1 ] && ph '[ -s $R/root/devday/shot.bin ]' 2> /dev/null; then
			ph 'cat $R/root/devday/shot.bin; rm -f $R/root/devday/shot.bin' > "$D/ramconsole-shot.bin"
			# a gzip'd xwd (FreeBSD), a tar (FreeBSD's camera), a PPM (KolibriOS)
			t=$(file -b "$D/ramconsole-shot.bin" 2> /dev/null || echo data)
			echo "ramconsole-shot.bin: $t" >> "$D/files-note.txt"
		fi
		if [ "${CLEAR:-0}" = 1 ]; then
			ph 'cd $R/root/devday && python3 ramconsole.py --clear > /dev/null'
			log "RAM console cleared on the phone"
		fi
	fi
	# a screenshot of the X display (Leste, the MeeGo copy, Kali's X)
	if [ "$DRY" = 1 ]; then
		ph 'DISPLAY=:0 xwd -root -silent | gzip'
	elif ph 'command -v xwd > /dev/null' 2> /dev/null; then
		ph 'DISPLAY=:0 XAUTHORITY=$(ls /home/*/.Xauthority 2>/dev/null | head -1) xwd -root -silent | gzip' > "$D/screen.xwd.gz" 2> /dev/null || rm -f "$D/screen.xwd.gz"
		if [ -s "$D/screen.xwd.gz" ] && command -v magick > /dev/null; then
			gzip -dc "$D/screen.xwd.gz" | magick xwd:- "$D/screen.png" 2> /dev/null || true
		fi
		[ -s "$D/screen.xwd.gz" ] || { rm -f "$D/screen.xwd.gz"; echo "no screenshot (xwd failed: no X on :0?)" >> "$D/files-note.txt"; }
	else
		echo "no screenshot: no xwd in $SYS (apt install x11-apps)" >> "$D/files-note.txt"
	fi
	;;
esac

# the files asked for
for f in "$@"; do
	b=$(echo "${f#/}" | tr '/' '_')
	if [ "$DRY" = 1 ]; then ph "cat $f"; continue; fi
	if ph "[ -d \$R$f ]" 2> /dev/null; then
		ph "tar -C \$R$f -cf - ." > "$D/$b.tar" || echo "$f: tar failed" >> "$D/files-note.txt"
	else
		ph "cat \$R$f" > "$D/$b" 2>> "$D/files-note.txt" || echo "$f: not there" >> "$D/files-note.txt"
	fi
done

[ "$DRY" = 1 ] && { log "dry run: $D has empty files"; exit 0; }

# the summary
{
	echo "run: $name    system: $SYS    collected: $(cat "$D/collected-at.txt")"
	echo
	sed -n '1,3p' "$D/meta.txt"
	if [ -f "$D/boot-menu.txt" ]; then
		echo
		echo "boot menu:"
		grep -E '^(running|default|next|active slot|trial):' "$D/boot-menu.txt" | sed 's/^/  /'
		grep -E 'starting|choice=' "$D/boot-menu.txt" | tail -2 | sed 's/^/  last-boot: /'
	fi
	if [ -f "$D/slots.txt" ]; then
		python3 -I -c '
import re, sys
name = None
for line in open(sys.argv[1]):
    m = re.match(r"== (\S+)", line)
    if m: name = m.group(1)
    m = re.search(r"Attribute flags: ([0-9a-fA-F]+)", line)
    if m and name:
        a = int(m.group(1), 16)
        print(f"  {name}: active {(a >> 50) & 1}, tries {(a >> 51) & 7}, successful {(a >> 54) & 1}, unbootable {(a >> 55) & 1}")
' "$D/slots.txt" || true
	fi
	if [ -f "$D/ramconsole.txt" ]; then
		echo
		echo "RAM console: $(head -1 "$D/ramconsole.txt") ($(($(wc -l < "$D/ramconsole.txt") - 1)) lines)"
		if grep -q '^PRO1X-' "$D/ramconsole.txt"; then
			grep -E 'PASS|FAIL|=== ' "$D/ramconsole.txt" | tail -15 | sed 's/^/  /'
			echo "  -- its last lines:"
			tail -8 "$D/ramconsole.txt" | sed 's/^/  /'
		fi
	fi
	if [ -s "$D/pstore.txt" ]; then
		echo
		echo "pstore (the boot before): $(grep -c . "$D/pstore.txt") lines; its end:"
		grep -v '^====' "$D/pstore.txt" | tail -5 | sed 's/^/  /'
	fi
	echo
	n=$(grep -c -i -E 'error|fail|warn|oops|panic|timed out|timeout|unable|fault' "$D/dmesg.txt" || true)
	echo "kernel log: $(wc -l < "$D/dmesg.txt") lines, $n with error/fail/warn/panic/timeout/fault; the first 25:"
	grep -i -E 'error|fail|warn|oops|panic|timed out|timeout|unable|fault' "$D/dmesg.txt" | head -25 | sed 's/^/  /' || true
	echo
	echo "files:"
	( cd "$D" && ls -la | tail -n +4 | awk '{ printf "  %10s  %s\n", $5, $9 }' )
	[ ! -f "$D/files-note.txt" ] || { echo; sed 's/^/  note: /' "$D/files-note.txt"; }
} > "$D/summary.txt"
cat "$D/summary.txt"
log "summary: $D/summary.txt"
