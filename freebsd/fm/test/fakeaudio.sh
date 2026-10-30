#!/bin/sh
# A stand-in for pro1x-fmaudio in run.sh: "start" answers as it does
# ("fmaudio: ready ..." or "fmaudio: error ..."), logs a line, and holds
# the path until SIGTERM, then says so and ends. "fail" first: the error.
mode=ok
[ "${1:-}" = fail ] && { mode=fail; shift; }
[ "${1:-}" = start ] || { echo "usage: fakeaudio.sh [fail] start" >&2; exit 2; }
echo "    0.001 slim: (fake) WCN3950 generic device at LA 0xc2"
if [ $mode = fail ]; then
	echo "fmaudio: error slimbus"
	exit 1
fi
trap 'echo "    0.002 slim: (fake) the way back"; echo "    0.003 fmaudio: start done"; exit 0' TERM INT
echo "fmaudio: ready (fake)"
while :; do sleep 0.1; done
