#!/bin/bash
# Writes tools/devday/owned-paths.txt, the paths fix-ownership.sh checks and
# fixes: what this repository's image and phone scripts put into a system's
# root, and the directories above them. Runs on the PC, in the repository:
#
#   bash tools/devday/mk-owned-paths.sh          (writes owned-paths.txt)
#   bash tools/devday/mk-owned-paths.sh -        (to stdout)
#
# Why: until bb39fa8 and 0f271a9 (2026-10-31) the image builds copied with
# the copier's owner and modes. scripts/45's "cp -a rootfs-overlay/. /" and
# 40's rsync of the kernel's modules and the firmware kept the PC checkout's
# uid 1000 (and the PC's gid 984), which is Leste's "user": /, /etc, /usr,
# /etc/init.d and the overlay's files belonged to "user" in the Leste image
# of 2026-10-24, and so in its MeeGo copy (meego/45 rsyncs Leste's root).
# kali/20, arch/20 and luneos/20 copied Leste's /lib/modules and
# /lib/firmware with cp -a, owner and all, and their overlays from a git
# archive (umask 0002: 0775 directories, 0664 files) with cp -a.
#
# The list, one entry per line, tab-separated: KIND PATH SOURCES
#   path   the path itself (a * or ? in its last part: every match)
#   tree   the directory and everything below it on the same file system
#   known  not ours, but owned by a user (uid >= 1000) on purpose: the
#          check names it and leaves it alone
# Each overlay file (every one the history has had: a file removed since
# 2026-10-24 is still in the phone's images), the copies' destinations
# below, and every directory above them, up to /.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUTF=${1:-$REPO/tools/devday/owned-paths.txt}

declare -A SRC KIND
add() {     # add KIND PATH SOURCE
	local k=$1 p=$2 s=$3
	if [ -n "${KIND[$p]:-}" ]; then
		# a tree wins over a path; "parent" only where nothing else applies
		[ "$k" = tree ] && KIND[$p]=tree
		[ "$k" = known ] && KIND[$p]=known
		case ",${SRC[$p]}," in *",$s,"*) ;; *)
			if [ "${SRC[$p]}" = parent ]; then SRC[$p]=$s
			elif [ "$s" != parent ]; then SRC[$p]="${SRC[$p]},$s"; fi ;;
		esac
	else
		KIND[$p]=$k
		SRC[$p]=$s
	fi
}
parents() { # parents PATH: every directory above PATH, to /
	local d=$1
	while [ "$d" != / ]; do
		d=$(dirname "$d")
		add path "$d" parent
	done
}
ours() {    # ours KIND PATH SOURCE: an entry and the directories above it
	add "$1" "$2" "$3"
	parents "$2"
}

# --- the overlays, every file they have had ------------------------------
# rootfs-overlay: Leste (45), and the MeeGo copy (meego/45 copies Leste)
for t in rootfs-overlay kali/rootfs-overlay arch/rootfs-overlay luneos/rootfs-overlay; do
	while IFS= read -r f; do
		[ -n "$f" ] || continue
		ours path "/${f#"$t"/}" "$t"
	done < <(git -C "$REPO" log --all --format= --name-only -- "$t" | sort -u)
done

# --- what the steps copy or install outside the overlays -----------------
# 40-make-rootfs.sh: the kernel's modules and the firmware (rsync -a, the
# builder's uid until bb39fa8); kali/20, luneos/20: cp -a of Leste's
# (lib/firmware/updates); arch/20 the same into /usr/lib (merged /usr:
# /lib/modules and /usr/lib/modules are one directory there)
ours tree /lib/modules 40,kali/20,luneos/20
ours tree /lib/firmware 40,kali/20,luneos/20
ours tree /usr/lib/modules arch/20
ours tree /usr/lib/firmware/updates arch/20
# 40, 65: the initramfs's hooks and scripts (root runs them)
ours tree /etc/initramfs-tools 40,65
ours path '/boot/vmlinuz-7.1*' 40,65
ours path '/boot/config-7.1*' 40,65
ours path '/boot/initrd.img-7.1*' 40,65
# 65-kernel-update.sh: every kernel's modules on userdata, bound onto
# /lib/modules/<version> at boot (the running kernel's: cp -a of Leste's)
ours tree /host/pro1x-modules 65
# 45: the UCM profile, the audio and boot helpers, the MeeGo session
ours tree /usr/share/alsa/ucm2/Pro1X 45,64,kali/20,arch/20,luneos/20
ours tree /usr/share/alsa/ucm2/sm4250 45,kali/20,arch/20
ours path '/usr/bin/pro1x-*' 45,49-apps,arch/20
ours path '/usr/local/bin/pro1x-*' 45,49-apps
ours path '/usr/local/sbin/pro1x-*' 45,46,66,arch/20
ours path '/etc/init.d/pro1x-*' 45,46,49-mobile-data,66
ours tree /usr/lib/pro1x-meego 45,meego/41
ours path /mnt/harmattan 45
# 46: the sensor stack's configuration and data
ours tree /usr/share/pro1x 46,kali/20,arch/20
ours path /etc/libssc.conf 46
ours path /etc/pro1x-autosuspend.conf 46
ours path '/etc/dbus-1/system.d/org.pro1x.*' 46,meego/41
ours path /usr/share/dbus-1/system.d/org.pro1x.Steps.conf arch/20
ours path /usr/share/dbus-1/system.d/net.hadess.SensorProxy.conf arch/20
ours path '/etc/udev/rules.d/*pro1x*' 45,46,arch/20
# 48: libcamera's tuning files
ours tree /usr/local/share/libcamera 48
# meego/41: the MeeGo copy's helpers
ours tree /etc/pro1x-meego meego/41
# kali/20, arch/20, luneos/20: the Pro1X stack copied from Leste (cp -a)
ours tree /usr/lib/pro1x kali/20,arch/20,luneos/20
ours path '/etc/systemd/system/pro1x-*' kali/20,arch/20,luneos/20,66,71
# arch/30, 40-42: Arch's build trees, and what arch/30 copies with cp -a
# from the N9's root and from its build tree
ours tree /usr/src/hildon arch/40,arch/41,arch/42
ours tree /usr/src/harmattan-qt5 arch/30
ours tree /usr/src/harmattan-qt5-build arch/30
for d in themes/base themes/blanco themes/pro1x fonts/nokia sounds/ui-tones \
    sounds/ring-tones sounds/vibra ngfd/events.d; do
	ours tree "/usr/share/$d" arch/30
done
# 64, 71, 72: the files they replaced, saved with cp -a (owner and all; a
# restore puts them back as they were)
ours tree /var/backups/pro1x-ucm 64
ours tree /var/backups/pro1x-power 71
ours tree /var/backups/pro1x-audio 72
# tools/devday/deploy.sh (kolibri block)
ours path /usr/local/sbin/kolibri-home devday

# --- owned by a user on purpose ---------------------------------------------
add known /var/cache/alarmd "alarmd's cache: its postinst chowns it to the user"
add known /srv/tftp "atftpd's root (Kali): its postinst chowns it to nobody"
add known '/var/spool/cron/crontabs/*' "a user's crontab"
add known '/var/mail/*' "a user's mailbox"
add known '/var/spool/mail/*' "a user's mailbox"

# no entry may lie in a home or in what the fix never touches
for p in "${!KIND[@]}"; do
	[ "${KIND[$p]}" = known ] && continue
	case $p in
	/home|/home/*|/root|/root/*|/tmp|/tmp/*|/var/tmp|/var/tmp/*|/var/lib/*|/proc*|/sys*|/dev*|/run*)
		echo "error: $p is not ours to fix" >&2; exit 1 ;;
	esac
done

OVERLAYS="rootfs-overlay kali/rootfs-overlay arch/rootfs-overlay luneos/rootfs-overlay"
# shellcheck disable=SC2086
commit=$(git -C "$REPO" log -1 --format=%h -- $OVERLAYS)
# shellcheck disable=SC2086
dirty=$(git -C "$REPO" status --porcelain -- $OVERLAYS | head -1)
{
	echo "# The paths tools/devday/fix-ownership.sh checks: written by"
	echo "# tools/devday/mk-owned-paths.sh, overlays at $commit${dirty:+ (with uncommitted changes)}; do not edit."
	echo "# KIND<tab>PATH<tab>SOURCES (path: the path; tree: it and all below; known: a user's on purpose)"
	for p in "${!KIND[@]}"; do
		printf '%s\t%s\t%s\n' "${KIND[$p]}" "$p" "${SRC[$p]}"
	done | LC_ALL=C sort -t "$(printf '\t')" -k2,2
} > "${OUTF/#-//dev/stdout}"
[ "$OUTF" = - ] || echo "$OUTF: $(grep -vc '^#' "$OUTF") entries" >&2
