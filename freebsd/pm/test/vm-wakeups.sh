#!/bin/sh
# vm-wakeups.sh [SECONDS]: the idle wake-ups of a Hildon session in the
# FreeBSD test VM (freebsd/vm, as root, with the packages installed), as
# powertop counts them on Linux: Hildon at 2160x1080 on a virtual screen
# (Xvfb :1, as vm/try-hildon.sh), left alone for $WAIT seconds (60) to
# settle, then for SECONDS (60) with DTrace:
#
#   wake-ups  sched:::wakeup per process (a thread made runnable: a timer
#             that fired, a message that came)
#   timers    the kernel's callouts by function (callout_execute)
#   on-CPU    sched:::on-cpu per process (each time it got a CPU)
#
# and the interrupts (vmstat -i) and CPU time (kern.cp_time) over the same
# time. EXTRA="cmd ..." runs more programs in the session first (each
# with PRO1X_TRY=1 in its environment, ended with it). Everything the
# session started ends afterwards. Output on stdout; nothing is written
# except below /tmp.
set -u
SECS=${1:-60}
T=/tmp/vm-wakeups
mkdir -p "$T"
kldstat -q -m dtrace || kldload dtraceall
service dbus onestatus > /dev/null 2>&1 || service dbus onestart
export DISPLAY=:1 PRO1X_TRY=1
Xvfb :1 -screen 0 2160x1080x24 -noreset -nolisten tcp > "$T/xvfb.log" 2>&1 &
xvfb=$!
sleep 3
/usr/local/bin/pro1x-hidpi theme > /dev/null 2>&1
daemon -f -o "$T/session.log" /usr/local/bin/hildon-session
for c in ${EXTRA:-}; do
	daemon -f -o "$T/extra.log" sh -c "$c"
done
sleep "${WAIT:-60}"
vmstat -i > "$T/intr.0"
sysctl -n kern.cp_time > "$T/cp.0"
dtrace -q -n '
sched:::wakeup { @w[stringof(args[1]->p_comm)] = count(); }
sched:::on-cpu /pid != 0/ { @o[execname] = count(); }
callout_execute:::callout-start { @c[args[0]->c_func] = count(); }
tick-'"$SECS"'s {
	printf("== wake-ups in %d s (per process)\n", '"$SECS"');
	printa("%-24s %@8d\n", @w);
	printf("== on-CPU in %d s (per process)\n", '"$SECS"');
	printa("%-24s %@8d\n", @o);
	printf("== kernel timers (callouts) in %d s\n", '"$SECS"');
	printa("%-48a %@8d\n", @c);
	exit(0);
}' > "$T/dtrace.out" 2>&1
vmstat -i > "$T/intr.1"
sysctl -n kern.cp_time > "$T/cp.1"
cat "$T/dtrace.out"
echo "== interrupts in $SECS s"
awk 'NR == FNR { if ($1 != "interrupt" && $1 != "Total") a[$1] = $(NF - 1); next }
     $1 != "interrupt" && $1 != "Total" { printf "%-24s %8d\n", $1, $(NF - 1) - a[$1] }' \
    "$T/intr.0" "$T/intr.1"
echo "== CPU time (user nice system interrupt idle, ticks)"
paste "$T/cp.0" "$T/cp.1" | awk '{ n = NF / 2; for (i = 1; i <= n; i++) printf "%d ", $(i + n) - $i; print "" }'
# the session and everything it started (by the marker in their environment)
for p in $(ps -axo pid=); do
	if procstat -e "$p" 2> /dev/null | grep -q PRO1X_TRY=1; then
		kill "$p" 2> /dev/null
	fi
done
kill $xvfb 2> /dev/null
