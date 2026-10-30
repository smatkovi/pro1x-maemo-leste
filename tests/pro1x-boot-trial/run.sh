#!/bin/bash
# The PC test of a kernel's trial start (docs/kernel-update.md, "Trial
# starts"): the real tools/pro1x-boot and tools/pro1x-boot-trial against a
# GPT in an image file (PRO1X_BOOT_GPT) with boot_a and boot_b, and a model
# of the bootloader's A/B choice at every power-on. It counts, per case,
# how many starts of slot b a kernel gets before the phone ends in slot a
# (Sailfish OS), and where a start that went well leaves the slot.
#
#   bash tests/pro1x-boot-trial/run.sh [-v]
#
# Needs sgdisk. Writes only below /tmp/pro1x-boot-trial-test (removed at the
# start of the next run). Never touches a real disk, /dev/watchdog or the
# PC's reboot: the trial's tools run with uname, pgrep, logger, sleep and
# reboot replaced, and pro1x-boot-trial's /dev/watchdog pointed at a file.
#
# The bootloader model (an assumption: Qualcomm's ABL as its sources
# describe it; on the phone only "the tries counted down to 0, then slot a"
# was seen, 2026-10-24): the active slot (bit 50) starts if it is marked
# successful (bit 54); else, if it has tries left (bits 51-53), one is
# taken and it starts; else it is marked unbootable (bit 55) and the other
# slot starts.
#
# What a start of slot b does, as in initramfs/local-top/pro1x-loop-root:
# `pro1x-boot trial-check` first; then the system chosen: a Linux system
# whose user interface comes up runs pro1x-boot-trial (confirms), one that
# hangs does nothing (the user's power key or the watchdog end it),
# FreeBSD/KolibriOS from the menu confirm in the initramfs.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
VERBOSE=0
[ "${1:-}" != -v ] || VERBOSE=1
command -v sgdisk > /dev/null || { echo "sgdisk missing"; exit 1; }

T=/tmp/pro1x-boot-trial-test
rm -rf /tmp/pro1x-boot-trial-test
mkdir -p "$T/bin" "$T/host"
IMG=$T/disk.img

# --- the stand-ins for the tools the trial uses --------------------------
cat > "$T/bin/uname" <<'EOF'
#!/bin/sh
[ "${1:-}" = -r ] && { cat "$PRO1X_TEST/kver"; exit 0; }
exec /usr/bin/uname "$@"
EOF
cat > "$T/bin/pgrep" <<'EOF'
#!/bin/sh
# the user interface is up when the test says so
[ "$(cat "$PRO1X_TEST/ui" 2>/dev/null)" = up ]
EOF
cat > "$T/bin/logger" <<'EOF'
#!/bin/sh
shift 2 2>/dev/null; echo "logger: $*" >> "$PRO1X_TEST/log"
EOF
cat > "$T/bin/sleep" <<'EOF'
#!/bin/sh
exit 0
EOF
cat > "$T/bin/reboot" <<'EOF'
#!/bin/sh
echo reboot >> "$PRO1X_TEST/rebooted"
EOF
chmod 755 "$T/bin/"*
sed "s|/dev/watchdog|$T/watchdog|g" "$REPO/tools/pro1x-boot-trial/pro1x-boot-trial" > "$T/pro1x-boot-trial"
chmod 755 "$T/pro1x-boot-trial"
export PRO1X_TEST=$T PRO1X_BOOT_HOST=$T/host PRO1X_BOOT_GPT=$IMG PATH="$T/bin:$PATH"
CONF=$T/host/pro1x-boot

boot() { sh "$REPO/tools/pro1x-boot/pro1x-boot" "$@"; }
say() { [ "$VERBOSE" = 0 ] || echo "   $*" >&2; }

# --- the GPT and its attribute bits -------------------------------------
# (new_disk makes boot_a partition 1 and boot_b partition 2)
num() { case $1 in boot_a) echo 1 ;; boot_b) echo 2 ;; esac; }
attr() {    # attr LABEL: the 64-bit attribute flags as hex
	sgdisk -i "$(num "$1")" "$IMG" | sed -n 's/^Attribute flags: //p'
}
bit() { echo $(( (0x$(attr "$1") >> $2) & 1 )); }
tries() { echo $(( (0x$(attr "$1") >> 51) & 7 )); }
setbits() {     # setbits LABEL set|clear BIT...
	local n=$(num "$1") op=$2 a=()
	shift 2
	for b in "$@"; do a+=(-A "$n:$op:$b"); done
	sgdisk "${a[@]}" "$IMG" > /dev/null
}
state() { echo "b: tries $(tries boot_b) successful $(bit boot_b 54) unbootable $(bit boot_b 55) active $(bit boot_b 50); a: active $(bit boot_a 50)"; }

new_disk() {
	rm -f "$IMG"
	truncate -s 40M "$IMG"
	sgdisk -o -n 1:2048:+8M -c 1:boot_a -n 2:0:+8M -c 2:boot_b "$IMG" > /dev/null
	# as on the phone with Leste running from a confirmed boot_b: a not
	# active, b active, 7 tries, successful (pro1x-boot slot b)
	setbits boot_a set 49 51 52 53 54
	setbits boot_b set 48 49 50 51 52 53 54
	rm -rf /tmp/pro1x-boot-trial-test/host
	mkdir -p "$T/host"
	rm -f "$T/log" "$T/rebooted"
}

# The bootloader at one power-on: prints the slot it starts.
abl() {
	local s=b o=a
	[ "$(bit boot_b 50)" = 1 ] || { s=a o=b; }
	if [ "$(bit "boot_$s" 55)" = 0 ] && [ "$(bit "boot_$s" 54)" = 1 ]; then
		echo $s
	elif [ "$(bit "boot_$s" 55)" = 0 ] && [ "$(tries "boot_$s")" -gt 0 ]; then
		local t=$(( $(tries "boot_$s") - 1 )) c=()
		for i in 0 1 2; do
			if [ $(( (t >> i) & 1 )) = 1 ]; then c+=(set); else c+=(clear); fi
		done
		local n=$(num "boot_$s")
		sgdisk -A "$n:${c[0]}:51" -A "$n:${c[1]}:52" -A "$n:${c[2]}:53" "$IMG" > /dev/null
		echo $s
	else
		setbits "boot_$s" set 55
		setbits "boot_$s" clear 50
		setbits "boot_$o" set 50
		echo $o
	fi
}

# The kernel in boot_b (what `65-kernel-update.sh flash` wrote last)
BOOT_B=7.1.0-pro1x.4
flash() {   # flash KERNEL TRIES: 65-kernel-update.sh flash
	BOOT_B=$1
	[ "$2" = 0 ] || boot trial "$1" "$2" > /dev/null
}

# One power-on: OUTCOME
#   good       the system's user interface comes up (pro1x-boot-trial)
#   early      the kernel hangs before the initramfs (no trial-check, no
#              watchdog: the user's power key ends it)
#   hang       it hangs after the initramfs (trial-check ran; on trial the
#              watchdog resets it)
#   noui       the system runs, its user interface never comes up
#   freebsd    FreeBSD or KolibriOS chosen in the menu
#   noconfirm  a system that runs but has no pro1x-boot-trial, like the
#              MeeGo copy that 66-boot-trial-install.sh left out on 10-29
# Prints where it ended: "b:<trial-check>:<outcome>" ("-" for no
# trial-check) or "a" (Sailfish OS).
power_on() {
	local out=$1 slot check
	slot=$(abl)
	if [ "$slot" = a ]; then echo a; return; fi
	echo "$BOOT_B" > "$T/kver"
	if [ "$out" = early ]; then echo "b:-:early"; return; fi
	check=$(boot trial-check 2> /dev/null) || check=none
	case $out in
	good)	echo up > "$T/ui"; "$T/pro1x-boot-trial" wait ;;
	noui)	echo down > "$T/ui"; echo 30 > "$CONF/trial-wait" 2> /dev/null || true
		"$T/pro1x-boot-trial" wait ;;
	freebsd) [ "$check" != running ] || boot trial-ok > /dev/null ;;
	hang|noconfirm) ;;
	esac
	rm -f "$CONF/trial-wait"
	echo "b:$check:$out"
}

fails=0
check() {   # check NAME EXPECTED ACTUAL
	if [ "$2" = "$3" ]; then
		echo "ok   $1"
	else
		echo "FAIL $1"
		echo "     expected: $2"
		echo "     got:      $3"
		fails=$((fails + 1))
	fi
}
run() {     # run OUTCOME...: the power-ons, their results in one line
	local r=() x
	for o in "$@"; do
		x=$(power_on "$o")
		say "$o -> $x; $(state)"
		r+=("$x")
	done
	echo "${r[*]}"
}

# --- the cases ----------------------------------------------------------
# 1. The phone today: pro1x.4 confirmed, then pro1x.5 flashed with the
#    default trial (65 flash, TRIES=3), never started.
new_disk
flash 7.1.0-pro1x.5 3
check "pro1x.5 flashed on trial: 3 tries, not successful" \
    "b: tries 3 successful 0 unbootable 0 active 1; a: active 0" "$(state)"
check "pro1x-boot list shows the trial" \
    "trial:   kernel 7.1.0-pro1x.5 on slot b, boot_b 3 tries left, not successful" \
    "$(boot list | grep '^trial:')"

# 2. It comes up at the first power-on: confirmed; the next start ends the
#    trial (slot b successful, 7 tries); after that no try is taken.
check "good start: confirmed, the next start ends the trial" \
    "b:running:good b:passed:good b:none:good" "$(run good good good)"
check "after the trial: 7 tries, successful" \
    "b: tries 7 successful 1 unbootable 0 active 1; a: active 0" "$(state)"
check "the confirmation stopped the watchdog (magic close)" "V" "$(cat "$T/watchdog" 2>/dev/null)"

# 3. It never comes up: three starts of slot b, the fourth power-on is
#    Sailfish OS (slot a), slot b unbootable.
new_disk; flash 7.1.0-pro1x.5 3
check "every start hangs: 3 starts of slot b, then slot a" \
    "b:running:hang b:running:hang b:running:hang a" "$(run hang hang hang hang)"
check "slot b unbootable, slot a active" \
    "b: tries 0 successful 0 unbootable 1 active 0; a: active 1" "$(state)"
# the way back from Sailfish OS: the saved boot_b written back (dd), the
# trial ended by hand (pro1x-boot works on /userdata there)
BOOT_B=7.1.0-pro1x.4
boot trial-stop > /dev/null
check "back from Sailfish: old image + trial-stop, slot b as before" \
    "b:none:good" "$(run good)"

new_disk; flash 7.1.0-pro1x.5 3
check "every start hangs before the initramfs: the same count, then slot a" \
    "b:-:early b:-:early b:-:early a" "$(run early early early early)"

# 4. One failed start is survived: hang, then good, then the trial ends.
new_disk; flash 7.1.0-pro1x.5 3
check "hang, good: confirmed on the 2nd start, ended on the 3rd" \
    "b:running:hang b:running:good b:passed:good b:none:good" "$(run hang good good good)"

# 5. Two failed starts are not: the third start comes up and is confirmed,
#    but no try is left for the start that would end the trial.
new_disk; flash 7.1.0-pro1x.5 3
check "hang, hang, good: confirmed, yet the next power-on is slot a" \
    "b:running:hang b:running:hang b:running:good a" "$(run hang hang good good)"
check "  ... and the confirmation is still there for slot b" "yes" \
    "$([ -f "$CONF/trial-ok" ] && echo yes || echo no)"
# from Sailfish OS: pro1x-boot slot b puts slot b back on trial (the
# trial's tries); its first start ends the trial, which was confirmed
boot slot b > /dev/null
check "  ... from Sailfish: pro1x-boot slot b, the next start ends it" \
    "b:passed:good b:none:good" "$(run good good)"

# 6. No user interface for trial-wait: pro1x-boot-trial reboots (counted
#    as a failed start, like a hang).
new_disk; flash 7.1.0-pro1x.5 3
check "no user interface: a reboot, not confirmed" \
    "b:running:noui" "$(run noui)"
check "  ... pro1x-boot-trial rebooted" "reboot" "$(cat "$T/rebooted" 2>/dev/null)"
check "  ... and did not confirm" "no" "$([ -f "$CONF/trial-ok" ] && echo yes || echo no)"

# 7. FreeBSD (or KolibriOS) chosen in the menu during a trial: the Linux
#    kernel got as far as the menu, that confirms.
new_disk; flash 7.1.0-pro1x.5 3
check "FreeBSD from the menu during a trial counts as good" \
    "b:running:freebsd b:passed:good" "$(run freebsd good)"

# 8. A system without pro1x-boot-trial (the MeeGo copy, left out by 66 on
#    2026-10-29 because it was mounted read only) never confirms.
new_disk; flash 7.1.0-pro1x.5 3
check "three starts of a system without pro1x-boot-trial: then slot a" \
    "b:running:noconfirm b:running:noconfirm b:running:noconfirm a" \
    "$(run noconfirm noconfirm noconfirm noconfirm)"

# 9. The day's flash on top of a pending trial. pro1x.5 started once and
#    was confirmed (2 tries left, not successful yet), then pro1x.10 is
#    flashed with the default trial: a fresh trial with 3 tries.
new_disk; flash 7.1.0-pro1x.5 3
run good > /dev/null
flash 7.1.0-pro1x.10 3
check "pro1x.10 flashed with TRIES=3 over the pending trial: a fresh trial" \
    "b: tries 3 successful 0 unbootable 0 active 1; a: active 0" "$(state)"
check "  ... runs like case 2" "b:running:good b:passed:good" "$(run good good)"

# 10. The same with TRIES=0: the pro1x.5 trial file stays; pro1x.10's first
#     start drops it as stale and marks slot b successful -- but until then
#     the bootloader counts the old trial's tries: 2 left, so a pro1x.10
#     that hangs gets two starts, and one that comes up ends it at once.
new_disk; flash 7.1.0-pro1x.5 3
run good > /dev/null
flash 7.1.0-pro1x.10 0
check "TRIES=0 over a pending trial: pro1x.10 comes up, stale trial dropped" \
    "b:stale:good b:none:good" "$(run good good)"
check "  ... slot b successful, 7 tries" \
    "b: tries 7 successful 1 unbootable 0 active 1; a: active 0" "$(state)"
new_disk; flash 7.1.0-pro1x.5 3
run good > /dev/null
flash 7.1.0-pro1x.10 0
check "TRIES=0 over a pending trial: pro1x.10 hangs early, 2 starts, then slot a" \
    "b:-:early b:-:early a" "$(run early early early)"
# ... but a pro1x.10 that gets as far as the initramfs and hangs later has
# made slot b successful already (stale trial): no way back by itself
new_disk; flash 7.1.0-pro1x.5 3
run good > /dev/null
flash 7.1.0-pro1x.10 0
check "TRIES=0 over a pending trial: pro1x.10 hangs late, slot b for ever" \
    "b:stale:hang b:none:hang b:none:hang b:none:hang" "$(run hang hang hang hang)"

# 11. TRIES=0 after the trial ended: nothing is armed. A kernel that hangs
#     before the initramfs is started again at every power-on, never slot a.
new_disk; flash 7.1.0-pro1x.5 3
run good good > /dev/null
flash 7.1.0-pro1x.10 0
check "TRIES=0 on a confirmed slot b: a hanging kernel, slot b for ever" \
    "b:-:early b:-:early b:-:early b:-:early b:-:early" \
    "$(run early early early early early)"

# 12. pro1x-boot backs up the GPT before it changes the attributes
check "pro1x-boot backed up the GPT before changing it" "yes" \
    "$([ -s "$CONF/gpt-disk.img.bak" ] && echo yes || echo no)"

# 13. pro1x-boot trial refuses 1 try (the confirming start takes one) and 8.
new_disk
check "trial with 1 try refused" "refused" "$(boot trial 7.1.0-pro1x.10 1 > /dev/null 2>&1 && echo taken || echo refused)"
check "trial with 8 tries refused" "refused" "$(boot trial 7.1.0-pro1x.10 8 > /dev/null 2>&1 && echo taken || echo refused)"

echo
if [ "$fails" = 0 ]; then
	echo "all cases passed"
else
	echo "$fails case(s) failed"
	exit 1
fi
