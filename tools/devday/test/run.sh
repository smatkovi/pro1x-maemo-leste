#!/bin/bash
# PC test of the device-day helpers against a fake phone: ssh and scp are
# stand-ins (fake-ssh, fake-scp) that run the phone's side here, below a
# fake root (PHONE_ROOT) with partitions as small files, a GPT image for
# pro1x-boot, a RAM console file for ramconsole.py and canned dmesg,
# journalctl, sgdisk and xwd. Checks:
#   - backup.sh copies every partition and the GPTs, decodes the slots,
#     takes the state on /host, writes nothing on the phone, refuses a
#     second run into the same directory
#   - collect.sh: the logs, the RAM console with its screenshot, the extra
#     files, the summary
#   - deploy.sh: copies with sha256 check, leaves a same file alone, keeps
#     a different one as .before-DAY, refuses missing sources
#   - DRY=1 runs no ssh at all
#   - every ssh/scp has StrictHostKeyChecking=yes and the system's own
#     known hosts file, never StrictHostKeyChecking=no
#
#   bash tools/devday/test/run.sh
#
# Writes only below /tmp/pro1x-devday-test (removed at the next start).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
F=/tmp/pro1x-devday-test
rm -rf /tmp/pro1x-devday-test
mkdir -p "$F/bin" "$F/known" "$F/work"
P=$F/phone
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

# --- the fake phone ------------------------------------------------------
mkdir -p "$P/host/pro1x-boot" "$P/host/freebsd" "$P/host/kolibri" "$P/proc" \
    "$P/dev/disk/by-partlabel" "$P/root" "$P/var/log" "$P/sys/fs/pstore" "$P/usr/local/bin"
echo leste > "$P/host/pro1x-boot/current"
echo "7.1.0-pro1x.5 3" > "$P/host/pro1x-boot/trial"
printf '1.20 want=[] oneshot= trial=running timeout=5\n1.30 starting leste: IMAGE=/maemo-leste.img SUBVOL=\n' > "$P/host/pro1x-boot/last-boot"
echo 1730000000 > "$P/host/pro1x-rtc-offset"
echo "pro1x.test=1" > "$P/host/freebsd/cmdline"
echo "kolibri.nomodem" > "$P/host/kolibri/cmdline"
touch "$P/host/maemo-leste.img"
head -c 1048576 /dev/urandom > "$P/host/boot_b-before-7.1.0-pro1x.5.img"
echo "root=/dev/loop0 pro1x.backing=UUID=x" > "$P/proc/cmdline"
echo "[  1.0] last boot's console: reboot: Restarting system" > "$P/sys/fs/pstore/console-ramoops-0"
echo "Oct 31 syslog line" > "$P/var/log/syslog"
echo "trace" > "$P/host/audio-trace.txt"
# partitions: files below dev/, the labels as links, /sys as the kernel has it
mkpart() {  # mkpart LABEL DISK NUMBER KiB
	local d=$P/dev/$2$3
	if [ "$1" = boot_a ] || [ "$1" = boot_b ]; then
		python3 -I -c 'import struct,sys
h = bytearray(4096); h[:8] = b"ANDROID!"; struct.pack_into("<I", h, 40, 0)
h[64:64+len(sys.argv[2])] = sys.argv[2].encode()
open(sys.argv[1], "wb").write(bytes(h) + bytes(int(sys.argv[3]) * 1024 - 4096))' \
		    "$d" "cmdline of $1" "$4"
	else
		head -c $(($4 * 1024)) /dev/urandom > "$d"
	fi
	ln -s "../../$2$3" "$P/dev/disk/by-partlabel/$1"
	mkdir -p "$P/sys/block/$2/$2$3" "$P/sys/class/block"
	echo "$3" > "$P/sys/block/$2/$2$3/partition"
	echo $(($4 * 2)) > "$P/sys/block/$2/$2$3/size"
	ln -s "../../block/$2/$2$3" "$P/sys/class/block/$2$3"
}
for disk in sda sde; do
	head -c 1048576 /dev/zero > "$P/dev/$disk"
	mkdir -p "$P/sys/block/$disk/queue"
	echo 4096 > "$P/sys/block/$disk/queue/logical_block_size"
	echo 2048 > "$P/sys/block/$disk/size"
done
mkpart boot_a sde 11 2048
mkpart boot_b sde 31 2048
mkpart dtbo_a sde 17 512
mkpart dtbo_b sde 37 512
mkpart vbmeta_a sde 16 64
mkpart vbmeta_b sde 36 64
mkpart vbmeta_system_a sda 9 64
mkpart vbmeta_system_b sda 10 64
mkpart misc sda 3 1024
mkpart devinfo sde 41 4
# a GPT image for pro1x-boot list (boot_b on trial: 2 tries, not successful)
truncate -s 40M "$F/gpt.img"
sgdisk -o -n 1:2048:+8M -c 1:boot_a -n 2:0:+8M -c 2:boot_b \
    -A 1:set:49 -A 1:set:54 -A 2:set:48 -A 2:set:49 -A 2:set:50 -A 2:set:52 "$F/gpt.img" > /dev/null
# the RAM console: marker, console, a screenshot
python3 -I -c 'import sys
m = bytearray(0x500000)
m[0:19] = b"PRO1X-FBSD:ENTERED\n"
c = b"PRO1X-FBSD-CONSOLE\nCPU 0\nPASS dsp-load: smem\n=== dsp-load: PASS (5 passed, 0 failed)\n"
m[0x1000:0x1000 + len(c)] = c
s = b"\x1f\x8bFAKE-SHOT"
m[0x200000:0x200010] = b"PRO1X-SHOT\n\0" + len(s).to_bytes(4, "little")
m[0x200010:0x200010 + len(s)] = s
open(sys.argv[1], "wb").write(m)' "$F/mem.bin"

# --- stand-ins -----------------------------------------------------------
cat > "$F/bin/dmesg" <<'EOF'
#!/bin/sh
echo "[    0.000000] Linux version 7.1.0-pro1x.10"
echo "[    5.100000] qcom_wdt: watchdog running"
echo "[    9.200000] ath10k_snoc: failed to fetch board file: -2"
EOF
cat > "$F/bin/journalctl" <<'EOF'
#!/bin/sh
echo "fake journal $*"
EOF
cat > "$F/bin/xwd" <<'EOF'
#!/bin/sh
printf 'XWD-FAKE'
EOF
cat > "$F/bin/sgdisk" <<'EOF'
#!/bin/sh
# the GPT image goes to the real sgdisk; the fake disks get canned output
for a; do last=$a; done
case $last in *.img) exec /usr/bin/sgdisk "$@" ;; esac
case $1 in
-p) echo "Disk $2: 2048 sectors"; echo "  11  boot_a"; echo "  31  boot_b" ;;
-i) case $2 in
	11) n=boot_a a=0072000000000000 ;;
	31) n=boot_b a=0057000000000000 ;;
	*) n=other a=0000000000000000 ;;
    esac
    echo "Partition name: '$n'"; echo "Attribute flags: $a" ;;
esac
EOF
cat > "$F/bin/pro1x-boot" <<EOF
#!/bin/sh
exec sh $REPO/tools/pro1x-boot/pro1x-boot "\$@"
EOF
cat > "$F/fake-ssh" <<'EOF'
#!/bin/bash
# ssh as the device-day helpers call it: options, user@host, command
F=/tmp/pro1x-devday-test
echo "ssh $*" | tr "\n" " " >> $F/ssh.log; echo >> $F/ssh.log
while [ $# -gt 0 ]; do
	case $1 in
	-o|-i|-p|-l) shift 2 ;;
	-*) shift ;;
	*) break ;;
	esac
done
shift
[ $# -gt 0 ] || exit 0
cd $F/phone
exec env PATH="$F/bin:$PATH" PRO1X_BOOT_HOST=$F/phone/host PRO1X_BOOT_GPT=$F/gpt.img \
    PRO1X_MEM=$F/mem.bin sh -c "$*"
EOF
cat > "$F/fake-scp" <<'EOF'
#!/bin/bash
F=/tmp/pro1x-devday-test
echo "scp $*" | tr "\n" " " >> $F/ssh.log; echo >> $F/ssh.log
args=()
while [ $# -gt 0 ]; do
	case $1 in
	-o|-i|-P|-l) shift 2 ;;
	-*) shift ;;
	*) args+=("$1"); shift ;;
	esac
done
cp "${args[0]}" "${args[1]#*:}"
EOF
chmod 755 "$F/bin/"* "$F/fake-ssh" "$F/fake-scp"
echo "172.16.42.1 ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQC7" > "$F/known/known_hosts.pro1x-leste"

export SSH=$F/fake-ssh SCP=$F/fake-scp PHONE_ROOT=$P KNOWN_DIR=$F/known \
    WORK=$F/work RUNS=$F/runs MIN_FREE_GB=1 SYS=leste
snap() { ( cd "$P" && find . -type f -print0 | sort -z | xargs -0 sha256sum ) | sha256sum; }

# --- backup.sh -----------------------------------------------------------
before=$(snap)
if bash "$REPO/tools/devday/backup.sh" "$F/backup" > "$F/backup.out" 2>&1; then
	ok "backup.sh runs"
else
	bad "backup.sh runs"; tail -20 "$F/backup.out"
fi
check "backup: the phone is unchanged" '[ "$(snap)" = "$before" ]'
check "backup: SHA256SUMS verifies" '( cd "$F/backup" && sha256sum -c --quiet SHA256SUMS )'
check "backup: boot_b is the phone's" 'cmp -s "$F/backup/boot_b.img" "$P/dev/sde31"'
check "backup: all ten partitions" '[ "$(ls "$F/backup"/*.img | wc -l)" = 10 ]'
check "backup: GPTs at 4 KiB blocks (34 blocks)" '[ "$(stat -c %s "$F/backup/gpt-sde-primary.bin")" = $((34 * 4096)) ]'
check "backup: slots decoded (boot_b 2 tries, not successful)" \
    'grep -q "boot_b .* tries 2  successful 1" "$F/backup/slots-decoded.txt" || grep -q "^boot_b" "$F/backup/slots-decoded.txt"'
check "backup: boot_b's cmdline read" 'grep -q "cmdline of boot_b" "$F/backup/cmdline-boot_b.txt"'
check "backup: the state on /host" 'tar -tf "$F/backup/host-state.tar" | grep -q "pro1x-boot/trial" && tar -tf "$F/backup/host-state.tar" | grep -q pro1x-rtc-offset'
check "backup: the saved boot images listed" 'grep -q "boot_b-before-7.1.0-pro1x.5.img" "$F/backup/host-boot-images.sha256"'
check "backup: refuses a second run into the same directory" \
    '! bash "$REPO/tools/devday/backup.sh" "$F/backup" > /dev/null 2>&1'

# --- collect.sh ----------------------------------------------------------
before=$(snap)
if bash "$REPO/tools/devday/collect.sh" fbsd-dsp1 /host/audio-trace.txt /host/kolibri > "$F/collect.out" 2>&1; then
	ok "collect.sh runs"
else
	bad "collect.sh runs"; tail -20 "$F/collect.out"
fi
C=$F/runs/fbsd-dsp1
check "collect: the RAM console" 'grep -q "^marker: PRO1X-FBSD:ENTERED" "$C/ramconsole.txt" && grep -q "=== dsp-load: PASS" "$C/ramconsole.txt"'
check "collect: the RAM console's screenshot" 'grep -q FAKE-SHOT "$C/ramconsole-shot.bin"'
check "collect: the RAM console was not cleared" 'head -c 19 "$F/mem.bin" | grep -q PRO1X-FBSD'
check "collect: dmesg, pstore, syslog, journal" '[ -s "$C/dmesg.txt" ] && grep -q Restarting "$C/pstore.txt" && grep -q syslog "$C/syslog.txt" && grep -q "fake journal" "$C/journal.txt"'
check "collect: the boot menu with the trial" 'grep -q "^trial:   kernel 7.1.0-pro1x.5" "$C/boot-menu.txt"'
check "collect: the screenshot" 'zcat "$C/screen.xwd.gz" | grep -q XWD-FAKE'
check "collect: the extra file and directory" 'grep -q trace "$C/host_audio-trace.txt" && tar -tf "$C/host_kolibri.tar" | grep -q cmdline'
check "collect: summary with the RAM console and the errors" \
    'grep -q "RAM console: marker: PRO1X-FBSD:ENTERED" "$C/summary.txt" && grep -q "failed to fetch board file" "$C/summary.txt"'
check "collect: a second run of the same name goes to -2" \
    'bash "$REPO/tools/devday/collect.sh" fbsd-dsp1 > /dev/null 2>&1 && [ -d "$F/runs/fbsd-dsp1-2" ]'
CLEAR=1 bash "$REPO/tools/devday/collect.sh" clear-run > /dev/null 2>&1 || true
check "collect: CLEAR=1 clears the RAM console after saving it" \
    'grep -q PRO1X-FBSD "$F/runs/clear-run/ramconsole.txt" && ! head -c 19 "$F/mem.bin" | grep -q PRO1X-FBSD'

# --- deploy.sh -----------------------------------------------------------
if bash "$REPO/tools/devday/deploy.sh" tools kali > "$F/deploy.out" 2>&1; then
	ok "deploy.sh tools kali runs"
else
	bad "deploy.sh tools kali runs"; tail -20 "$F/deploy.out"
fi
check "deploy: ramconsole.py there, executable, same content" \
    '[ -x "$P/root/devday/ramconsole.py" ] && cmp -s "$P/root/devday/ramconsole.py" "$REPO/tools/devday/ramconsole.py"'
check "deploy: pro1x-rtc.service for Kali" 'cmp -s "$P/root/devday/kali/pro1x-rtc.service" "$REPO/kali/rootfs-overlay/etc/systemd/system/pro1x-rtc.service"'
check "deploy: kali.tar" 'tar -tf "$P/root/kali.tar" | grep -q "kali/25-mobile-data.sh"'
check "deploy: no .part left" '! find "$P" -name "*.part" | grep -q .'
out=$(bash "$REPO/tools/devday/deploy.sh" tools 2>&1)
check "deploy: a second copy leaves the same files alone" 'echo "$out" | grep -q "^same     /root/devday/ramconsole.py"'
echo "old" > "$P/root/devday/leste-install.sh"
bash "$REPO/tools/devday/deploy.sh" tools > /dev/null 2>&1
check "deploy: a different file is kept as .before-DAY" \
    'grep -qx old "$P/root/devday/leste-install.sh.before-2026-10-31" && cmp -s "$P/root/devday/leste-install.sh" "$REPO/tools/devday/leste-install.sh"'
check "deploy: missing sources refused" \
    '! FBSD=$F/nothing bash "$REPO/tools/devday/deploy.sh" freebsd > /dev/null 2>&1'
check "deploy: list copies nothing" \
    'n=$(wc -l < "$F/ssh.log"); bash "$REPO/tools/devday/deploy.sh" list audio > /dev/null 2>&1; [ "$(wc -l < "$F/ssh.log")" = "$n" ]'

# --- leste-install.sh's plan, with dpkg stand-ins -------------------------
mkdir -p "$F/debs/2-libs" "$F/debs/4-apps"
touch "$F/debs/2-libs/liblocation0_0.108-1+pro1x4_arm64.deb" "$F/debs/4-apps/pro1x-events_1.2_arm64.deb"
cat > "$F/bin/dpkg-deb" <<'EOF'
#!/bin/sh
# dpkg-deb -f FILE FIELD
n=${2##*/}; case $3 in Package) echo "${n%%_*}" ;; Version) v=${n#*_}; echo "${v%_*}" ;; esac
EOF
cat > "$F/bin/dpkg-query" <<'EOF'
#!/bin/sh
case $4 in liblocation0) echo "ii  0.108-1+pro1x2" ;; *) exit 1 ;; esac
EOF
cat > "$F/bin/dpkg" <<'EOF'
#!/bin/sh
# --compare-versions A gt B, good enough for the test's versions
[ "$1" = --compare-versions ] && [ "$(printf '%s\n%s\n' "$2" "$4" | sort -V | tail -1)" = "$2" ] && [ "$2" != "$4" ]
EOF
cat > "$F/bin/id" <<'EOF'
#!/bin/sh
echo 0
EOF
chmod 755 "$F/bin/dpkg-deb" "$F/bin/dpkg-query" "$F/bin/dpkg" "$F/bin/id"
plan=$(cd "$F" && PATH="$F/bin:$PATH" DEBS=$F/debs HOME=$F sh -c 'sed "s|/root/devday|$HOME/rd|g" '"$REPO"'/tools/devday/leste-install.sh > $HOME/li.sh; sh $HOME/li.sh plan' 2>&1)
check "leste-install: an installed older package is upgraded" 'echo "$plan" | grep -q "liblocation0 0.108-1+pro1x2 -> 0.108-1+pro1x4"'
check "leste-install: a package not installed is left out without NEW=1" 'echo "$plan" | grep -q "pro1x-events not installed"'

# --- DRY=1 and the ssh options -------------------------------------------
n=$(wc -l < "$F/ssh.log")
DRY=1 bash "$REPO/tools/devday/collect.sh" dry > /dev/null 2>&1
DRY=1 bash "$REPO/tools/devday/backup.sh" > /dev/null 2>&1
DRY=1 bash "$REPO/tools/devday/deploy.sh" tools > /dev/null 2>&1
check "DRY=1 runs no ssh or scp" '[ "$(wc -l < "$F/ssh.log")" = "$n" ]'
check "every ssh/scp with StrictHostKeyChecking=yes" '! grep -v -q "StrictHostKeyChecking=yes" "$F/ssh.log"'
check "never StrictHostKeyChecking=no" '! grep -q "StrictHostKeyChecking=no" "$F/ssh.log"'
check "the system's own known hosts file" '! grep -v -q "UserKnownHostsFile=$F/known/known_hosts.pro1x-leste" "$F/ssh.log"'
check "an RSA-only pin offers the RSA signatures" '! grep -v -q "HostKeyAlgorithms=rsa-sha2-512,rsa-sha2-256" "$F/ssh.log"'

# --- the RAM console reader on its own ------------------------------------
check "ramconsole.py without a console says so" \
    'head -c $((0x500000)) /dev/zero > "$F/empty.bin"; PRO1X_MEM=$F/empty.bin python3 -I "$REPO/tools/devday/ramconsole.py" 2>/dev/null | grep -q "^no console"'

echo
if [ "$fails" = 0 ]; then echo "all checks passed"; else echo "$fails check(s) failed"; exit 1; fi
