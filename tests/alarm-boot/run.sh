#!/bin/bash
# Host test of the alarm power-on (docs/alarm-power-on.md): the boot menu's
# decision (tools/pro1x-boot alarm-check) on a faked PMIC, and the
# initramfs script (initramfs/local-top/pro1x-loop-root) around it.
#
#   bash tests/alarm-boot/run.sh                  pro1x-boot under the host's sh
#   INITRD=$WORK/out-pro1x.5/initrd.img-7.1.0-pro1x.5 bash tests/alarm-boot/run.sh
#       also under the initramfs's own busybox (aarch64, through
#       qemu-user's binfmt), and the whole local-top script in it, in a
#       bubblewrap sandbox: its blkid, mount, losetup, kexec, the menu
#       stubbed, /host a directory
#   LINUX=$WORK/build/linux ...   the register addresses checked against
#       Linux's rtc-pm8xxx.c too
#
# The PMIC is a file in the form of its regmap's debugfs "registers" file
# (one 9-byte line per register, 65536 of them), which pro1x-boot reads
# with dd at 9 * register, as on the phone.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
T=$(mktemp -d /tmp/alarm-boot-test.XXXXXX)
trap 'rm -rf "$T"' EXIT
fails=0 checks=0
ok() { checks=$((checks + 1)); echo "ok   $*"; }
bad() { checks=$((checks + 1)); fails=$((fails + 1)); echo "FAIL $*"; }
expect() { # name, wanted substring, got
	case $3 in *"$2"*) ok "$1" ;; *) bad "$1: wanted '$2' in: $3" ;; esac
}

# regs FILE reg=val ...: a registers file, all 0 but the ones given
# (reg and val in hex)
regs() {
	local f=$1
	shift
	awk -v spec="$*" 'BEGIN {
		n = split(spec, a, " ")
		for (i = 1; i <= n; i++) { split(a[i], kv, "="); v[strtonum("0x" kv[1])] = strtonum("0x" kv[2]) }
		for (r = 0; r < 65536; r++) printf "%04x: %02x\n", r, (r in v) ? v[r] : 0
	}' > "$f"
}
# pmic FILE PON_REASON TRIGGERS RTC ALARM [ALARM_CTRL [SUBTYPE]]: the
# registers of the PON (GEN2 unless SUBTYPE is 01..03) and the RTC
pmic() {
	local f=$1 pon=$2 trig=$3 rtc=$4 alarm=$5 actl=${6:-80} sub=${7:-04} r
	case $sub in 01|02|03) r=808 ;; *) r=8c0 ;; esac
	regs "$f" 805=$sub $r=$pon 880=$trig 6046=80 \
	    6048=$(printf %02x $((rtc & 255))) 6049=$(printf %02x $((rtc >> 8 & 255))) \
	    604a=$(printf %02x $((rtc >> 16 & 255))) 604b=$(printf %02x $((rtc >> 24 & 255))) \
	    6140=$(printf %02x $((alarm & 255))) 6141=$(printf %02x $((alarm >> 8 & 255))) \
	    6142=$(printf %02x $((alarm >> 16 & 255))) 6143=$(printf %02x $((alarm >> 24 & 255))) \
	    6146=$actl
}

# A userdata with Leste, FreeBSD and Arch (two snapshots), no Kali
H=$T/host
mkdir -p "$H/pro1x-boot/snapshots" "$H/freebsd"
: > "$H/maemo-leste.img"
: > "$H/arch.img"
: > "$H/freebsd/kernel.bin"
printf '3|before the update\n4|after it\n' > "$H/pro1x-boot/snapshots/arch"
echo 1761000000 > "$H/pro1x-rtc-offset"
R=$T/regs

# check SHELL...: alarm-check through that shell
run_checks() {
	local sh=("$@") out
	pb() { PRO1X_BOOT_HOST=$H PRO1X_BOOT_REGS=$R "${sh[@]}" "$REPO/tools/pro1x-boot/pro1x-boot" "$@"; }
	rm -f "$H/pro1x-boot/rtc-alarm" "$H/pro1x-boot/alarmboot"
	printf '3|before the update\n4|after it\n' > "$H/pro1x-boot/snapshots/arch"
	A=50000000

	echo leste > "$H/pro1x-boot/current"
	pmic "$R" 04 86 $((A + 14)) $A
	out=$(pb alarm-check) && ok "rtc start, alarm set in Leste: alarm" || bad "rtc start: failed: $out"
	expect "  owner leste" "leste late=14 " "$out"
	expect "  marker" "system=leste rtc_alarm=$A rtc_now=$((A + 14)) due=$((A + 1761000000 + 60)) " \
	    "$(tr '\n' ' ' < "$H/pro1x-boot/alarmboot")"
	expect "  state" "$A leste used" "$(cat "$H/pro1x-boot/rtc-alarm")"

	out=$(pb alarm-check) && bad "same alarm again: $out" || ok "the same alarm a second time: none"
	expect "  why" "none alarm-used-already" "$out"
	[ ! -e "$H/pro1x-boot/alarmboot" ] && ok "  no marker" || bad "  marker left"

	# a start by the power key; the alarm (from FreeBSD) came after it
	echo freebsd > "$H/pro1x-boot/current"
	A=50100000
	pmic "$R" 80 86 $((A - 3600)) $A
	out=$(pb alarm-check) && bad "power key: $out" || ok "power key start: none"
	expect "  why" "none not-by-the-rtc owner=freebsd" "$out"
	expect "  state" "$A freebsd" "$(cat "$H/pro1x-boot/rtc-alarm")"
	# Leste ran after it and left the alarm alone, then the alarm came
	echo leste > "$H/pro1x-boot/current"
	pmic "$R" 04 86 $((A + 20)) $A
	out=$(pb alarm-check) && ok "rtc start, FreeBSD's alarm left by Leste: alarm" || bad "freebsd alarm: $out"
	expect "  owner freebsd" "freebsd late=20" "$out"

	# Arch set an alarm while it ran snapshot 4, then 3 is gone, 4 too
	echo arch@4 > "$H/pro1x-boot/current"
	A=50200000
	pmic "$R" 04 86 $((A + 9)) $A
	out=$(pb alarm-check) || true
	expect "snapshot owner" "arch@4 late=9" "$out"
	printf '3|before the update\n' > "$H/pro1x-boot/snapshots/arch"
	A=50300000
	pmic "$R" 04 86 $((A + 9)) $A
	out=$(pb alarm-check) || true
	expect "snapshot gone: its system" "arch late=9" "$out"

	# Kali set it, but kali.img is not there (any more)
	echo kali > "$H/pro1x-boot/current"
	A=50400000
	pmic "$R" 04 86 $((A + 9)) $A
	out=$(pb alarm-check) && bad "kali: $out" || ok "owner not installed: none"
	expect "  why" "none owner-not-installed owner=kali" "$out"

	# by the RTC, but long after the alarm (a start much later)
	echo leste > "$H/pro1x-boot/current"
	A=50500000
	pmic "$R" 04 86 $((A + 1200)) $A
	out=$(pb alarm-check) && bad "late: $out" || ok "20 min past the alarm: none"
	expect "  why" "none not-this-alarm" "$out"
	# before the alarm's count
	A=50600000
	pmic "$R" 04 86 $((A - 60)) $A
	out=$(pb alarm-check) && bad "early: $out" || ok "before the alarm: none"
	# no alarm at all
	pmic "$R" 04 86 50700000 0 00
	out=$(pb alarm-check) && bad "no alarm: $out" || ok "alarm off (0): none"
	expect "  why" "none no-alarm" "$out"

	# the GEN1 layout (PON_REASON1 at 0x08), and the 32-bit count's top byte
	A=$((0x9a000010))
	pmic "$R" 04 04 $((A + 30)) $A 80 01
	out=$(pb alarm-check) && ok "GEN1 PON, a count above 2^31: alarm" || bad "gen1: $out"
	expect "  values" "rtc=$((A + 30)) alarm=$A on=1" "$out"

	# no PMIC (no debugfs): none, the state as it was
	cp "$H/pro1x-boot/rtc-alarm" "$T/state.before"
	out=$(PRO1X_BOOT_HOST=$H PRO1X_BOOT_REGS=$T/missing "${sh[@]}" "$REPO/tools/pro1x-boot/pro1x-boot" alarm-check) &&
	    bad "no pmic: $out" || ok "PMIC unreadable: none"
	expect "  why" "none pmic-unreadable" "$out"
	cmp -s "$T/state.before" "$H/pro1x-boot/rtc-alarm" && ok "  state kept" || bad "  state changed"
	# a broken registers file (the wrong format): unreadable as well
	printf 'garbage\n' > "$T/garbage"
	out=$(PRO1X_BOOT_HOST=$H PRO1X_BOOT_REGS=$T/garbage "${sh[@]}" "$REPO/tools/pro1x-boot/pro1x-boot" alarm-check) &&
	    bad "garbage: $out" || ok "registers file of another form: none"

	# pmic: the report
	pmic "$R" 84 86 50800010 50800070
	out=$(pb pmic)
	expect "pmic report reasons" "power-on reason: 0x84 (RTC alarm, power key), cold boot" "$out"
	expect "pmic report sources" "power-on sources: 0x86 (SMPL, RTC alarm, power key)" "$out"
	expect "pmic report alarm" "alarm at 50800070 (on, in 60 s)" "$out"
}

echo "== pro1x-boot under $(readlink -f /bin/sh)"
run_checks sh

BB=
if [ -n "${INITRD:-}" ]; then
	I=$T/initrd
	mkdir -p "$I"
	(cd "$I" && xz -dc "$INITRD" 2> /dev/null | cpio -idm --quiet 2> /dev/null ||
	    zstd -dc "$INITRD" | cpio -idm --quiet)
	BB=$I/usr/bin/busybox
	echo "== pro1x-boot under the initramfs's busybox ($("$BB" 2>&1 | head -1 | cut -d' ' -f1-2))"
	run_checks "$BB" sh
fi

if [ -n "${LINUX:-}" ]; then
	echo "== the addresses against Linux ($LINUX)"
	c=$LINUX/drivers/rtc/rtc-pm8xxx.c
	blk=$(sed -n '/^static const struct pm8xxx_rtc_regs pm8941_regs/,/^};/p' "$c")
	for want in ".read		= 0x6048" ".alarm_rw	= 0x6140" ".alarm_ctrl	= 0x6146" ".alarm_en	= BIT(7)"; do
		case $blk in *"$want"*) ok "rtc-pm8xxx pm8941_regs $want" ;; *) bad "rtc-pm8xxx: no '$want'" ;; esac
	done
	grep -q 'reg = <0x6000>, <0x6100>;' "$LINUX/arch/arm64/boot/dts/qcom/pm6125.dtsi" &&
	    ok "pm6125.dtsi: the RTC at 0x6000, its alarm at 0x6100" || bad "pm6125.dtsi rtc"
	grep -A2 'pm6125_pon: pon@800' "$LINUX/arch/arm64/boot/dts/qcom/pm6125.dtsi" | grep -q 'qcom,pm8998-pon' &&
	    ok "pm6125.dtsi: the PON at 0x800, qcom,pm8998-pon (GEN2)" || bad "pm6125.dtsi pon"
fi
# patch 0028 (the kernel's view) and pro1x-boot must agree
p=$REPO/device/kernel/patches/0028-power-reset-qcom-pon-power-on-reason-and-rtc-trigger.patch
if [ -f "$p" ]; then
	for d in "PON_GEN2_PON_REASON1		0xc0" "PON_GEN2_WARM_RESET_REASON1	0xc2" \
	    "PON_GEN2_POFF_REASON1		0xc5" "PON_GEN1_PON_REASON1		0x08" \
	    "PON_TRIGGER_EN			0x80" "PON_RTC			BIT(2)"; do
		grep -qF "+#define $d" "$p" || grep -qF "+#define  $d" "$p" && ok "0028: $d" || bad "0028: no '$d'"
	done
fi

# The initramfs script itself, in a sandbox whose root is the initramfs
if [ -n "$BB" ] && command -v bwrap > /dev/null; then
	echo "== initramfs/local-top/pro1x-loop-root in the initramfs (bubblewrap)"
	F=$I
	cp "$REPO/initramfs/local-top/pro1x-loop-root" "$F/scripts/local-top/pro1x-loop-root"
	cp "$REPO/tools/pro1x-boot/pro1x-boot" "$F/usr/sbin/pro1x-boot"
	mkdir -p "$F/stub" "$F/proc" "$F/host" "$F/sys/kernel/debug/regmap/0-00" "$F/log"
	# stubs: each logs its call; blkid finds userdata, the loop image is
	# ext4. The initramfs's busybox runs its own applets before PATH: those
	# are shell functions (fns), the others scripts in /stub
	for c in mount umount losetup mknod modprobe devmem sync reboot; do
		printf '%s() { echo "%s $*" >> /log/calls; }\n' "$c" "$c"
	done > "$F/stub/fns"
	# what initramfs-tools' init sets before local-top
	echo 'quiet=n; export quiet' >> "$F/stub/fns"
	printf '#!/bin/sh\necho "sgdisk $*" >> /log/calls\n' > "$F/stub/sgdisk"
	cat > "$F/stub/blkid" <<-'EOF'
	#!/bin/sh
	echo "blkid $*" >> /log/calls
	case "$*" in -U*) echo /dev/sda13 ;; *"-s TYPE"*) echo ext4 ;; esac
	EOF
	cat > "$F/stub/pro1x-bootmenu" <<-'EOF'
	#!/bin/sh
	echo "pro1x-bootmenu $*" >> /log/calls
	# the countdown ran out: the default
	while [ $# -gt 0 ]; do [ "$1" = -d ] && { echo "$2"; exit 0; }; shift; done
	echo 0
	EOF
	# kexec -e: the jump; Linux (this script) ends there
	cat > "$F/stub/kexec" <<-'EOF'
	#!/bin/sh
	echo "kexec $*" >> /log/calls
	[ "$1" != -e ] || kill -KILL $PPID
	EOF
	chmod 755 "$F"/stub/*
	: > "$F/sbin/pro1x-bootmenu" && chmod 755 "$F/sbin/pro1x-bootmenu"
	[ -x "$F/sbin/kexec" ] || { : > "$F/sbin/kexec"; chmod 755 "$F/sbin/kexec"; }
	echo "root=/dev/loop0 pro1x.backing=UUID=cffbd472-3eed-4cb3-b73e-d8088ac7fcdd pro1x.image=/maemo-leste.img" > "$F/proc/cmdline"
	echo "12.34 40.00" > "$F/proc/uptime"
	: > "$F/proc/mounts"
	# initramfs-tools' functions want these
	mkdir -p "$F/conf"
	: > "$F/conf/param.conf"
	boot() { # the local-top script once; prints the calls and the log
		rm -f "$F/log/calls"
		# userdata's block device: any block node will do (only stat()ed:
		# mount and blkid are stubs)
		bwrap --bind "$F" / --dev /dev --dev-bind "$BLK" /dev/sda13 --tmpfs /tmp --bind "$H" /host \
		    --setenv PATH /stub:/usr/sbin:/usr/bin:/sbin:/bin --unsetenv PRO1X_BOOT_HOST \
		    /bin/sh -c '. /stub/fns; . /scripts/local-top/pro1x-loop-root' > "$T/boot.out" 2>&1 || true
		cat "$F/log/calls" 2> /dev/null
		[ -z "${DEBUG:-}" ] || sed 's/^/   | /' "$T/boot.out" >&2
	}
	rm -f "$H/pro1x-boot/rtc-alarm" "$H/pro1x-boot/next"
	echo leste > "$H/pro1x-boot/default"
	R2=$F/sys/kernel/debug/regmap/0-00/registers
	BLK=$(find /dev -maxdepth 1 -name 'loop[0-9]*' -type b | head -1)

	# 1. power key: the menu as before
	echo leste > "$H/pro1x-boot/current"
	pmic "$R2" 80 86 60000000 0 00
	calls=$(boot)
	expect "power key: the menu" "pro1x-bootmenu -t 5 -d 0" "$calls"
	expect "  Leste's image" "losetup /dev/loop0 /host/maemo-leste.img" "$calls"
	expect "  the log" "alarm-check: none not-by-the-rtc" "$(cat "$H/pro1x-boot/last-boot")"

	# 2. Arch ran and set an alarm; the RTC powers the phone on: Arch at
	# once (its image, no menu), the watchdog as for a one-shot choice;
	# a one-shot "next freebsd" stays for the next start
	echo arch > "$H/pro1x-boot/current"
	echo freebsd > "$H/pro1x-boot/next"
	pmic "$R2" 04 86 60100012 60100000
	calls=$(boot)
	case $calls in *pro1x-bootmenu*) bad "alarm: the menu came: $calls" ;; *) ok "alarm: no menu" ;; esac
	expect "  Arch's image" "losetup /dev/loop0 /host/arch.img" "$calls"
	expect "  the watchdog" "modprobe qcom_wdt" "$calls"
	expect "  current" "arch" "$(cat "$H/pro1x-boot/current")"
	expect "  next kept" "freebsd" "$(cat "$H/pro1x-boot/next" 2> /dev/null)"
	expect "  marker" "system=arch" "$(cat "$H/pro1x-boot/alarmboot" 2> /dev/null)"
	expect "  the log" "starting arch: IMAGE=/arch.img" "$(cat "$H/pro1x-boot/last-boot")"

	# 3. the next start by the power key: next (freebsd) is taken now,
	# the marker gone, kexec with FreeBSD's arguments, no alarm word
	pmic "$R2" 80 86 60200000 60100000 00
	calls=$(boot)
	expect "then the power key: next=freebsd by kexec" "kexec -s -l /host/freebsd/kernel.bin --command-line=FreeBSD: hint.qcom_rtc.0.offset=1761000000" "$calls"
	case $calls in *alarmboot*) bad "  alarm word without an alarm" ;; *) ok "  no alarm word" ;; esac
	[ ! -e "$H/pro1x-boot/alarmboot" ] && ok "  marker gone" || bad "  marker left"

	# 4. FreeBSD ran (kexec above: current=freebsd) and set an alarm: by
	# the RTC, FreeBSD with pro1x.alarmboot=1 and the alarm's real time
	pmic "$R2" 04 86 60300015 60300000
	calls=$(boot)
	expect "FreeBSD's alarm: kexec with the alarm" "pro1x.alarmboot=1 pro1x.alarmdue=$((60300000 + 1761000000 + 60))" "$calls"
	case $calls in *pro1x-bootmenu*) bad "  menu" ;; *) ok "  no menu" ;; esac

	# 5. an alarm boot into a snapshot: arch@3, its overlay
	echo arch@3 > "$H/pro1x-boot/current"
	pmic "$R2" 04 86 60400010 60400000
	calls=$(boot)
	expect "a snapshot's alarm: its image" "losetup /dev/loop0 /host/arch.img" "$calls"
	case $calls in *pro1x-bootmenu*) bad "  menu" ;; *) ok "  no menu (not the snapshots' either)" ;; esac
	expect "  the log" "starting arch@3: IMAGE=/arch.img SUBVOL=@snapshots/3/snapshot" "$(cat "$H/pro1x-boot/last-boot")"

	# 6. an old Leste init script set next=leste at its shutdown: taken
	echo leste > "$H/pro1x-boot/current"
	echo leste > "$H/pro1x-boot/next"
	pmic "$R2" 04 86 60500010 60500000
	calls=$(boot)
	[ ! -e "$H/pro1x-boot/next" ] && ok "next=owner taken with the alarm" || bad "next=owner left"
	expect "  Leste" "losetup /dev/loop0 /host/maemo-leste.img" "$calls"
	grep -q . "$T/boot.out" && sed 's/^/   | /' "$T/boot.out" | head -5
fi

echo "$checks checks, $fails failures"
[ "$fails" -eq 0 ]
