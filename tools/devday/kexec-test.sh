#!/bin/bash
# A candidate kernel started once from the running Leste with kexec, from
# the PC: no flash, no trial, boot_b and the slots stay as they are
# (docs/crash-dumps.md, "Testing a kernel without flashing it";
# docs/device-day-2026-10-31.md, block 18).
#
#   kexec-test.sh check K       read only: what the test needs, here and on
#                               the phone, and the command line it would use
#   kexec-test.sh load K        kernel, initramfs and device tree into
#                               /host/kernel-update, then kexec -l (no reboot;
#                               kexec-test.sh unload takes it back)
#   OK=yes kexec-test.sh go K   [OK] the jump: a one-shot "leste" for the
#                               candidate's boot menu, the APSS watchdog armed
#                               (32 s), sync, file systems read-only, kexec -e
#   kexec-test.sh after K       waits for the phone and says what came back;
#                               OK=yes: in the crash-dump mode it takes the
#                               dump, which resets the phone (ways back 4)
#   kexec-test.sh unload        kexec -u
#
# K: the kernel's output directory, $WORK/out-K (pro1x.12, pro1x.12nobt,
# pro1x.12no29, pro1x.12nobtslim; pro1x.5 for the calibration run, the
# running kernel into itself). Before load, 65-kernel-update.sh prepare
# (OUT=$WORK/out-K) has put its modules into /host/pro1x-modules/<version>
# and made its initramfs in Leste; it writes no partition.
#
# What happens: the candidate boots with its own device tree (the one its
# boot image would carry, with the memory banks the bootloader filled in for
# the running kernel, taken from /sys/firmware/fdt), its initramfs and the
# running command line plus TEST_ARGS and EXTRA. Its boot menu takes the
# one-shot "leste", arms the watchdog and loads qcom_wdt, as a trial start
# does; dsme takes the watchdog over in Leste. A candidate that hangs is
# reset by the watchdog: either straight into boot_b, whose 7.1.0-pro1x.5
# starts Leste again, or through the crash-dump mode (05c6:900e), which
# `after` (OK=yes) leaves with qdl ramdump. Both keep the RAM, so the pstore
# of the next Leste start holds the candidate's console (collect.sh).
#
# Known limits:
# - On 2026-10-28 every kernel started this way stopped about 0.23 s into its
#   boot (docs/kernel-update.md), the running kernel started into itself
#   too. Run 0 (K=pro1x.5, EXTRA="ignore_loglevel initcall_debug") shows
#   whether that still happens and where. Until a run gets past it, a
#   candidate's result says nothing about the candidate.
# - The first kernel's state carries over: the remote processors keep
#   running, the display stays on (patch 0004/0005 keep UFS and the display
#   powered). A candidate that crashes under kexec may crash for that
#   reason; one that runs is good, not perfect, evidence.
#
# EXTRA: more for the command line (default ignore_loglevel: the whole
# kernel log in the RAM console; it slows the serial console's boot).
# NOSERIAL=1: the running command line without console=ttyMSM0 and earlycon
# (run 0 only: initcall_debug's lines at 115200 baud could outlast the 32 s
# watchdog; runs 1-4 keep the console as the crashed starts had it).
# BASE: the kernel that must run and that boot_b must hold (7.1.0-pro1x.5).
set -euo pipefail
. "$(dirname "$0")/lib.sh"
SYS=leste

usage() { sed -n '2,/^set -euo/p' "$0" | sed '$d'; exit 1; }
what=${1:-}
K=${2:-}
case $what in
unload) ph 'kexec -u; echo "kexec_loaded: $(cat /sys/kernel/kexec_loaded)"'; exit 0 ;;
check|load|go|after) [ -n "$K" ] || usage ;;
*) usage ;;
esac

OUT=$WORK/out-$K
KVER=$(cat "$OUT/kernelrelease" 2>/dev/null) || die "no $OUT/kernelrelease"
BASE=${BASE:-7.1.0-pro1x.5}
# boot_b as restored on 2026-10-31 23:04 (= pro1x.5's image of 10-29)
BOOTB_IMG=${BOOTB_IMG:-$WORK/out-pro1x.5/boot_b-7.1.0-pro1x.5.img}
UP=/host/kernel-update
DTB_SRC=$OUT/sm6115-fxtec-pro1x-abl.dtb
[ -f "$DTB_SRC" ] || DTB_SRC=$OUT/sm6115-fxtec-pro1x.dtb
# for the test: RCU stalls reported after 5 s instead of 21 (the watchdog
# bites 30 s after the last feed), and a marker
TEST_ARGS="pro1x.kexectest=$KVER rcupdate.rcu_cpu_stall_timeout=5"
EXTRA=${EXTRA-ignore_loglevel}
# the crash-dump mode's regions worth having (small ones; DATARAM holds the
# RPM's sleep statistics, PMON_HIS the PMIC's power-on history)
QDL_FILTER='OCIMEM*,CODERAM*,DATARAM*,MSGRAM*,PMIC*,PMON*,RST*,FSM*,*.cmm'
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

fails=0
ok() { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fails=$((fails + 1)); }
note() { echo "  note  $*"; }

# the device tree for the jump: the candidate's, with the live memory node;
# and the ramoops node, which must be the live one (the pstore survives the
# reset only where the next kernel, boot_b's, looks for it)
make_dtb() {
	ph 'cat /sys/firmware/fdt' > "$T/live.dtb"
	dtc -q -I dtb -O dts "$T/live.dtb" -o "$T/live.dts"
	cp "$DTB_SRC" "$T/cand.dtb"
	fdtget "$T/cand.dtb" / qcom,board-id > /dev/null 2>&1 ||
	    fdtput -t u "$T/cand.dtb" / qcom,board-id 0 0
	dtc -q -I dtb -O dts "$T/cand.dtb" -o "$T/cand.dts"
	python3 -I - "$T/live.dts" "$T/cand.dts" "$T/kexec.dts" <<'EOF'
import re, sys
live, cand, out = open(sys.argv[1]).read(), open(sys.argv[2]).read(), sys.argv[3]
mem = lambda s: re.search(r'\n\tmemory@[0-9a-f]+ \{\n(.*?)\n\t\};', s, re.S)
ramoops = lambda s: re.search(r'ramoops@[0-9a-f]+ \{(.*?)\};', s, re.S)
l, c = mem(live), mem(cand)
if not l or not c:
    sys.exit('no /memory node')
lr, cr = ramoops(live), ramoops(cand)
if not lr or not cr:
    sys.exit('no ramoops node')
strip = lambda s: re.sub(r'\s*phandle = <[^>]*>;', '', s).split()
if strip(lr.group(0)) != strip(cr.group(0)):
    sys.exit('the ramoops nodes differ:\n' + lr.group(0) + '\n' + cr.group(0))
open(out, 'w').write(cand[:c.start(1)] + l.group(1) + cand[c.end(1):])
EOF
	dtc -q -I dts -O dtb "$T/kexec.dts" -o "$T/kexec.dtb"
}

# the command line: the running one, what the candidate's boot image adds
# (when prepare made one), the test's arguments
make_cmdline() {
	local live add=
	live=$(ph 'cat /proc/cmdline')
	if [ -f "$OUT/boot_b-$KVER.img" ]; then
		args() { python3 -I -c "
import sys; d = open(sys.argv[1], 'rb').read(4096)
print((d[64:576].split(b'\0')[0] + d[608:1632].split(b'\0')[0]).decode())" "$1"; }
		ph 'dd if=/dev/disk/by-partlabel/boot_b bs=4096 count=1 status=none' > "$T/flashed.hdr"
		add=$(comm -13 <(args "$T/flashed.hdr" | tr ' ' '\n' | sort -u) \
		    <(args "$OUT/boot_b-$KVER.img" | tr ' ' '\n' | sort -u) | paste -sd' ' -)
	fi
	# NOSERIAL=1: without the serial console and earlycon (115200 baud,
	# nothing attached), for run 0's initcall_debug, whose thousands of
	# lines there could outlast the 32 s watchdog before the menu re-arms it
	echo "$live${add:+ $add} $TEST_ARGS${EXTRA:+ $EXTRA}" | tr ' ' '\n' | grep . |
	    if [ "${NOSERIAL:-0}" = 1 ]; then grep -v -e '^console=ttyMSM' -e '^earlycon'; else cat; fi |
	    paste -sd' ' -
}

checks() {
	local r
	echo "== $K ($KVER) on the PC"
	[ -f "$OUT/Image.gz" ] && ok "$OUT/Image.gz" || bad "$OUT/Image.gz is missing"
	[ -f "$DTB_SRC" ] && ok "device tree ${DTB_SRC##*/}" || bad "no device tree in $OUT"
	echo "== the phone"
	r=$(ph 'uname -r') || die "Leste does not answer (phone.sh usbnet, phone.sh wait)"
	[ "$r" = "$BASE" ] && ok "runs $r" ||
	    bad "runs $r, not $BASE: the way back is boot_b's kernel, which must be the one known to run"
	if [ -f "$BOOTB_IMG" ]; then
		[ "$(ph 'sha256sum < /dev/disk/by-partlabel/boot_b' | cut -c1-64)" = "$(sha "$BOOTB_IMG")" ] &&
		    ok "boot_b = ${BOOTB_IMG##*/}" ||
		    bad "boot_b is not ${BOOTB_IMG##*/}: after a reset the phone would start something else"
	else
		note "no $BOOTB_IMG to compare boot_b with"
	fi
	if ph '/usr/local/sbin/pro1x-boot list' > "$T/list" 2>&1; then
		grep -q '^trial:' "$T/list" && bad "a trial is pending: $(grep '^trial:' "$T/list")" ||
		    ok "no trial pending"
		grep -q '^next: *-' "$T/list" && ok "no one-shot choice pending" ||
		    note "a one-shot choice is set: $(grep '^next:' "$T/list")"
	else
		bad "pro1x-boot list failed"
	fi
	ph 'command -v kexec > /dev/null && kexec --version' > "$T/kexec" 2>&1 &&
	    ok "$(head -1 "$T/kexec")" || bad "no kexec in Leste (kexec-tools)"
	ph 'busybox --list | grep -qx devmem' && ok "busybox devmem" ||
	    bad "no busybox devmem: the watchdog cannot be armed"
	ph "[ -f /host/pro1x-modules/$KVER/modules.dep ]" && ok "/host/pro1x-modules/$KVER" ||
	    bad "/host/pro1x-modules/$KVER is missing: OUT=$OUT 65-kernel-update.sh prepare"
	if [ -f "$OUT/initrd.img-$KVER" ]; then
		ok "initramfs $OUT/initrd.img-$KVER"
	elif ph "[ -f $UP/initrd.img-$KVER ]"; then
		ok "initramfs $UP/initrd.img-$KVER (on the phone)"
	else
		bad "no initramfs for $KVER: 65-kernel-update.sh prepare makes it"
	fi
	ph 'mountpoint -q /sys/fs/pstore && ls /sys/fs/pstore | head -3' > "$T/pstore" 2>&1 &&
	    ok "pstore mounted ($(paste -sd' ' "$T/pstore"))" ||
	    bad "/sys/fs/pstore is not mounted: the candidate's console would be lost"
	if (make_dtb) > "$T/dtb.err" 2>&1; then
		ok "device tree: the candidate's, live memory banks, ramoops node as the running one"
	else
		bad "device tree: $(cat "$T/dtb.err")"
	fi
	echo "== command line"
	make_cmdline | fold -w 76 -s | sed 's/^/  /'
	[ "$fails" = 0 ] || echo "!! $fails check(s) failed"
	return "$fails"
}

case $what in
check)
	checks
	;;
load)
	checks || die "not loading: fix the FAILs first"
	make_dtb
	make_cmdline > "$T/cmdline"
	log "to $UP: Image-$KVER.gz, kexec-$KVER.dtb, kexec-$KVER.cmdline"
	ph "mkdir -p $UP"
	put "$OUT/Image.gz" "$UP/Image-$KVER.gz"
	put "$T/kexec.dtb" "$UP/kexec-$KVER.dtb"
	put "$T/cmdline" "$UP/kexec-$KVER.cmdline"
	[ ! -f "$OUT/initrd.img-$KVER" ] || put "$OUT/initrd.img-$KVER" "$UP/initrd.img-$KVER"
	[ "$(ph "sha256sum < $UP/Image-$KVER.gz" | cut -c1-64)" = "$(sha "$OUT/Image.gz")" ] &&
	[ "$(ph "sha256sum < $UP/kexec-$KVER.dtb" | cut -c1-64)" = "$(sha "$T/kexec.dtb")" ] ||
	    die "the files arrived damaged on the phone"
	log "kexec -l (nothing restarts)"
	ph "set -e; cd $UP
		kexec -l Image-$KVER.gz --initrd=initrd.img-$KVER --dtb=kexec-$KVER.dtb \
		    --command-line=\"\$(cat kexec-$KVER.cmdline)\"
		echo $KVER > kexec-loaded
		echo \"kexec_loaded: \$(cat /sys/kernel/kexec_loaded)\""
	log "loaded; the jump needs the user's OK: OK=yes $0 go $K"
	;;
go)
	[ "${OK:-}" = yes ] || die "the jump restarts the phone: only with the user's OK, as OK=yes $0 go $K"
	checks > "$T/checks" 2>&1 || { cat "$T/checks"; die "not jumping"; }
	[ "$(ph 'cat /sys/kernel/kexec_loaded')" = 1 ] &&
	[ "$(ph "cat $UP/kexec-loaded 2>/dev/null")" = "$KVER" ] || die "$KVER is not loaded: $0 load $K"
	date -Iseconds > "$T/at"
	log "one-shot leste: the candidate's menu arms the watchdog and loads qcom_wdt, as on trial"
	ph '/usr/local/sbin/pro1x-boot next leste'
	log "the APSS watchdog (32 s), sync, read-only, kexec -e ($(cat "$T/at"))"
	ph 'set -e
		busybox devmem 0x0f017008 32 0; busybox devmem 0x0f017004 32 1
		busybox devmem 0x0f017010 32 0xfffff; busybox devmem 0x0f017014 32 0xfffff
		busybox devmem 0x0f017008 32 1
		sync; echo s > /proc/sysrq-trigger; echo u > /proc/sysrq-trigger; sleep 2
		busybox devmem 0x0f017004 32 1
		nohup kexec -e > /dev/null 2>&1 &' || true
	log "jumped; then: $0 after $K"
	;;
after)
	log "waiting for the phone (at most 12 minutes)"
	# "<version> kexec" for the test's start (its marker on the command
	# line), "<version> boot" for a start from boot_b: for run 0 both are
	# 7.1.0-pro1x.5
	whoami="echo \"\$(uname -r) \$(grep -q 'pro1x.kexectest=$KVER' /proc/cmdline && echo kexec || echo boot)\""
	now=
	for i in $(seq 72); do
		if lsusb -d 05c6:900e > /dev/null 2>&1; then
			d=$RUNS/ramdump-kexec-$K-$(date +%H%M)
			log "crash-dump mode (05c6:900e)"
			if [ "${OK:-}" = yes ]; then
				need_space "$RUNS" 50
				sudo -n qdl ramdump -o "$d" "$QDL_FILTER" ||
				    die "qdl ramdump failed; the phone is still in 900e (docs/device-day-2026-10-31.md, ways back 4)"
				python3 -I "$(dirname "$0")/crashdump.py" "$d" > "$d/decoded.txt" 2>&1 || true
				log "dump in $d (decoded.txt); the phone resets into boot_b now"
				sleep 30
			else
				echo "take the dump (it resets the phone): sudo -n qdl ramdump -o $d '$QDL_FILTER'"
				echo "or: OK=yes $0 after $K"
				exit 2
			fi
		fi
		bash "$(dirname "$0")/phone.sh" usbnet > /dev/null 2>&1 || true
		now=$(CONNECT_TIMEOUT=8 ph "$whoami" 2> /dev/null) && break
		now=
		sleep 10
	done
	case $now in
	"$KVER kexec")
		# pro1x.12 went down about 40 s after its start, ssh never came up
		log "up with $KVER (the test's start): watching it until it has run ${MIN_UP:-300} s"
		while :; do
			up=$(CONNECT_TIMEOUT=8 ph "$whoami; cut -d. -f1 /proc/uptime" 2> /dev/null) &&
			    [ "${up%%$'\n'*}" = "$KVER kexec" ] || {
				log "$KVER is gone: waiting for what comes back"
				exec bash "$0" after "$K"
			}
			up=${up##*$'\n'}
			[ "$up" -ge "${MIN_UP:-300}" ] && break
			sleep 15
		done
		log "$KVER has run $up s; its logs:"
		bash "$(dirname "$0")/collect.sh" "kexec-$K-up" || true ;;
	"")	log "no answer after 12 minutes: the power key [HAND], then collect.sh"; exit 1 ;;
	*)	log "back with ${now% *} from boot_b: $KVER did not stay up; its console is in the pstore now"
		bash "$(dirname "$0")/collect.sh" "kexec-$K" || true
		echo "  then: grep -n 'pretimeout\\|rcu.*stall\\|Bluetooth\\|remoteproc\\|Call trace' $RUNS/kexec-$K*/pstore.txt" ;;
	esac
	;;
esac
