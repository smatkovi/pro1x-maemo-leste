#!/bin/sh
# pro1x-fmd against a simulated WCN3950 (fakechip.c) on a pty, through its
# socket with pro1x-fmctl: the chip's bring-up (pulses, version, baud
# rate, rampatch and NVM download, reset), in-band sleep both ways, the
# receiver on, tune, seek (with wrap, and without any station), RDS, mute,
# mono, the events, a second power cycle. The fake chip checks the host's
# side and fails the run on anything wrong. Run by the Makefile.
set -u
OUT=${OUT:-/tmp/pro1x-fm-test}
S=$OUT/fm.sock
fail=0

ctl() { "$OUT/pro1x-fmctl" -S "$S" "$@"; }

# expect PATTERN REQUEST...: the answer must match the shell pattern
expect() {
	pat=$1
	shift
	r=$(ctl "$@")
	echo "  $* -> $r"
	case "$r" in
	$pat) ;;
	*) echo "FAIL: '$*' answered '$r', wanted '$pat'"; fail=1 ;;
	esac
}

start() {
	rm -f "$OUT/pty" "$S"
	"$OUT/fakechip" "$OUT/pty" "$OUT/fw/qca/cmbtfw12.tlv" \
	    "$OUT/fw/qca/cmnv12.bin" "$1" 2> "$OUT/fakechip-$1.log" &
	chip=$!
	i=0
	while [ ! -s "$OUT/pty" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
	"$OUT/pro1x-fmd" -F -d "$(cat "$OUT/pty")" -r none -S "$S" \
	    -f "$OUT/fw" ${2:-} 2> "$OUT/fmd-$1.log" &
	fmd=$!
	i=0
	while [ ! -S "$S" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
}

stop() {
	kill -TERM "$fmd" 2> /dev/null
	wait "$fmd"
	kill -TERM "$chip" 2> /dev/null
	if ! wait "$chip"; then
		echo "FAIL: the fake chip's report:"
		fail=1
	fi
	grep "report" "$OUT/fakechip-$1.log"
}

echo "--- with stations"
start stations
expect "ok" on
grep -E "QCA|firmware|UART at|setup" "$OUT/fmd-stations.log" | sed 's/^/  fmd: /'
expect "ok on=1 freq=87500 *" status
expect "ok 99900" tune 99.9
sleep 1
expect "ok TESTFM" ps
expect "ok Hello from the fake WCN3950" rt
expect "ok on=1 freq=99900 rssi=-70 signal=100 stereo=1 rds=1 *" station
expect "ok 103800" seek up
expect "ok 99900" seek up
expect "ok 103800" seek down
expect "ok" mute 1
expect "ok" mono 1
expect "ok on=1 freq=103800 * mute=1 mono=1 *" status
ctl -t 2 watch > "$OUT/watch.log" &
w=$!
sleep 0.3
expect "ok 99900" tune 99900
wait $w
sed 's/^/  watch: /' "$OUT/watch.log"
grep -q "^event tune 99900" "$OUT/watch.log" || { echo "FAIL: no tune event"; fail=1; }
grep -q "^event ps TESTFM" "$OUT/watch.log" || { echo "FAIL: no ps event"; fail=1; }
# idle for 3 s: both sides go to sleep; then woken again
sleep 3
expect "ok on=1 freq=99900 rssi=-70 *" station
expect "ok up=1 soc=0x40070120 rom=0x0102 * files=qca/cmbtfw12.tlv,qca/cmnv12.bin *" info
info=$(ctl info)
sl=$(echo "$info" | sed -n 's/.*ibs_sleeps=\([0-9]*\)\/\([0-9]*\).*/\1 \2/p')
set -- $sl
[ "${1:-0}" -ge 1 ] && [ "${2:-0}" -ge 1 ] ||
    { echo "FAIL: no in-band sleep both ways ($sl)"; fail=1; }
expect "ok" off
expect "ok on=0 *" status
expect "error ENETDOWN *" tune 99.9
expect "ok" on
expect "ok 99900" tune 99.9
expect "ok" off
stop stations

echo "--- without stations"
start nostations
expect "ok" on
expect "error ENODATA *" seek up
expect "ok on=1 freq=87500 *" status
expect "ok" off
stop nostations

echo "--- at 115200 baud throughout (no baud rate change; the NVM's rate 0)"
start stations "-s 115200"
expect "ok" on
grep -q "UART at" "$OUT/fmd-stations.log" && { echo "FAIL: the baud rate changed"; fail=1; }
expect "ok 103800" tune 103.8
expect "ok" off
stop stations

if [ $fail -eq 0 ]; then echo "run.sh: ok"; else echo "run.sh: FAILED (logs in $OUT)"; fi
exit $fail
