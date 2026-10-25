#!/bin/bash
# Builds the FreeBSD root filesystem for the Pro1X on a Linux PC: the
# release's base system (base.txz) with the Pro1X configuration (rootfs/),
# as a UFS image. On the phone it lives at /host/freebsd/root.ufs; the RAM
# root (mfsroot/etc/rc) mounts userdata read only, attaches the image and
# reroots into it.
#
#   WORK=/path/to/freebsd-build bash freebsd/rootfs.sh   (after build.sh)
#
# Owners, modes and flags come from the release tarball (an mtree of it),
# since the files are unpacked as an ordinary user.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
WORK=${WORK:?set WORK=<build directory>}
SIZE=${SIZE:-2g}
OUT=$WORK/out
B=$WORK/dl/base.txz
R=$WORK/rootfs
SPEC=$WORK/rootfs.mtree
log() { echo ">> $*"; }

MAKEFS=$(find "$WORK/obj" -path '*/tmp/legacy/*' -name makefs -type f | head -1)
[ -x "$MAKEFS" ] || { echo "makefs missing: run build.sh first"; exit 1; }
[ -f "$B" ] || { echo "$B missing: run build.sh first"; exit 1; }

log "base system"
rm -rf "$R"
mkdir -p "$R"
bsdtar -xf "$B" -C "$R"
bsdtar -cf - --format=mtree @"$B" > "$SPEC"

log "Pro1X configuration (rootfs/)"
cp -a "$REPO/freebsd/rootfs/." "$R/"
# A fixed host id: rc would write one into /etc, which is read only.
uuidgen > "$R/etc/hostid"
# The overlay's files replace the release's entries; its directories are
# added where the release has none (the specification lists parents first).
files=$( (cd "$REPO/freebsd/rootfs" && find . -type f); echo ./etc/hostid)
dirs=$(cd "$REPO/freebsd/rootfs" && find . -mindepth 1 -type d | sort)
awk -v list="$files" 'BEGIN { n = split(list, a, "\n"); for (i = 1; i <= n; i++) drop[a[i]] = 1 }
	!($1 in drop)' "$SPEC" > "$SPEC.new"
for d in $dirs; do
	grep -q "^$d " "$SPEC.new" || echo "$d type=dir uid=0 gid=0 mode=755"
done >> "$SPEC.new"
while read -r p; do
	if [ -x "$R/$p" ]; then
		echo "$p type=file uid=0 gid=0 mode=755"
	else
		echo "$p type=file uid=0 gid=0 mode=644"
	fi
done <<< "$files" >> "$SPEC.new"
mv "$SPEC.new" "$SPEC"

log "image ($SIZE)"
rm -f "$OUT/root.ufs"
"$MAKEFS" -t ffs -B little -o version=2,label=pro1xroot -s "$SIZE" \
	-F "$SPEC" -N "$R/etc" "$OUT/root.ufs" "$R"
ls -la "$OUT/root.ufs"
log "done: copy to /host/freebsd/root.ufs on the phone"
