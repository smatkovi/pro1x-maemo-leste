#!/bin/bash
# The PC test of the boot menu's FreeBSD and KolibriOS starts in
# initramfs/local-top/pro1x-loop-root: the real script in a bubblewrap
# sandbox, every tool it calls replaced by a stand-in that logs the call,
# so that the order of the watchdog's arming, /host's umount, the SMMU
# hand-over (arm_smmu.kexec_bypass, kernel patch 0040) and kexec -e can be
# checked, and what follows a kexec -e that comes back.
#
#   bash tests/boot-menu-kexec/run.sh [repo]
#
# Needs bwrap and a block device node /dev/loop0 on the PC (it stands in
# for UFS's partition; only stat()ed, never opened). Writes only below
# /tmp/boot-menu-kexec-test. kexec -e always "fails" here, so a case's log
# shows everything up to the jump and the way back.
set -euo pipefail
REPO=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
SCRIPT=$REPO/initramfs/local-top/pro1x-loop-root
T=/tmp/boot-menu-kexec-test
command -v bwrap > /dev/null || { echo "bwrap missing"; exit 1; }
[ -b /dev/loop0 ] || { echo "no /dev/loop0 on this PC"; exit 1; }
fails=0

stubs() {   # the tools, written into the sandbox's /usr/bin overlay
	local B=$1
	mkdir -p "$B"
	for t in devmem mount umount sync reboot modprobe losetup mknod; do
		printf '#!/bin/sh\necho "%s $*" >> /t/trace\n' "$t" > "$B/$t"
	done
	cat > "$B/kexec" <<'EOF'
#!/bin/sh
case "$1" in
-e) echo "kexec -e bypass=[$(cat /sys/module/arm_smmu/parameters/kexec_bypass 2>/dev/null)] i2c=[$(cat /sys/bus/platform/devices/4a84000.i2c/power/control)] host=[$(cat /t/mounted)]" >> /t/trace
    exit 1 ;;
-s) echo "kexec -s -l $3" >> /t/trace; [ ! -e /t/load-fails ] ;;
*) echo "kexec $*" >> /t/trace ;;
esac
EOF
	cat > "$B/umount" <<'EOF'
#!/bin/sh
echo "umount $*" >> /t/trace; echo no > /t/mounted
EOF
	cat > "$B/mount" <<'EOF'
#!/bin/sh
echo "mount $*" >> /t/trace; echo yes > /t/mounted
EOF
	cat > "$B/blkid" <<'EOF'
#!/bin/sh
case "$*" in
*-U*) echo /dev/sda13 ;;
*BLOCK_SIZE*) echo 4096 ;;
*TYPE*) echo ext4 ;;
esac
EOF
	cat > "$B/uname" <<'EOF'
#!/bin/sh
echo 7.1.0-pro1x.test
EOF
	cat > "$B/pro1x-bootmenu" <<'EOF'
#!/bin/sh
# the user's choice if the test set one, else the preselected entry
if [ -s /t/menu ]; then cat /t/menu; exit 0; fi
while [ $# -gt 0 ]; do [ "$1" = -d ] && { echo "$2"; exit 0; }; shift; done
EOF
	cat > "$B/pro1x-boot" <<'EOF'
#!/bin/sh
case "$1" in
trial-check) echo none ;;
entries) printf 'leste|Maemo Leste|/maemo-leste.img\nfreebsd|FreeBSD|/freebsd/kernel.bin\nkolibri|KolibriOS|/kolibri/kernel.bin\n' ;;
alarm-check) echo "none not-by-the-rtc"; exit 1 ;;
take-next) cat /t/next 2>/dev/null; rm -f /t/next ;;
get-default) echo leste ;;
get-timeout) echo 5 ;;
set-current) echo "set-current $2" >> /t/trace ;;
*) echo "pro1x-boot $*" >> /t/trace ;;
esac
EOF
	chmod +x "$B"/*
}

# case NAME NEXT MENU PARAM LOADFAILS: one start; prints its trace
run_case() {
	local name=$1 next=$2 menu=$3 param=$4 loadfails=$5
	rm -rf /tmp/boot-menu-kexec-test; mkdir -p "$T/t" "$T/scripts/local-top" "$T/host/freebsd" \
		"$T/host/kolibri" "$T/host/pro1x-boot" "$T/conf"
	: > "$T/t/trace"; echo yes > "$T/t/mounted"; : > "$T/t/kmsg"
	[ -z "$next" ] || echo "$next" > "$T/t/next"
	[ -z "$menu" ] || echo "$menu" > "$T/t/menu"
	[ "$loadfails" = 0 ] || : > "$T/t/load-fails"
	: > "$T/host/maemo-leste.img"; echo "pro1x.test=1" > "$T/host/freebsd/cmdline"
	cat > "$T/scripts/functions" <<'EOF'
log_begin_msg() { echo "log: $*" >> /t/trace; }
log_failure_msg() { echo "failure: $*" >> /t/trace; }
log_warning_msg() { echo "warning: $*" >> /t/trace; }
panic() { echo "panic: $*" >> /t/trace; exit 1; }
EOF
	# /dev/kmsg is a file here: append instead of truncating (the only
	# change to the script under test)
	sed 's|> /dev/kmsg|>> /dev/kmsg|' "$SCRIPT" > "$T/scripts/local-top/pro1x-loop-root"
	echo "pro1x.backing=UUID=abc pro1x.image=/maemo-leste.img" > "$T/t/cmdline"
	local sysargs=(--tmpfs /sys
		--dir /sys/bus/platform/devices/4a84000.i2c/power
		--dir /sys/module/watchdog/parameters)
	local setup="echo auto > /sys/bus/platform/devices/4a84000.i2c/power/control; : > /sys/module/watchdog/parameters/open_timeout"
	if [ "$param" = 1 ]; then
		sysargs+=(--dir /sys/module/arm_smmu/parameters)
		setup="$setup; : > /sys/module/arm_smmu/parameters/kexec_bypass"
	fi
	stubs "$T/bin"
	bwrap --unshare-all --uid 0 --gid 0 --chdir / \
		--setenv PATH /sbin:/usr/bin \
		--ro-bind /usr /usr --ro-bind "$T/bin" /sbin \
		--symlink usr/bin /bin \
		--symlink usr/lib /lib --symlink usr/lib /lib64 \
		--ro-bind /etc /etc --proc /proc --dev /dev \
		--ro-bind "$T/t/cmdline" /proc/cmdline \
		--bind "$T/t/kmsg" /dev/kmsg \
		--ro-bind /dev/loop0 /dev/sda13 \
		--tmpfs /tmp "${sysargs[@]}" \
		--bind "$T/t" /t --bind "$T/host" /host \
		--bind "$T/scripts" /scripts --bind "$T/conf" /conf \
		/bin/sh -c "$setup; sh /scripts/local-top/pro1x-loop-root" \
		> "$T/t/out" 2>&1 || echo "exit $?" >> "$T/t/trace"
	cat "$T/t/trace"
	sed 's/^/kmsg: /' "$T/t/kmsg"
}

expect() {   # expect NAME TRACE PATTERN...: the patterns in this order
	local name=$1 trace=$2 p n=0 i found
	local -a lines
	shift 2
	mapfile -t lines <<< "$trace"
	for p in "$@"; do
		found=
		for ((i = n; i < ${#lines[@]}; i++)); do
			if [[ ${lines[i]} == *"$p"* ]]; then found=1; n=$((i + 1)); break; fi
		done
		if [ -z "$found" ]; then
			echo "FAIL $name: '$p' missing or out of order"; printf '%s\n' "$trace" | sed 's/^/     /'
			fails=$((fails + 1)); return
		fi
	done
	echo "ok   $name"
}
absent() {   # absent NAME TRACE PATTERN
	if printf '%s\n' "$2" | grep -qF -- "$3"; then
		echo "FAIL $1: '$3' should not be there"; fails=$((fails + 1))
	else echo "ok   $1 (no '$3')"; fi
}

ARM="devmem 0x0f017008 32 0
devmem 0x0f017004 32 1
devmem 0x0f017010 32 0xfffff
devmem 0x0f017014 32 0xfffff
devmem 0x0f017008 32 1"

# 1. FreeBSD by `pro1x-boot next` (one-shot), a kernel with 0040; kexec -e
#    returns (failure) so the trace shows what precedes the jump and after
tr=$(run_case freebsd-next freebsd "" 1 0)
expect "freebsd: armed before umount, param, reload, jump" "$tr" \
	"kexec -s -l /host/freebsd/kernel.bin" "sync" \
	"devmem 0x0f017008 32 0" "devmem 0x0f017004 32 1" "devmem 0x0f017010 32 0xfffff" \
	"devmem 0x0f017014 32 0xfffff" "devmem 0x0f017008 32 1" \
	"umount /host" "devmem 0x0f017004 32 1" \
	"kexec -e bypass=[4804000.ufshc] i2c=[on] host=[no]"
expect "freebsd: kexec -e failed: param cleared, watchdog stopped, Leste with the one-shot watchdog" "$tr" \
	"kexec -e bypass" "devmem 0x0f017008 32 0" "mount -t ext4" \
	"failure: kexec failed" "set-current leste" \
	"devmem 0x0f017008 32 0" "devmem 0x0f017008 32 1" "modprobe qcom_wdt"
absent "freebsd: no UFS rebind" "$tr" "unbind"
expect "freebsd: the RAM console lines" "$tr" "kmsg: <3>pro1x-loop-root: starting freebsd: watchdog armed" \
	"kmsg: <3>pro1x-loop-root: kexec -e failed: not starting freebsd"

# 2. FreeBSD from an old kernel (no arm_smmu.kexec_bypass)
tr=$(run_case freebsd-oldkernel freebsd "" 0 0)
expect "old kernel: FreeBSD refused, Leste starts" "$tr" \
	"failure: kernel 7.1.0-pro1x.test cannot hand UFS over to FreeBSD" \
	"set-current leste" "devmem 0x0f017008 32 1" "modprobe qcom_wdt"
absent "old kernel: no kexec" "$tr" "kexec -s"
absent "old kernel: /host not unmounted" "$tr" "umount"

# 3. KolibriOS: the watchdog the same way, UFS untouched
tr=$(run_case kolibri kolibri "" 1 0)
expect "kolibri: armed before umount, reload before the jump" "$tr" \
	"kexec -s -l /host/kolibri/kernel.bin" "devmem 0x0f017008 32 1" \
	"umount /host" "devmem 0x0f017004 32 1" "kexec -e bypass=[] i2c=[on]"

# 4. kexec -s -l fails: nothing armed in the branch, /host stays
tr=$(run_case loadfails freebsd "" 1 1)
expect "load fails: Leste" "$tr" "kexec -s -l" "failure: kexec failed" "set-current leste"
absent "load fails: no umount" "$tr" "umount"

# 5. FreeBSD chosen by hand (no one-shot): after a failed kexec -e the
#    watchdog stays stopped for Leste
tr=$(run_case byhand "" 1 1 0)
expect "by hand: stopped after the failed jump" "$tr" "kexec -e bypass=[4804000.ufshc]" \
	"devmem 0x0f017008 32 0" "set-current leste"
last=$(printf '%s\n' "$tr" | grep '^devmem 0x0f017008' | tail -1)
[ "$last" = "devmem 0x0f017008 32 0" ] && echo "ok   by hand: last watchdog write disables it" ||
	{ echo "FAIL by hand: last write '$last'"; fails=$((fails + 1)); }
absent "by hand: no qcom_wdt" "$tr" "modprobe qcom_wdt"

# 6. Leste (default): no watchdog at all
tr=$(run_case leste "" "" 1 0)
absent "leste: no devmem" "$tr" "devmem"
absent "leste: no kexec" "$tr" "kexec"

echo
[ "$fails" = 0 ] && echo "all cases passed" || { echo "$fails failed"; exit 1; }
