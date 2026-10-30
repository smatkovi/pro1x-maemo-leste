# Power management device tests in the RAM root (freebsd/pm/README.md),
# sourced by /etc/rc; each reports on the console (the RAM console keeps
# it for Linux) and reboots into the boot menu:
#
#   pro1x.test=smp      the CPUs (kern.smp.disabled=0 on the command line):
#                       what PSCI and the kernel found, one busy loop per
#                       CPU and each CPU's share of it, the frequency
#                       domains, and the same work check as idle
#   pro1x.test=idle     the PSCI idle states (hw.psci_idle.enable=1): the
#                       states, a work check (8 workers: xz decompression
#                       piped into md5, against the reference) while the
#                       CPUs keep entering and leaving them, 60 s idle,
#                       then pro1x.power_min (10) minutes of the standby
#                       power with the panel off, in pro1x.test=power's
#                       columns, the idle statistics every minute
#
# Only /rescue: no grep, no cpuset, no vmstat.
PATH=/rescue; export PATH

pm_log() { echo "pm: $(date +%T) $*"; }

pm_cpus() {
	sysctl hw.ncpu kern.smp.cpus kern.smp.maxid kern.smp.disabled 2>&1
	sysctl hw.psci_idle.mode hw.psci_idle.enable hw.psci_idle.depth 2>&1
	dmesg | sed -n -E '/^CPU +[0-9]+:|^Release APs|Failed to start|^psci_idle|^arm_tmr_mem|^gic[0-9]|^qcom_cpufreq/p'
	for c in 0 4; do
		sysctl dev.cpu.$c.freq dev.cpu.$c.freq_levels 2>/dev/null
	done
}

# A CPU's busy and all ticks in kern.cp_times (user nice sys intr idle
# per CPU): pm_busy CPU VALUES...
pm_busy() {
	shift $(($1 * 5 + 1))
	echo "$(($1 + $2 + $3 + $4)) $(($1 + $2 + $3 + $4 + $5))"
}

# One busy loop per CPU (sh arithmetic, no forks), then each CPU's busy
# share of the time
pm_load() {
	n=$(sysctl -n hw.ncpu)
	before=$(sysctl -n kern.cp_times)
	t0=$(date +%s)
	i=0
	while [ $i -lt "$n" ]; do
		(j=0; while [ $j -lt "${1:-300000}" ]; do j=$((j + 1)); done) &
		i=$((i + 1))
	done
	wait
	after=$(sysctl -n kern.cp_times)
	pm_log "load: $n loops in $(($(date +%s) - t0)) s"
	c=0
	out=""
	while [ $c -lt "$n" ]; do
		set -- $(pm_busy $c $before) $(pm_busy $c $after)
		d=$(($4 - $2))
		[ $d -gt 0 ] || d=1
		out="$out cpu$c $((100 * ($3 - $1) / d))%"
		c=$((c + 1))
	done
	pm_log "busy:$out"
}

# The work check: 8 workers for $1 seconds, a nap of $2 between rounds
pm_work() {
	[ -d /tmp/pm ] || { mdmfs -s 64m md /tmp && mkdir -p /tmp/pm; } ||
	    { pm_log "!! no /tmp"; return; }
	dd if=/dev/random of=/tmp/pm/a bs=64k count=32 2>/dev/null
	ref=$(md5 -q /tmp/pm/a)
	xz -c -1 /tmp/pm/a > /tmp/pm/a.xz
	pm_log "work: 8 workers, $1 s, nap $2 s, reference $ref"
	for w in 1 2 3 4 5 6 7 8; do
		(r=0; bad=0; end=$(($(date +%s) + $1))
		 while [ "$(date +%s)" -lt $end ]; do
			got=$(xz -dc /tmp/pm/a.xz | md5 -q)
			[ "$got" = "$ref" ] || { bad=$((bad + 1)); pm_log "!! worker $w: $got"; }
			r=$((r + 1))
			sleep $2
		 done
		 echo "worker $w: $r rounds, $bad wrong" > /tmp/pm/w$w) &
	done
	wait
	cat /tmp/pm/w*
}

pm_stats() {
	sysctl -n hw.psci_idle.stats 2>&1 | sed -e 's/^/pm:   /'
	sysctl dev.arm_tmr_mem.0.state 2>/dev/null
}

# The standby power, pro1x.test=power's columns ("power: s mA mV %
# status"), the idle statistics every minute
pm_power() {
	min=$(kenv -q pro1x.power_min)
	min=${min:-10}
	echo "=== power: $min min, the panel off, on the battery"
	sysctl dev.qcom_dpu.0.display=0
	for c in 0 4; do
		l=$(sysctl -n dev.cpu.$c.freq_levels 2>/dev/null) || continue
		l=${l##* }
		sysctl dev.cpu.$c.freq=${l%%/*}
	done
	sysctl dev.pro1x_battery.0.input_suspend=1
	echo "power: s mA mV % status"
	t=0
	while [ $t -lt $((min * 60)) ]; do
		sleep 30
		t=$((t + 30))
		i=$(sysctl -n dev.pro1x_battery.0.current)
		v=$(sysctl -n dev.pro1x_battery.0.voltage)
		echo "power: $t $((-i / 1000)) $((v / 1000))" \
		    "$(sysctl -n dev.pro1x_battery.0.capacity)" \
		    "$(sysctl -n dev.pro1x_battery.0.status)"
		[ $((t % 60)) -ne 0 ] || pm_stats
	done
	sysctl dev.pro1x_battery.0.input_suspend=0
	sysctl dev.qcom_dpu.0.display=1
	echo "=== power: done"
}

pm_done() {
	pm_log "=== test $1 done, rebooting"
	sleep 2
	reboot
}

case "$(kenv -q pro1x.test)" in
smp)
	echo "=== pm: SMP test"
	pm_cpus
	pm_load 300000
	pm_work 30 0.05
	pm_done smp
	;;
idle)
	echo "=== pm: PSCI idle test"
	pm_cpus
	pm_stats
	pm_work 60 0.02
	pm_log "after the work"
	pm_stats
	sleep 60
	pm_log "after 60 s idle"
	pm_stats
	pm_power
	pm_stats
	pm_done idle
	;;
esac
