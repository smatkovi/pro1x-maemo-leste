#!/bin/sh
# Finds and repairs what the image builds before bb39fa8 and 0f271a9
# (2026-10-31) left in a system: paths owned by the PC's uid 1000 (Leste's
# "user", Kali's "kali", Arch's "alarm"; group 984, the PC's "users") where
# root should own them (/, /etc, /usr, /etc/init.d, the overlays' files, the
# kernel's modules, the firmware), and the overlays' files and directories
# made group-writable by a git archive's umask 0002. Either is a way to
# root. As root, on the phone (docs/device-day-2026-10-31.md):
#
#   sh fix-ownership.sh check [ROOT]      read only: lists what is wrong
#   sh fix-ownership.sh fix [ROOT]        changes our paths only (below),
#                                         writes an undo list first
#   sh fix-ownership.sh undo FILE [ROOT]  puts back what a fix changed
#
# ROOT is / (the running system) or another system's root mounted on a
# directory: read-only for check, read-write for fix. Never a system that
# runs (the MeeGo copy from Leste only while MeeGo is not booted):
#   mount -o loop,ro,noload /host/meego-leste.img /mnt/own   (ext4)
#   mount -o loop,ro,subvol=@ /host/arch.img /mnt/own        (Arch: Btrfs @)
#
# How it decides:
# - Our paths are owned-paths.txt's (next to this script; LIST= another):
#   every file the overlays have had, the destinations of the image steps'
#   and phone scripts' copies (trees such as /lib/modules, /lib/firmware,
#   /host/pro1x-modules, /var/backups/pro1x-*), and the directories above
#   them. tools/devday/mk-owned-paths.sh writes it from the repository.
#   Each must be root:root and neither group- nor world-writable, unless
#   ROOT's dpkg-statoverride or pacman's package records (mtree) give that
#   path another owner or mode: then that one. A link: its owner only. A
#   link is followed only as a path's first part (/lib -> usr/lib in Kali
#   and Arch); a path with a link further down is left out and named.
# - Anything else owned by a uid >= 1000 is listed with the package whose
#   file it is (dpkg's or pacman's lists), for a look by hand: fix does not
#   touch it. Not looked at: the homes (/home/*, the users' home
#   directories, /root's contents), /tmp/*, /var/tmp/*, /proc, /sys, /dev,
#   /run, and other file systems below ROOT (of /host, userdata, only
#   /host/pro1x-modules is ours: /host itself is Sailfish's /data, with
#   Android's owner, and is at most listed). The list's "known" entries
#   (alarmd's cache, a crontab, a mailbox) are named, not counted.
# fix: chown -h to the expected owner, g-w and o-w, a file whose owner
# changes loses setuid/setgid. Before changing anything it writes every
# path's owner and mode to UNDO_DIR (/root/devday) as
# fix-ownership-undo-<time>.txt, with ROOT and what is mounted there (an
# image's file): undo refuses another. Refuses unless root, and on a
# read-only ROOT. A path where the user had write access may have been
# changed; this does not check contents.
# Exit: 0 nothing found or all changed, 1 found or failed, 2 usage.
# Needs GNU find (Leste, the MeeGo copy, Kali, Arch): LuneOS has busybox's,
# so check and fix its image from Leste.
set -u
TAB=$(printf '\t')
NL='
'
HERE=$(cd "$(dirname "$0")" && pwd)
LIST=${LIST:-$HERE/owned-paths.txt}
UNDO_DIR=${UNDO_DIR:-/root/devday}

usage() { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 2; }
die() { echo "fix-ownership.sh: $*" >&2; exit 2; }

cmd=${1:-}
case $cmd in
check|fix) ROOT=${2:-/} ;;
undo) [ $# -ge 2 ] || usage; UNDO_FILE=$2; ROOT=${3:-} ;;
*) usage ;;
esac

# R: ROOT without its trailing slash ("" for /), the prefix of every path
setroot() {
	[ -d "$1" ] || die "$1 is not a directory"
	R=$(cd "$1" && pwd -P) || die "cannot enter $1"
	[ "$R" != / ] || R=
}
must_write() {
	[ "$(id -u)" = 0 ] || die "$cmd: run as root"
	[ -w "${R:-/}" ] || die "$cmd: ${R:-/} is read-only (mount it read-write)"
}

# resolve DIR: DIR inside ROOT, a link followed only as DIR's first part
# (/lib -> usr/lib); fails when a later part is a link
resolve() {
	p=$1 n=0
	while :; do
		first=${p#/}
		first=/${first%%/*}
		rest=${p#"$first"}
		[ -L "$R$first" ] || break
		n=$((n + 1))
		[ $n -le 8 ] || return 1
		t=$(readlink "$R$first") || return 1
		case $t in ../*|*/../*|*/..) return 1 ;; /*) ;; *) t=/$t ;; esac
		p=${t%/}$rest
	done
	q=$first r=${rest#/}
	while [ -n "$r" ]; do
		c=${r%%/*}
		case $r in */*) r=${r#*/} ;; *) r= ;; esac
		q=$q/$c
		[ ! -L "$R$q" ] || return 1
	done
	printf '%s\n' "$p"
}

# never ours, whatever a link says
forbidden() {
	case $1 in
	/home|/home/*|/root/*|/tmp/*|/var/tmp/*|/var/lib/*|/proc|/proc/*|/sys|/sys/*|/dev|/dev/*|/run|/run/*|/host) return 0 ;;
	esac
	return 1
}

# S KIND PATH SOURCES (our paths that exist), K PATH REASON (known), W NOTE;
# the plain paths first, then the patterns, then the trees: a path is
# reported under the first entry that has it
spec() {
	for pass in plain pattern tree; do
		spec_pass
	done
}
spec_pass() {
	grep -v '^#' "$LIST" | while IFS="$TAB" read -r kind p src; do
		case $kind:$p in
		tree:*) [ $pass = tree ] || continue ;;
		path:*[*?]*|known:*[*?]*) [ $pass = pattern ] || continue ;;
		path:*|known:*) [ $pass = plain ] || continue ;;
		*) continue ;;
		esac
		if [ "$p" = / ]; then
			printf 'S\t%s\t/\t%s\n' "$kind" "$src"
			continue
		fi
		dir=${p%/*} base=${p##*/}
		if [ -z "$dir" ]; then
			rdir=
		elif ! rdir=$(resolve "$dir"); then
			[ "$kind" = known ] ||
			    printf 'W\t%s: a link inside its path, left out\n' "$p"
			continue
		fi
		for f in "$R$rdir"/$base; do
			[ -e "$f" ] || [ -L "$f" ] || continue
			rel=${f#"$R"}
			if forbidden "$rel"; then
				printf 'W\t%s: %s is not ours to touch, left out\n' "$p" "$rel"
				continue
			fi
			k=$kind
			if [ "$k" = tree ] && [ -L "$f" ]; then
				printf 'W\t%s: a link, not followed (its owner is checked)\n' "$rel"
				k=path
			fi
			if [ "$k" = known ]; then
				printf 'K\t%s\t%s\n' "$rel" "$src"
			else
				printf 'S\t%s\t%s\t%s\n' "$k" "$rel" "$src"
			fi
		done
	done
}

# B KIND PATH SOURCES before the F UID GID MODE TYPE PATH of each entry
stats() {
	printf '%s\n' "$SPEC" | while IFS="$TAB" read -r t kind rel src; do
		[ "$t" = S ] || continue
		printf 'B\t%s\t%s\t%s\n' "$kind" "$rel" "$src"
		if [ "$kind" = tree ]; then
			find "$R$rel" -xdev \( -name "*$NL*" -printf 'N\t%h\n' -prune \) -o \
			    -printf 'F\t%U\t%G\t%m\t%y\t%p\n'
		else
			find "$R$rel" -maxdepth 0 -printf 'F\t%U\t%G\t%m\t%y\t%p\n'
		fi
	done
}

# U UID GID MODE TYPE PATH: owned by a uid >= 1000, outside the homes
scan() {
	set -- "${R:-/}" -xdev \( -path "$R/proc" -o -path "$R/sys" -o -path "$R/dev" \
	    -o -path "$R/run" -o -path "$R/tmp/*" -o -path "$R/var/tmp/*" -o -path "$R/home/*" \
	    -o -path "$R/root/*"
	for h in $(awk -F: '$3 >= 1000 && $6 ~ /^\/./ && $6 != "/nonexistent" { print $6 }' "$R/etc/passwd" 2> /dev/null); do
		set -- "$@" -o -path "$R$h"
	done
	find "$@" \) -prune -o -name "*$NL*" -printf 'N\t%h\n' -prune -o \
	    -uid +999 -printf 'U\t%U\t%G\t%m\t%y\t%p\n'
}

# P NAME UID, G NAME GID, O OWNER GROUP MODE PATH, M PATH UID GID MODE TYPE
records() {
	[ ! -f "$R/etc/passwd" ] || awk -F: '{ print "P\t" $1 "\t" $3 }' "$R/etc/passwd"
	[ ! -f "$R/etc/group" ] || awk -F: '{ print "G\t" $1 "\t" $3 }' "$R/etc/group"
	[ ! -f "$R/var/lib/dpkg/statoverride" ] ||
	    awk 'NF >= 4 { print "O\t" $1 "\t" $2 "\t" $3 "\t" $4 }' "$R/var/lib/dpkg/statoverride"
	set -- "$R"/var/lib/pacman/local/*/mtree
	[ -f "$1" ] || return 0
	for m in "$@"; do gzip -dc "$m" 2> /dev/null; done | awk '
		/^#/ { next }
		$1 == "/set" { for (i = 2; i <= NF; i++) { split($i, kv, "="); d[kv[1]] = kv[2] }; next }
		$1 == "/unset" { for (i = 2; i <= NF; i++) delete d[$i]; next }
		substr($1, 1, 2) == "./" && $1 !~ /\\/ {
			u = d["uid"]; g = d["gid"]; m = d["mode"]; t = d["type"]
			for (i = 2; i <= NF; i++) {
				split($i, kv, "=")
				if (kv[1] == "uid") u = kv[2]; else if (kv[1] == "gid") g = kv[2]
				else if (kv[1] == "mode") m = kv[2]; else if (kv[1] == "type") t = kv[2]
			}
			print "M\t" substr($1, 2) "\t" u "\t" g "\t" m "\t" t
		}'
}

# X PATH UID GID MODE TYPE NUID NGID NMODE WHAT OLD NEW SPEC KIND SOURCES BY NOTE
# U PATH UID GID MODE TYPE OWNER, K PATH OWNER REASON, N DIR, W NOTE
DECIDE='
BEGIN { FS = OFS = "\t"; lr = length(R) }
function rel(p) { p = substr(p, lr + 1); return p == "" ? "/" : p }
function oct(s,   n, i) { n = 0; for (i = 1; i <= length(s); i++) n = n * 8 + substr(s, i, 1); return n }
function bit(m, b) { return int(m / b) % 2 }
function who(u, g) { return u "(" ((u in PN) ? PN[u] : "?") "):" g "(" ((g in GN) ? GN[g] : "?") ")" }
$1 == "P" { PN[$3] = $2; PU[$2] = $3; next }
$1 == "G" { GN[$3] = $2; GG[$2] = $3; next }
$1 == "O" {
	u = $2; g = $3
	if (u ~ /^#/) u = substr(u, 2); else u = (u in PU) ? PU[u] : -1
	if (g ~ /^#/) g = substr(g, 2); else g = (g in GG) ? GG[g] : -1
	if (u >= 0 && g >= 0) { EU[$5] = u; EG[$5] = g; EM[$5] = oct($4); BY[$5] = "dpkg-statoverride" }
	next
}
$1 == "M" {
	if (BY[$2] != "dpkg-statoverride" && $3 != "" && $4 != "") {
		EU[$2] = $3; EG[$2] = $4; EM[$2] = ($5 == "" || $6 == "link") ? -1 : oct($5); BY[$2] = "pacman"
	}
	next
}
$1 == "K" { KR[$2] = $3; next }
$1 == "W" { print; next }
$1 == "B" { bkind = $2; bspec = $3; bsrc = $4; next }
$1 == "F" {
	r = rel($6)
	if (r in seen) next
	seen[r] = 1
	u = $2; g = $3; m = oct($4); t = $5
	eu = 0; eg = 0; em = -1; by = ""
	if (r in BY) { eu = EU[r]; eg = EG[r]; em = EM[r]; by = BY[r] }
	bo = (u != eu || g != eg)
	nm = m; bm = 0; note = ""
	if (t != "l") {
		if (bit(m, 16) && !(em >= 0 && bit(em, 16))) { nm -= 16; bm = 1 }
		if (bit(m, 2) && !(em >= 0 && bit(em, 2))) { nm -= 2; bm = 1 }
		if (bo && t == "f" && (bit(nm, 2048) || bit(nm, 1024))) {
			nm -= 2048 * bit(nm, 2048) + 1024 * bit(nm, 1024); note = "setuid/setgid dropped"
		}
	}
	if (!bo && !bm) next
	what = bo ? (bm ? "owner+mode" : "owner") : "mode"
	print "X", r, u, g, sprintf("%04o", m), t, eu, eg, sprintf("%04o", nm), what, who(u, g), who(eu, eg), bspec, bkind, bsrc, by, note
	next
}
$1 == "U" {
	r = rel($6)
	if (r in seen) next
	if (r in KR) { print "K", r, who($2, $3), KR[r]; next }
	print "U", r, $2, $3, sprintf("%04o", oct($4)), $5, who($2, $3)
	next
}
$1 == "N" { r = rel($2); if (!(r in nl)) print "N", r; nl[r] = 1; next }
'

# the U records with the package that has the path: dpkg's, pacman's or
# opkg's file lists
attribute() {
	db= files=
	if [ -d "$R/var/lib/dpkg/info" ]; then db=dpkg files="$R/var/lib/dpkg/info/*.list"
	elif [ -d "$R/var/lib/pacman/local" ]; then db=pacman files="$R/var/lib/pacman/local/*/files"
	elif [ -d "$R/var/lib/opkg/info" ]; then db=opkg files="$R/var/lib/opkg/info/*.list"
	fi
	# shellcheck disable=SC2086
	set -- $files
	if [ -z "$db" ] || [ ! -f "${1:-}" ]; then
		awk -F '\t' -v OFS='\t' '{ print $0, "no package database" }'
		return
	fi
	# stdin first (the U records, then a line "."), then the lists
	{ cat; echo .; } | awk -F '\t' -v OFS='\t' -v db="$db" '
		!lists { if ($0 == ".") { lists = 1; next }
			want[$2] = $0; key = substr($2, 2); k1[key] = $2; k1[key "/"] = $2; next }
		{ x = $0; sub(/\t.*/, "", x) }       # opkg: "path<tab>mode"
		db == "pacman" { x = (x in k1) ? k1[x] : "" }
		x != "" && (x in want) {
			f = FILENAME
			if (db == "pacman") { sub(/\/files$/, "", f); sub(/.*\//, "", f) }
			else { sub(/.*\//, "", f); sub(/\.list$/, "", f); sub(/:.*/, "", f) }
			if (!(x in pk)) pk[x] = f
			else if (index("," pk[x] ",", "," f ",") == 0) pk[x] = pk[x] "," f
		}
		END { for (p in want) print want[p], (p in pk) ? "package " pk[p] : "in no package" }
	' - "$@"
}

findings() {
	SPEC=$(spec)
	{
		records
		printf '%s\n' "$SPEC"
		stats
		scan
	} 2> /dev/null | awk -v R="$R" "$DECIDE"
}

# check's report of FIND; its exit status: 0 nothing found
report() {
	head=$(sed -n 's/^# tools\/devday\/mk-owned-paths.sh, \([^;]*\);.*/\1/p' "$LIST")
	echo "== fix-ownership.sh $cmd ${R:-/} (owned-paths.txt: $head, $(grep -vc '^#' "$LIST") entries)"
	printf '%s\n' "$FIND" | awk -F '\t' '$1 == "W" { print "  note: " $2 }'
	echo "-- our paths: owner or mode not as it should be (fix changes these)"
	printf '%s\n' "$FIND" | awk -F '\t' '$1 == "X" { n++
		from = (index($10, "owner") ? $11 " " : "") $5
		to = (index($10, "owner") ? $12 " " : "") $9
		src = ($14 == "tree") ? "tree " $13 ": " $15 : $15
		extra = ($16 != "" ? ", by " $16 : "") ($17 != "" ? ", " $17 : "")
		printf "  %-10s %s  %s -> %s  %s  [%s%s]\n", $10, $6, from, to, $2, src, extra }
		END { if (!n) print "  (none)" }'
	users=
	if printf '%s\n' "$FIND" | grep -q "^U$TAB"; then
		users=$(printf '%s\n' "$FIND" | grep "^U$TAB" | attribute | sort -t "$TAB" -k2,2)
	fi
	echo "-- owned by a user (uid >= 1000), not ours (fix leaves these: look at them)"
	if [ -n "$users" ]; then
		printf '%s\n' "$users" | awk -F '\t' '{ printf "  %s %s %s  %s  (%s)\n", $6, $7, $5, $2, $8 }'
	else
		echo "  (none)"
	fi
	printf '%s\n' "$FIND" | awk -F '\t' '
		$1 == "K" { printf "  known: %s %s (%s)\n", $3, $2, $4 }
		$1 == "N" { printf "  not checked: a name with a line break in %s\n", $2 }'
	printf '%s\n' "$FIND" | awk -F '\t' -v users="$(printf '%s\n' "$users" | grep -c .)" '
		$1 == "X" { x++; if (index($10, "owner")) o++; if (index($10, "mode")) m++
			k = ($14 == "tree") ? "tree " $13 : "the listed paths"; n[k]++ }
		$1 == "K" { known++ }
		END {
			for (k in n) if (k != "the listed paths") printf "  %6d below %s\n", n[k], substr(k, 6) | "sort -k3"
			close("sort -k3")
			printf "== %d of our paths to fix (owner %d, mode %d), %d by hand, %d known\n", x, o, m, users, known
			exit (x + users > 0)
		}'
}

# what is mounted at ROOT: the device, or a loop device's image file
source_of() {
	s=$(findmnt -n -o SOURCE -T "${R:-/}" 2> /dev/null | head -1)
	s=${s%%"["*}
	case $s in /dev/loop*)
		b=/sys/block/${s#/dev/}/loop/backing_file
		[ ! -r "$b" ] || s=$(cat "$b") ;;
	esac
	printf '%s\n' "$s"
}

# the undo list of FIND's X records, as they are before the fix
write_undo() {
	mkdir -p "$UNDO_DIR" && chmod 700 "$UNDO_DIR" || die "cannot write to $UNDO_DIR"
	UNDO=$UNDO_DIR/fix-ownership-undo-$(date +%Y%m%d-%H%M%S).txt
	[ ! -e "$UNDO" ] || die "$UNDO exists"
	{
		echo "# fix-ownership.sh: owners and modes before 'fix ${R:-/}', $(date)"
		echo "# back: sh fix-ownership.sh undo $UNDO [ROOT]"
		echo "# u UID GID MODE TYPE PATH (inside root)"
		printf 'root\t%s\n' "${R:-/}"
		printf 'source\t%s\n' "$(source_of)"
		printf '%s\n' "$FIND" | awk -F '\t' -v OFS='\t' '$1 == "X" { print "u", $3, $4, $5, $6, $2 }'
	} > "$UNDO" && sync || die "cannot write $UNDO"
}

# apply STREAM: lines "UID GID MODE TYPE PATH" -> chown -h, chmod (not on links)
apply() {
	while IFS="$TAB" read -r u g m t r; do
		f=$R$r
		if [ ! -e "$f" ] && [ ! -L "$f" ]; then echo "FAILED  $r: gone"; continue; fi
		ok=1
		cur=$(stat -c '%u:%g' "$f")
		if [ "$cur" != "$u:$g" ]; then chown -h "$u:$g" "$f" || ok=0; fi
		if [ "$t" != l ] && [ $ok = 1 ]; then chmod "$m" "$f" || ok=0; fi
		if [ $ok = 1 ]; then echo "changed $u:$g $m  $r"; else echo "FAILED  $r"; fi
	done | awk '{ print } /^FAILED/ { f++ } /^changed/ { c++ }
		END { printf "== %d changed, %d failed\n", c, f; exit (f > 0) }'
}

[ -f "$LIST" ] || die "no $LIST (tools/devday/mk-owned-paths.sh writes it)"
find / -maxdepth 0 -printf '' 2> /dev/null || die "needs GNU find (LuneOS: check its image from Leste)"

case $cmd in
check)
	setroot "$ROOT"
	FIND=$(findings)
	report
	;;
fix)
	setroot "$ROOT"
	must_write
	FIND=$(findings)
	report
	if ! printf '%s\n' "$FIND" | grep -q "^X$TAB"; then
		echo "nothing of ours to fix"
		exit 0
	fi
	write_undo
	echo "-- fixing (undo list: $UNDO)"
	printf '%s\n' "$FIND" | awk -F '\t' -v OFS='\t' '$1 == "X" { print $7, $8, $9, $6, $2 }' | apply
	st=$?
	echo "-- after the fix"
	FIND=$(findings)
	report
	echo "undo: sh $0 undo $UNDO${ROOT:+ $ROOT}"
	exit $st
	;;
undo)
	[ -f "$UNDO_FILE" ] || die "no $UNDO_FILE"
	[ -n "$ROOT" ] || ROOT=$(awk -F '\t' '$1 == "root" { print $2; exit }' "$UNDO_FILE")
	[ -n "$ROOT" ] || die "$UNDO_FILE names no root: give ROOT"
	setroot "$ROOT"
	must_write
	was=$(awk -F '\t' '$1 == "source" { print $2; exit }' "$UNDO_FILE")
	now=$(source_of)
	[ -z "$was" ] || [ -z "$now" ] || [ "$was" = "$now" ] ||
	    die "$UNDO_FILE is for $was, ${R:-/} has $now mounted"
	echo "== undo of $UNDO_FILE in ${R:-/} ($now)"
	awk -F '\t' -v OFS='\t' '$1 == "u" { print $2, $3, $4, $5, $6 }' "$UNDO_FILE" | apply
	;;
esac
