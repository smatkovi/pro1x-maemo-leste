#!/bin/bash
# FreeBSD packages of the parts build.sh built (desktop, apps, data): the
# metadata comes from pkgmeta.py on this PC, the packages from FreeBSD's
# own pkg create in a FreeBSD/arm64 machine (the test VM, freebsd/vm/),
# which also makes them a repository (pkg repo).
#
#   WORK=/path/to/freebsd-build VM="ssh ... root@host" \
#       bash freebsd/hildon/package.sh
#
# Result: $WORK/out/pkg/ (the packages, meta.conf, packagesite.pkg, ...),
# usable as a pkg(8) repository (freebsd/hildon/README.md) and attached to
# the GitHub release.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
WORK=${WORK:?set WORK=<build directory>}
VM=${VM:?set VM=<ssh command for a FreeBSD/arm64 machine, as root>}
H=$WORK/hildon
META=$H/pkgmeta
OUT=$WORK/out/pkg
log() { echo ">> $*"; }

log "metadata"
python3 -I "$REPO/freebsd/hildon/pkgmeta.py" "$H" "$WORK/pkgs/site/packagesite.yaml" "$META"

log "packages (pkg create in the FreeBSD machine)"
# (all of it or again: the TCG VM has once decompressed wrongly for a
# while, and a part missing there is a package missing from the repository)
want=$(find "$META" -mindepth 1 -maxdepth 1 -type d | wc -l)
for try in 1 2 3; do
	tar -C "$META" -czf - . | $VM 'rm -rf /root/pkgbuild /root/pkgout &&
		mkdir -p /root/pkgbuild /root/pkgout && tar -xzf - -C /root/pkgbuild' || true
	got=$($VM 'ls /root/pkgbuild/*/+MANIFEST | wc -l' | tr -d ' ')
	[ "$got" = "$want" ] && break
	log "only $got of $want parts arrived, again ($try)"
	[ "$try" -lt 3 ] || { echo "the parts did not arrive in the FreeBSD machine"; exit 1; }
done
$VM 'cd /root/pkgbuild && fail=0 && for d in */; do
		d=${d%/}
		pkg create -M "$d/+MANIFEST" -p "$d/plist" -r "$d/root" \
		    -o /root/pkgout > "$d.log" 2>&1 || { echo "!! $d"; cat "$d.log"; fail=1; }
	done
	pkg repo /root/pkgout > /root/pkgout.repo.log 2>&1 || { cat /root/pkgout.repo.log; exit 1; }
	ls /root/pkgout | wc -l; exit $fail'

log "back to $OUT"
rm -rf "$OUT"
mkdir -p "$OUT"
$VM 'tar -C /root/pkgout -cf - .' | tar -C "$OUT" -xf -
ls "$OUT" | head -50
log "done: $(ls "$OUT"/*.pkg | wc -l) packages in $OUT"
