#!/bin/bash
# Builds Leste packages for the phone on the PC (build host, needs sudo),
# in leste-chroot.sh's chroot of the official arm64 image (qemu):
# 49-hidpi.sh's rebuilt Hildon parts (ONLY=, its OUT) and mkdeb-tools.sh's
# own tools, from the repository's HEAD (commit first). The packages go to
# $OUT, dpkg-deb -I/-c and lintian of each to $OUT/checks. The chroot is
# unmounted at the end and kept (what is installed in it saves time the
# next time); leste-chroot.sh delete removes it.
#
#   bash scripts/41-leste-debs-chroot.sh
#   ONLY="gtk libhildon" bash scripts/41-leste-debs-chroot.sh   (ONLY= all)
#   HIDPI=0 ...  only the tools;  TOOLS=0 ...  only 49-hidpi.sh's parts
#
# BASE_IMG, CHROOT, MIN_FREE_GB: leste-chroot.sh's. The build is stopped
# when a disk has less than MIN_FREE_GB free (checked every 30 s).
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
WORK=${WORK:-$(cd "$REPO/.." && pwd)}
OUT=${OUT:-$WORK/out/hidpi-debs}
ONLY=${ONLY-libhildon hildon-status-menu hildon-desktop}
HIDPI=${HIDPI:-1}
TOOLS=${TOOLS:-1}
export REPO WORK
C=("bash" "$REPO/scripts/leste-chroot.sh")
log() { echo ">> $(date +%T) $*"; }

WATCH=
cleanup() {
  set +e
  [ -z "$WATCH" ] || kill "$WATCH" 2> /dev/null
  "${C[@]}" umount
}
trap cleanup EXIT
trap 'exit 130' INT TERM

"${C[@]}" mount
"${C[@]}" repo
"${C[@]}" run rm -rf /root/out
"${C[@]}" run mkdir -p /root/out

# while it builds: stopped when a disk gets too full
(
  while sleep 30; do
    "${C[@]}" space 2> /dev/null && continue
    "${C[@]}" space || true
    echo "!! stopping the build: too little disk space" >&2
    "${C[@]}" kill
    exit 0
  done
) &
WATCH=$!

if [ "$HIDPI" = 1 ]; then
  log "49-hidpi.sh ONLY=\"$ONLY\""
  "${C[@]}" run REPO=/root/pro1x-repo SRC=/root/src OUT=/root/out ONLY="$ONLY" \
    bash /root/pro1x-repo/scripts/49-hidpi.sh
fi
"${C[@]}" space
if [ "$TOOLS" = 1 ]; then
  log "mkdeb-tools.sh"
  "${C[@]}" run REPO=/root/pro1x-repo SRC=/root/src OUT=/root/out \
    COMMIT="$(git -C "$REPO" rev-parse --short HEAD)" \
    SOURCE_DATE_EPOCH="$(git -C "$REPO" log -1 --format=%ct)" \
    bash /root/pro1x-repo/scripts/mkdeb-tools.sh
fi

log "checks"
"${C[@]}" run bash -c '
  if ! command -v lintian > /dev/null; then
    apt-get install -y -q --no-install-recommends lintian > /root/lintian-install.log 2>&1 ||
      echo "lintian not installed (/root/lintian-install.log)"
  fi
  cd /root/out
  for f in *.deb; do
    { dpkg-deb -I "$f"; dpkg-deb -c "$f"; } > "check-${f%.deb}.txt"
    ! command -v lintian > /dev/null ||
      { echo "== lintian"; lintian --no-tag-display-limit "$f" 2>&1; } >> "check-${f%.deb}.txt" || true
  done'

mkdir -p "$OUT/checks"
t=$(mktemp -d)
"${C[@]}" run tar -C /root/out -c . | tar -x -C "$t"
mv "$t"/*.deb "$OUT/"
mv "$t"/check-*.txt "$OUT/checks/"
rm -rf "$t"
log "done"
