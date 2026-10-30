# Power management device tests in the RAM root (freebsd/pm/README.md),
# sourced by /etc/rc; each reports on the console (the RAM console keeps
# it for Linux) and reboots into the boot menu:
#
#   pro1x.test=smp      the CPUs (kern.smp.disabled=0 on the command line):
#                       what PSCI and the kernel found, one busy loop per
#                       CPU and each CPU's share of it, the frequency
#                       domains, and the same work check as idle
#   pro1x.test=suspend  the SoC's sleep (the RPM sleep set, the MPM hand-over)
#                       and suspend-to-idle (pro1x_pm): A 60 s idle, B a
#                       60 s suspend, C until the power key
#                       (pro1x.suspend_key=1), D the battery over
#                       pro1x.power_min minutes of suspend
#   pro1x.test=usb      the USB controller as Linux left it (read only)
#   pro1x.test=usbnet   the USB gadget: dwc3dci, CDC Ethernet, ue0 as
#                       172.16.42.1 for pro1x.usb_min (5) minutes
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

pm_bat() {
	i=$(sysctl -n dev.pro1x_battery.0.current 2>/dev/null)
	v=$(sysctl -n dev.pro1x_battery.0.voltage 2>/dev/null)
	i=${i:-0}
	v=${v:-0}
	pm_log "battery $1: $((v / 1000)) mV, $((-i / 1000)) mA," \
	    "$(sysctl -n dev.pro1x_battery.0.capacity 2>/dev/null) %"
}

# The RPM sleep set needs the RPM channel: qcom_dsp.ko and
# pro1x_rpmsleep.ko from userdata's freebsd/pm/ (freebsd/pm/build.sh's
# pm-out/modules), read only
pm_kld() {
	mount -t ext2fs -o ro /dev/gpt/userdata /dev/pro1x-host ||
	    { pm_log "!! userdata not mounted"; return 1; }
	for m in qcom_dsp pro1x_rpmsleep; do
		kldload /dev/pro1x-host/freebsd/pm/$m.ko ||
		    pm_log "!! $m.ko not loaded"
	done
	umount /dev/pro1x-host
	kldstat
}

# Quiet: the panel off (DCS), the touchscreen asleep, the keyboard light
# off, the charger's input suspended (on the battery with the cable in)
pm_quiet() {
	sysctl dev.qcom_dpu.0.display=$((1 - $1)) dev.goodix.0.sleep=$1 \
	    dev.pro1x_battery.0.input_suspend=$1 2>&1
	for l in /dev/led/*kbd* /dev/led/*keyboard*; do
		[ -c "$l" ] && echo 0 > "$l"
	done
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
usbnet)
	# stage 4: the USB gadget, CDC Ethernet (usb_template 1) on dwc3dci;
	# the modules from userdata's freebsd/pm/ (pm-out/modules)
	echo "=== pm: USB gadget, CDC Ethernet"
	sysctl hw.pro1x_pm.usb_probe
	if mount -t ext2fs -o ro /dev/gpt/userdata /dev/pro1x-host; then
		M=/dev/pro1x-host/freebsd/pm
		for m in qcom_dsp usb usb_template uether if_cdce; do
			kldload $M/$m.ko || pm_log "!! $m.ko not loaded"
		done
		sysctl hw.usb.template=1
		kenv hw.dwc3dci.enable=1
		kldload $M/pro1x_usb.ko || pm_log "!! pro1x_usb.ko not loaded"
		umount /dev/pro1x-host
	else
		pm_log "!! userdata not mounted"
	fi
	kldstat
	dmesg | sed -n -E '/dwc3dci|usbus|ugen|cdce|ue[0-9]/p'
	sysctl dev.dwc3dci.0.bringup dev.dwc3dci.0.stats
	min=$(kenv -q pro1x.usb_min)
	min=${min:-5}
	t=0
	up=
	while [ $t -lt $((min * 60)) ]; do
		if [ -z "$up" ] && ifconfig ue0 > /dev/null 2>&1; then
			ifconfig ue0 inet 172.16.42.1/24 up && up=1
			pm_log "ue0 up as 172.16.42.1"
		fi
		pm_log "$(sysctl -n dev.dwc3dci.0.stats 2>&1)"
		if [ -n "$up" ]; then
			ifconfig ue0 | sed -n -E '/status|ether|inet /p'
			ping -c 1 -t 2 172.16.42.2 > /dev/null 2>&1 &&
			    pm_log "172.16.42.2 answers" || pm_log "172.16.42.2: no answer"
		fi
		sleep 10
		t=$((t + 10))
	done
	dmesg | sed -n -E '/dwc3dci|usbus|ugen|cdce|ue[0-9]/p' | tail -20
	pm_done usbnet
	;;
usb)
	# stage 4: what Linux leaves of the USB controller (read only)
	echo "=== pm: USB probe"
	sysctl hw.pro1x_pm.usb_probe
	dmesg | sed -n -E '/usb|dwc|xhci/p' | head -20
	pm_done usb
	;;
suspend)
	# needs hw.psci_idle.enable=1 for A; B and C freeze the CPUs anyway
	echo "=== pm: SoC sleep and suspend test"
	pm_cpus
	sysctl hw.pro1x_pm.rpm_stats
	pm_kld && sysctl hw.pro1x_rpmsleep.set=1 hw.pro1x_rpmsleep.result
	pm_quiet 1
	pm_bat "before"
	# A: the SoC's sleep from idle, the MPM handed over to the RPM
	sysctl hw.pro1x_pm.mpm=1
	pm_log "A: 60 s idle with the hand-over"
	sleep 60
	sysctl hw.pro1x_pm.rpm_stats
	pm_stats
	pm_bat "after A"
	# B: suspend-to-idle for 60 s, woken by the deadline (epochs of 5 s)
	pm_log "B: suspend 60 s"
	sysctl hw.pro1x_pm.suspend=60 > /dev/null
	sysctl hw.pro1x_pm.last hw.pro1x_pm.rpm_stats
	pm_bat "after B"
	# C: until the power key (pro1x.suspend_key=1: someone at the phone
	# presses it within 120 s)
	if [ "$(kenv -q pro1x.suspend_key)" = 1 ]; then
		pm_log "C: suspend until the power key (120 s at most): press it"
		sysctl hw.pro1x_pm.suspend=120 > /dev/null
		sysctl hw.pro1x_pm.last
	fi
	# D: the battery over pro1x.power_min (10) minutes of suspend, a
	# minute each, read right after each wake-up
	min=$(kenv -q pro1x.power_min)
	min=${min:-10}
	echo "=== power: $min min suspended, the panel off, on the battery"
	echo "power: s mA mV % status"
	t=0
	while [ $t -lt $((min * 60)) ]; do
		sysctl hw.pro1x_pm.suspend=60 > /dev/null
		t=$((t + 60))
		i=$(sysctl -n dev.pro1x_battery.0.current)
		v=$(sysctl -n dev.pro1x_battery.0.voltage)
		echo "power: $t $((-i / 1000)) $((v / 1000))" \
		    "$(sysctl -n dev.pro1x_battery.0.capacity)" \
		    "$(sysctl -n dev.pro1x_battery.0.status)"
		sysctl -n hw.pro1x_pm.last
	done
	echo "=== power: done"
	sysctl hw.pro1x_pm.rpm_stats
	pm_stats
	sysctl hw.pro1x_pm.mpm=0
	pm_quiet 0
	pm_bat "at the end"
	pm_done suspend
	;;
esac
