#!/bin/bash
# On the PC, before the device day: the packages the phone gets, from this
# repository's GitHub pre-releases, into $WORK/devday/ (so the day does
# not depend on the network), each checked against its release's SHA256SUMS.
#
#   bash tools/devday/fetch.sh [list]
#       list: what would be fetched (the newest of each package over all
#       leste-* pre-releases), nothing downloaded
#
# $WORK/devday/leste-debs/<group>/: the newest version of every package of
# all leste-* pre-releases (leste-hildon-*, leste-harmattan-*,
# leste-mobile-data-*, leste-waydroid-*), sorted into the groups
# tools/devday/leste-install.sh installs in this order:
#   1-gtk        GTK 2 (libgtk2.0-0, -common, -bin, libgail*, engines, gir)
#   2-libs       libhildon1, liblocation0, calendar-backend, libngf
#   3-system     mce, hildon-desktop, hildon-home, hildon-status-menu
#   4-apps       pro1x-events, pro1x-volume-osd, pro1x-look, conversations,
#                location-ui, ngfd
#   5-harmattan  the N9's user interface (harmattan-*, qt-components)
#   ofono        oFono for mobile data (scripts/49-mobile-data.sh installs it)
#   waydroid     pro1x-waydroid (docs/waydroid.md)
# Left out: -dev, -doc and -dbgsym packages (not needed on the phone).
# $WORK/devday/arch-pkgs/: the newest arch-hildon-* release's packages.
# $WORK/devday/whisperfish/: Whisperfish for Leste (WF_TAG, default
# leste-0.6.0-dev.1 of smatkovi/whisperfish-leste).
# $WORK/devday/FETCHED: what came from where, with the sha256.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
GH_REPO=${GH_REPO:-smatkovi/pro1x-maemo-leste}
DEST=$WORK/devday
mode=${1:-fetch}
command -v gh > /dev/null || die "gh missing"

tmp=$(mktemp -d /tmp/pro1x-devday-fetch.XXXXXX)
mkdir "$tmp/dl"
trap 'rm -f "$tmp"/dl/*; rmdir "$tmp/dl"; rm -f "$tmp"/*; rmdir "$tmp"' EXIT
gh release list -R "$GH_REPO" -L 200 --json tagName -q '.[].tagName' > "$tmp/tags"
for t in $(grep -E '^leste-' "$tmp/tags"); do
	gh release view -R "$GH_REPO" "$t" --json assets -q '.assets[].name' | sed "s/^/$t /"
done > "$tmp/assets"

# the newest version of each package (Debian version order), its group
python3 -I - "$tmp/assets" > "$tmp/plan" <<'EOF'
import re, sys

def order(c):
    if c.isdigit(): return 0
    if c.isalpha(): return ord(c)
    if c == '~': return -1
    return ord(c) + 256 if c else 0

def cmp_part(a, b):
    while a or b:
        na = re.match(r'^[^0-9]*', a).group(); nb = re.match(r'^[^0-9]*', b).group()
        a, b = a[len(na):], b[len(nb):]
        for i in range(max(len(na), len(nb))):
            x = order(na[i]) if i < len(na) else 0
            y = order(nb[i]) if i < len(nb) else 0
            if x != y: return (x > y) - (x < y)
        da = re.match(r'^[0-9]*', a).group(); db = re.match(r'^[0-9]*', b).group()
        a, b = a[len(da):], b[len(db):]
        x, y = int(da or 0), int(db or 0)
        if x != y: return (x > y) - (x < y)
    return 0

def split(v):
    e, _, v = v.rpartition(':') if ':' in v else ('0', '', v)
    u, _, r = v.rpartition('-') if '-' in v else (v, '', '0')
    return int(e or 0), u, r

def vcmp(a, b):
    ea, ua, ra = split(a); eb, ub, rb = split(b)
    if ea != eb: return (ea > eb) - (ea < eb)
    return cmp_part(ua, ub) or cmp_part(ra, rb)

def group(p):
    if p.startswith(('libgtk2.0', 'libgail', 'gtk2-engines', 'gir1.2-gtk-2.0')): return '1-gtk'
    if p in ('libhildon1', 'liblocation0', 'calendar-backend', 'libngf'): return '2-libs'
    if p in ('mce', 'hildon-desktop', 'hildon-home', 'hildon-status-menu'): return '3-system'
    if p.startswith('harmattan-') or p == 'qt-components': return '5-harmattan'
    if p.startswith('ofono'): return 'ofono'
    if p == 'pro1x-waydroid': return 'waydroid'
    return '4-apps'

best = {}
for line in open(sys.argv[1]):
    tag, name = line.split()
    m = re.match(r'^([^_]+)_([^_]+)_([^_.]+)\.deb$', name)
    if not m: continue
    pkg, ver = m.group(1), m.group(2)
    if pkg.endswith(('-dev', '-doc', '-dbgsym')): continue
    # file names carry the version without the epoch: ofono's is 1:
    if pkg in best and vcmp(ver, best[pkg][1]) <= 0: continue
    best[pkg] = (tag, ver, name)
for pkg in sorted(best, key=lambda p: (group(p), p)):
    tag, ver, name = best[pkg]
    print(group(pkg), tag, name)
EOF
arch_tag=$(grep -E '^arch-hildon-' "$tmp/tags" | sort -V | tail -1 || true)

if [ "$mode" = list ]; then
	column -t "$tmp/plan"
	echo "arch packages: ${arch_tag:-none}"
	exit 0
fi
need_space "$WORK" 300
mkdir -p "$DEST/leste-debs"
: > "$DEST/FETCHED.new"
for tag in $(cut -d' ' -f2 "$tmp/plan" | sort -u); do
	pats=()
	for n in $(awk -v t="$tag" '$2 == t { print $3 }' "$tmp/plan"); do pats+=(-p "$n"); done
	gh release download -R "$GH_REPO" "$tag" -D "$tmp/dl" "${pats[@]}"
	# the release's checksums, where it has them (leste-harmattan-* has none)
	if grep -qx "$tag SHA256SUMS" "$tmp/assets"; then
		gh release download -R "$GH_REPO" "$tag" -D "$tmp/dl" -p SHA256SUMS
		( cd "$tmp/dl" && sha256sum -c --ignore-missing SHA256SUMS > /dev/null ) ||
		    die "$tag: SHA256SUMS does not match"
		chk=checked
	else
		chk="no SHA256SUMS in the release"
	fi
	awk -v t="$tag" '$2 == t { print $1, $3 }' "$tmp/plan" | while read -r g n; do
		mkdir -p "$DEST/leste-debs/$g"
		mv "$tmp/dl/$n" "$DEST/leste-debs/$g/$n"
		echo "$(sha "$DEST/leste-debs/$g/$n")  leste-debs/$g/$n  ($tag, $chk)" >> "$DEST/FETCHED.new"
	done
	rm -f "$tmp"/dl/*
done
if [ -n "$arch_tag" ]; then
	mkdir -p "$DEST/arch-pkgs"
	gh release download -R "$GH_REPO" "$arch_tag" -D "$DEST/arch-pkgs" --clobber
	if [ -f "$DEST/arch-pkgs/SHA256SUMS" ]; then
		( cd "$DEST/arch-pkgs" && sha256sum -c --ignore-missing SHA256SUMS > /dev/null ) ||
		    die "$arch_tag: SHA256SUMS does not match"
	fi
	echo "arch-pkgs from $arch_tag" >> "$DEST/FETCHED.new"
fi
# Whisperfish for Leste (github smatkovi/whisperfish-leste), its pre-release
WF_REPO=${WF_REPO:-smatkovi/whisperfish-leste}
WF_TAG=${WF_TAG:-leste-0.6.0-dev.1}
mkdir -p "$DEST/whisperfish"
gh release download -R "$WF_REPO" "$WF_TAG" -D "$DEST/whisperfish" --clobber
( cd "$DEST/whisperfish" && for c in *.sha256; do
	[ -f "$c" ] || continue
	[ "$(sha256sum < "${c%.sha256}" | cut -c1-64)" = "$(cut -c1-64 "$c")" ] ||
	    { echo "error: $c does not match" >&2; exit 1; }
done )
for f in "$DEST"/whisperfish/*.deb; do
	echo "$(sha "$f")  whisperfish/${f##*/}  ($WF_TAG)" >> "$DEST/FETCHED.new"
done
mv "$DEST/FETCHED.new" "$DEST/FETCHED"
# packages of an older fetch that a newer version replaced
for g in "$DEST"/leste-debs/*/; do
	for f in "$g"*.deb; do
		[ -f "$f" ] || continue
		grep -q " leste-debs/${g#"$DEST"/leste-debs/}${f##*/} " "$DEST/FETCHED" ||
		    { log "older, removed: $f"; rm -f "$f"; }
	done
done
log "fetched into $DEST:"
cat "$DEST/FETCHED"
