#!/bin/sh
# per-process context switches and per-IRQ counts over $1 seconds
T=${1:-30}
snap_ctx() { for p in /proc/[0-9]*; do
	c=$(awk '/^voluntary_ctxt_switches|^nonvoluntary_ctxt_switches/{s+=$2}END{print s}' $p/status 2>/dev/null)
	[ -n "$c" ] && echo "${p#/proc/} $c $(tr -d '\n' < $p/comm 2>/dev/null)"; done; }
snap_irq() { awk 'NR>1{s=0;for(i=2;i<=9;i++)s+=$i; $1=$1; n=$1; d=""; for(i=10;i<=NF;i++)d=d" "$i; print n, s, d}' /proc/interrupts; }
snap_ctx > /tmp/ctx1; snap_irq > /tmp/irq1
sleep $T
snap_ctx > /tmp/ctx2; snap_irq > /tmp/irq2
echo "== wakeups/s per process (top 20)"
awk -v T=$T 'NR==FNR{a[$1]=$2;next} ($1 in a){d=$2-a[$1]; if(d>0){$2=""; printf "%7.2f %s\n", d/T, $0}}' /tmp/ctx1 /tmp/ctx2 | sort -rn | head -20
echo "== interrupts/s (top 15)"
awk -v T=$T 'NR==FNR{a[$1]=$2;next} ($1 in a){d=$2-a[$1]; if(d>0){n=$1;$1="";$2=""; printf "%7.2f %s %s\n", d/T, n, $0}}' /tmp/irq1 /tmp/irq2 | sort -rn | head -15
