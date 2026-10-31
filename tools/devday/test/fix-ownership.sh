#!/bin/bash
# Tests tools/devday/fix-ownership.sh on the PC, against two fake roots
# made the way the image builds before bb39fa8/0f271a9 made them:
#
#   leste: the overlay copied with cp -a from this checkout (uid 1000, the
#          PC's gid 984), the kernel's modules and the firmware with
#          rsync -a (40), the modules again into /host/pro1x-modules (65),
#          a 71 backup, files the overlay had once (pro1x-rmnet, ...); and
#          what must stay: the homes, /root, /tmp, a daemon's /var/lib,
#          /host (userdata's root: Android's uid 1000, 0771),
#          alarmd's cache, a crontab, a mailbox, a dpkg-statoverride entry,
#          links that point out of the root's system paths
#   kali:  merged /usr (lib -> usr/lib, sbin -> /usr/sbin), the overlay from
#          a git archive with umask 0002 (root-owned, 0775/0664) copied with
#          cp -a, Leste's modules and firmware copied with cp -a (kali/20),
#          a pacman mtree record that gives a path another group
#
# check, fix, check again (nothing of ours left), undo, check (as at first).
# Needs sudo -n. Works in /var/tmp/fix-ownership-test (removed before and
# after).
#
#   bash tools/devday/test/fix-ownership.sh
#   CLEAN=/mnt/x bash tools/devday/test/fix-ownership.sh
#       also: a correctly built image's root, mounted read-only, must give
#       nothing (exit 0)
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
T=$REPO/tools/devday/fix-ownership.sh
S=/var/tmp/fix-ownership-test
SH=${SH:-sh}
pass=0 fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
bad() { echo "FAIL  $1"; fail=$((fail + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
asroot() { sudo -n "$@"; }
run() { asroot env PATH="$PATH" UNDO_DIR="$S/undo" $SH "$T" "$@"; }

sudo -n true || { echo "needs sudo -n"; exit 1; }
asroot rm -rf /var/tmp/fix-ownership-test
mkdir -p "$S/stage"
F1=$S/leste F2=$S/kali

# --- the fake Leste -------------------------------------------------------------
asroot install -d -m 755 "$F1"
for d in bin etc etc/init.d usr usr/bin usr/share usr/share/alsa/ucm2/Pro1X \
    usr/share/alsa/ucm2/sm4250 usr/local/sbin lib/modules/6.1.0-44-arm64/kernel \
    lib/firmware/qcom var/lib/dpkg/info var/cache var/spool/cron/crontabs var/mail \
    var/backups var/lib/foo home/user root opt mnt boot proc sys dev run host; do
	asroot install -d -m 755 "$F1/$d"
done
asroot install -d -m 1777 "$F1/tmp"
asroot chmod 700 "$F1/root"
asroot tee "$F1/etc/passwd" > /dev/null <<'EOF'
root:x:0:0:root:/root:/bin/bash
user:x:1000:1000::/home/user:/bin/bash
nobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin
EOF
asroot tee "$F1/etc/group" > /dev/null <<'EOF'
root:x:0:
mail:x:8:
users:x:100:
crontab:x:101:
user:x:1000:
EOF
asroot tee "$F1/var/lib/dpkg/statoverride" > /dev/null <<'EOF'
root crontab 2755 /usr/bin/crontab
root crontab 0640 /etc/pro1x-autosuspend.conf
EOF
printf '/.\n/var\n/var/cache\n/var/cache/alarmd\n' | asroot tee "$F1/var/lib/dpkg/info/alarmd.list" > /dev/null
printf '/.\n/opt\n/opt/userapp\n' | asroot tee "$F1/var/lib/dpkg/info/foo:arm64.list" > /dev/null
asroot touch "$F1/lib/modules/6.1.0-44-arm64/kernel/a.ko" "$F1/lib/firmware/qcom/pkg.bin" "$F1/usr/bin/crontab"
asroot install -m 644 /dev/null "$F1/usr/share/alsa/ucm2/Pro1X/Pro1X.conf"
asroot ln -s ../Pro1X/Pro1X.conf "$F1/usr/share/alsa/ucm2/sm4250/sm4250.conf"
# 40: modules and firmware with rsync -a, as the PC's user built them
mkdir -p "$S/stage/modules/7.1.0+/kernel/drivers" "$S/stage/firmware/qcom/sm6115"
touch "$S/stage/modules/7.1.0+/kernel/drivers/x.ko" "$S/stage/modules/7.1.0+/modules.dep" \
    "$S/stage/firmware/qcom/sm6115/adsp.mbn"
touch "$S/stage/firmware/bad
name"
asroot rsync -a "$S/stage/modules/7.1.0+" "$F1/lib/modules/"
asroot rsync -a "$S/stage/firmware/" "$F1/lib/firmware/"
# 45: the overlay with cp -a, and files it had before
asroot cp -a "$REPO/rootfs-overlay/." "$F1/"
mkdir -p "$S/stage/old/etc/init.d" "$S/stage/old/etc/udev/rules.d" "$S/stage/old/usr/share/pro1x" \
    "$S/stage/old/usr/local/sbin"
touch "$S/stage/old/etc/init.d/pro1x-rmnet" "$S/stage/old/etc/udev/rules.d/91-pro1x-music-route.rules" \
    "$S/stage/old/usr/share/pro1x/60-usb-runtime-pm.rules" "$S/stage/old/etc/pro1x-autosuspend.conf"
chmod 664 "$S/stage/old/etc/pro1x-autosuspend.conf"
install -m 4755 /dev/null "$S/stage/old/usr/local/sbin/pro1x-setuid-test"
asroot cp -a "$S/stage/old/." "$F1/"
# 65: the running kernel's modules to userdata with cp -a; 71: a backup
asroot mkdir -p "$F1/host/pro1x-modules" "$F1/var/backups/pro1x-power/etc/conf.d"
asroot cp -a "$F1/lib/modules/7.1.0+" "$F1/host/pro1x-modules/"
asroot cp -a "$F1/etc/conf.d/mce" "$F1/var/backups/pro1x-power/etc/conf.d/mce"
# root's own but writable by all; root's with the PC's group
asroot chown 0:0 "$F1/etc/udev/rules.d/60-pro1x-ufs.rules"
asroot chmod 666 "$F1/etc/udev/rules.d/60-pro1x-ufs.rules"
asroot chown 0:984 "$F1/etc/mce/mce.ini.d/50-pro1x.ini"
# a link owned by the user, a tree that is a link into a home, a link
# in the middle of a listed path (into a home)
asroot chown -h 1000:1000 "$F1/usr/share/alsa/ucm2/sm4250/sm4250.conf"
asroot install -d -o 1000 -g 1000 "$F1/home/user/evil" "$F1/home/user/dbus"
asroot install -o 1000 -g 1000 -m 644 /dev/null "$F1/home/user/evil/x"
asroot ln -s /home/user/evil "$F1/etc/pro1x-meego"
asroot chown -h 1000:1000 "$F1/etc/pro1x-meego"
asroot cp -a "$F1/usr/share/dbus-1/." "$F1/home/user/dbus/"
asroot rm -r "$F1/usr/share/dbus-1"
asroot ln -s /home/user/dbus "$F1/usr/share/dbus-1"
# what is the user's, or a package's, on purpose
asroot install -o 1000 -g 1000 -m 644 /dev/null "$F1/home/user/.profile"
asroot install -o 1000 -g 1000 -m 644 /dev/null "$F1/root/notes"
asroot install -o 1000 -g 1000 -m 644 /dev/null "$F1/tmp/x"
asroot install -o 1000 -g 1000 -m 644 /dev/null "$F1/var/lib/foo/data"
asroot install -o 1000 -g 1000 -m 755 /dev/null "$F1/opt/userapp"
asroot install -o 1000 -g 1000 -m 755 /dev/null "$F1/opt/other"
asroot install -d -o 1000 -g 100 "$F1/var/cache/alarmd"
asroot install -o 1000 -g 101 -m 600 /dev/null "$F1/var/spool/cron/crontabs/user"
asroot install -o 1000 -g 8 -m 660 /dev/null "$F1/var/mail/user"
asroot ln -s ../mail "$F1/var/spool/mail"
# userdata's root, Sailfish's /data: Android's system user (uid 1000), 0771
asroot chown 1000:1000 "$F1/host"
asroot chmod 771 "$F1/host"

keep="home/user/.profile root/notes tmp/x var/lib/foo/data opt/userapp opt/other var/cache/alarmd
var/spool/cron/crontabs/user var/mail/user home/user/evil/x home/user/dbus usr/bin/crontab host"
snap() { (cd "$1" && for f in $keep; do asroot stat -c '%u:%g %a %n' "$f"; done); }

echo "== fake Leste"
before=$(snap "$F1")
out1=$(run check "$F1") && st=0 || st=$?
echo "$out1" | sed 's/^/    /' | head -${SHOW:-40}
check "check: exit 1 on the old build's state" '[ $st = 1 ]'
check "check: / owned by 1000:984" 'echo "$out1" | grep -q "^  owner      d  1000(user):984(?) 0755 -> 0(root):0(root) 0755  /  \[parent\]"'
check "check: /etc, /usr, /etc/init.d owned by the user" 'for p in /etc /usr /etc/init.d; do echo "$out1" | grep -q "^  owner .* $p  \[" || exit 1; done'
check "check: an overlay file" 'echo "$out1" | grep -q "^  owner      f  1000(user):984(?) 0755 -> 0(root):0(root) 0755  /etc/init.d/pro1x-cal  \[rootfs-overlay"'
check "check: a file the overlay had before" 'echo "$out1" | grep -q " /etc/init.d/pro1x-rmnet  \[rootfs-overlay"'
check "check: the modules' tree (40)" 'echo "$out1" | grep -q " /lib/modules/7.1.0+/kernel/drivers/x.ko  \[tree /lib/modules: 40"'
check "check: the firmware directory itself (40's rsync of lib/firmware/)" 'echo "$out1" | grep -q "^  owner      d .* /lib/firmware  \[tree /lib/firmware"'
check "check: /host/pro1x-modules (65)" 'echo "$out1" | grep -q " /host/pro1x-modules/7.1.0+/modules.dep  \[tree /host/pro1x-modules: 65\]"'
check "check: the 71 backup" 'echo "$out1" | grep -q " /var/backups/pro1x-power/etc/conf.d/mce  \[tree /var/backups/pro1x-power: 71\]"'
check "check: world-writable, root-owned: mode only" 'echo "$out1" | grep -q "^  mode       f  0666 -> 0644  /etc/udev/rules.d/60-pro1x-ufs.rules"'
check "check: root with the PC's group 984" 'echo "$out1" | grep -q "^  owner      f  0(root):984(?) 0644 -> 0(root):0(root) 0644  /etc/mce/mce.ini.d/50-pro1x.ini"'
check "check: dpkg-statoverride's owner" 'echo "$out1" | grep -q "^  owner+mode f  1000(user):984(?) 0664 -> 0(root):101(crontab) 0644  /etc/pro1x-autosuspend.conf  \[.*by dpkg-statoverride\]"'
check "check: setuid dropped with the owner" 'echo "$out1" | grep -q "4755 -> 0(root):0(root) 0755  /usr/local/sbin/pro1x-setuid-test .*setuid/setgid dropped"'
check "check: a link owned by the user: owner only" 'echo "$out1" | grep -q "^  owner      l  1000(user):1000(user) 0777 -> 0(root):0(root) 0777  /usr/share/alsa/ucm2/sm4250/sm4250.conf"'
check "check: a tree that is a link: not followed" 'echo "$out1" | grep -q "note: /etc/pro1x-meego: a link, not followed" && ! echo "$out1" | grep -q "/home/user/evil"'
check "check: a link in the middle of a path: left out" 'echo "$out1" | grep -q "note: /usr/share/dbus-1/system-services/org.gnome.GConf.service: a link inside its path, left out"'
check "check: a name with a line break: named, not checked" 'echo "$out1" | grep -q "not checked: a name with a line break in /lib/firmware"'
check "check: by hand: /var/lib/foo/data, /opt/userapp (package foo), /opt/other" \
    'echo "$out1" | grep -q "/var/lib/foo/data  (in no package)" && echo "$out1" | grep -q "/opt/userapp  (package foo)" && echo "$out1" | grep -q "/opt/other  (in no package)"'
check "check: /host (userdata's root) listed by hand, not ours" \
    'echo "$out1" | grep -q "^  d 1000(user):1000(user) 0771  /host  (in no package)" && ! echo "$out1" | grep -q "  /host  \["'
check "check: known: alarmd's cache, the crontab, the mailbox" \
    'echo "$out1" | grep -q "known: .* /var/cache/alarmd" && echo "$out1" | grep -q "known: .* /var/spool/cron/crontabs/user" && echo "$out1" | grep -q "known: .* /var/mail/user"'
check "check: not the homes, /root, /tmp" '! echo "$out1" | grep -E -q "/home/user/\.profile|/root/notes|/tmp/x"'
check "check: changed nothing" '[ "$(snap "$F1")" = "$before" ] && asroot stat -c %u "$F1/etc" | grep -qx 1000'
check "fix: refused when not root" '! UNDO_DIR="$S/undo" $SH "$T" fix "$F1" > /dev/null 2>&1'

fixout=$(run fix "$F1") && st=0 || st=$?
echo "$fixout" | grep -E '^(== |undo:)' | sed 's/^/    /'
check "fix: exit 0" '[ $st = 0 ]'
undo=$(echo "$fixout" | sed -n 's/^undo: .* undo \([^ ]*\).*/\1/p')
check "fix: an undo list" '[ -n "$undo" ] && asroot grep -q "^u	1000	984	0755	d	/etc$" "$undo"'
out2=$(run check "$F1") && st=0 || st=$?
check "after fix: none of our paths left" 'echo "$out2" | grep -q "^== 0 of our paths to fix"'
check "after fix: / /etc /usr root:root 0755" '[ "$(asroot stat -c "%u:%g %a" "$F1" "$F1/etc" "$F1/usr" | sort -u)" = "0:0 755" ]'
check "after fix: the statoverride's group" '[ "$(asroot stat -c "%u:%g %a" "$F1/etc/pro1x-autosuspend.conf")" = "0:101 644" ]'
check "after fix: setuid gone" '[ "$(asroot stat -c "%u:%g %a" "$F1/usr/local/sbin/pro1x-setuid-test")" = "0:0 755" ]'
check "after fix: the link's owner" '[ "$(asroot stat -c "%u:%g" "$F1/usr/share/alsa/ucm2/sm4250/sm4250.conf" "$F1/etc/pro1x-meego" | sort -u)" = "0:0" ]'
check "after fix: the user's, the package's and the known left alone" '[ "$(snap "$F1")" = "$before" ]'
check "after fix: still listed by hand (exit 1)" '[ $st = 1 ] && echo "$out2" | grep -q "/opt/other  (in no package)"'

undoout=$(run undo "$undo") && st=0 || st=$?
echo "$undoout" | tail -1 | sed 's/^/    /'
check "undo: exit 0" '[ $st = 0 ]'
out3=$(run check "$F1") || true
check "undo: check as at first" '[ "$out3" = "$out1" ]'
check "undo: setuid back" '[ "$(asroot stat -c "%u:%g %a" "$F1/usr/local/sbin/pro1x-setuid-test")" = "1000:984 4755" ]'

# --- the fake Kali --------------------------------------------------------------
echo "== fake Kali"
asroot install -d -m 755 "$F2"
for d in usr/bin usr/sbin usr/lib/modules usr/lib/firmware etc etc/ssl var/lib/dpkg/info \
    var/lib/pacman/local/xorg-fake-1-1 home/kali root tmp proc sys dev run host; do
	asroot install -d -m 755 "$F2/$d"
done
asroot ln -s usr/lib "$F2/lib"
asroot ln -s usr/bin "$F2/bin"
asroot ln -s /usr/sbin "$F2/sbin"
printf 'root:x:0:0:root:/root:/bin/bash\nkali:x:1000:1000::/home/kali:/bin/bash\n' | asroot tee "$F2/etc/passwd" > /dev/null
printf 'root:x:0:\nvideo:x:44:\nssl-cert:x:110:\nkali:x:1000:\n' | asroot tee "$F2/etc/group" > /dev/null
asroot install -d -g 110 -m 710 "$F2/etc/ssl/private"
printf '#mtree\n/set type=file uid=0 gid=0 mode=644\n/set type=dir uid=0 gid=44 mode=2775\n./etc/X11 time=1.0\n' |
    gzip | asroot tee "$F2/var/lib/pacman/local/xorg-fake-1-1/mtree" > /dev/null
# the chroot's repository: a git archive with tar.umask 0002, as root
asroot install -d "$S/kali-src"
git -C "$REPO" -c tar.umask=0002 archive HEAD kali | asroot tar -x -C "$S/kali-src"
asroot cp -a "$S/kali-src/kali/rootfs-overlay/." "$F2/"
# kali/20: Leste's modules and firmware with cp -a (the fake Leste as it was)
asroot cp -a "$F1/lib/modules/7.1.0+" "$F2/lib/modules/"
asroot mkdir -p "$F2/lib/firmware/updates"
asroot cp -a "$F1/lib/firmware/." "$F2/lib/firmware/updates/"
asroot install -o 1000 -g 1000 -m 644 /dev/null "$F2/home/kali/.profile"
kbefore=$(asroot stat -c '%u:%g %a %n' "$F2/etc/ssl/private" "$F2/home/kali/.profile")

kout1=$(run check "$F2/") && st=0 || st=$?
echo "$kout1" | sed 's/^/    /' | head -24
check "kali: exit 1" '[ $st = 1 ]'
check "kali: / /etc /usr group-writable (git archive)" 'for p in / /etc /usr /etc/systemd/system; do echo "$kout1" | grep -q "^  mode       d  0775 -> 0755  $p  \[" || exit 1; done'
check "kali: an overlay file 0664" 'echo "$kout1" | grep -q "^  mode       f  0664 -> 0644  /etc/systemd/system/pro1x-rtc.service  \[kali/rootfs-overlay"'
check "kali: modules through /lib -> usr/lib, once" '[ "$(echo "$kout1" | grep -c " /usr/lib/modules/7.1.0+/modules.dep ")" = 1 ] && ! echo "$kout1" | grep -q " /lib/modules/7"'
check "kali: firmware/updates through the link" 'echo "$kout1" | grep -q "^  owner      f  1000(kali):984(?) 0644 -> 0(root):0(root) 0644  /usr/lib/firmware/updates/qcom/sm6115/adsp.mbn"'
check "kali: pacman's group for /etc/X11, its g+w allowed" 'echo "$kout1" | grep -q "^  owner      d  0(root):0(root) 0775 -> 0(root):44(video) 0775  /etc/X11  \[parent, by pacman\]"'
check "kali: /etc/ssl/private (a package's group) not touched" '! echo "$kout1" | grep -q "/etc/ssl"'
check "kali: the links /lib /bin /sbin root's, not reported" '! echo "$kout1" | grep -E -q "  (/lib|/bin|/sbin)  \["'
kfix=$(run fix "$F2") && st=0 || st=$?
check "kali: fix exit 0" '[ $st = 0 ]'
kout2=$(run check "$F2") && st=0 || st=$?
check "kali: after fix: nothing (exit 0)" '[ $st = 0 ] && echo "$kout2" | grep -q "^== 0 of our paths to fix (owner 0, mode 0), 0 by hand"'
check "kali: after fix: the others left alone" '[ "$(asroot stat -c "%u:%g %a %n" "$F2/etc/ssl/private" "$F2/home/kali/.profile")" = "$kbefore" ]'
kundo=$(echo "$kfix" | sed -n 's/^undo: .* undo \([^ ]*\).*/\1/p')
run undo "$kundo" > /dev/null && kout3=$(run check "$F2") || true
check "kali: undo: as at first" '[ "$kout3" = "$kout1" ]'

# --- a correctly built image ------------------------------------------------------
if [ -n "${CLEAN:-}" ]; then
	echo "== $CLEAN (read-only)"
	cout=$(asroot env PATH="$PATH" $SH "$T" check "$CLEAN") && st=0 || st=$?
	echo "$cout" | sed 's/^/    /'
	check "clean image: nothing (exit 0)" '[ $st = 0 ]'
	check "clean image: fix refused (read-only)" '! asroot env PATH="$PATH" UNDO_DIR="$S/undo" $SH "$T" fix "$CLEAN" > /dev/null 2>&1'
fi

[ "${KEEP:-0}" = 1 ] || asroot rm -rf /var/tmp/fix-ownership-test
echo "== $pass passed, $fail failed"
[ "$fail" = 0 ] && echo "all checks passed"
[ "$fail" = 0 ]
