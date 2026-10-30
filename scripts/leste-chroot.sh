#!/bin/bash
# A chroot of the official Maemo Leste arm64 image on the PC (build host,
# needs sudo), run by qemu-aarch64-static: the image unpacked sparse, its
# root file system mounted read-only under an overlay whose upper and work
# layers ($CHROOT) keep what is installed and built in it between uses.
# 41-leste-debs-chroot.sh builds the phone's packages in it.
#
#   scripts/leste-chroot.sh mount        unpack (the first time), mount,
#                                        the static qemu for aarch64
#   scripts/leste-chroot.sh repo         the repository's HEAD (git archive)
#                                        to /root/pro1x-repo in it
#   scripts/leste-chroot.sh run CMD...   CMD as root in it
#   scripts/leste-chroot.sh shell        a shell in it
#   scripts/leste-chroot.sh status       what is mounted, the disks' space
#   scripts/leste-chroot.sh space        fails below MIN_FREE_GB
#   scripts/leste-chroot.sh kill         its processes stopped
#   scripts/leste-chroot.sh umount       its processes stopped, unmounted,
#                                        binfmt as before the mount
#   scripts/leste-chroot.sh delete       (unmounted) its layers and the
#                                        unpacked image removed
#
# BASE_IMG may be a symlink to another disk (where there is more room).
# MIN_FREE_GB (GiB, default 10) must stay free on the disks of BASE_IMG
# and CHROOT; mount checks it.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
WORK=${WORK:-$(cd "$REPO/.." && pwd)}
BASE_XZ=${BASE_XZ:-$WORK/dl/maemo-leste-5.0-arm64-arm64-generic-20260426.img.xz}
BASE_IMG=${BASE_IMG:-$WORK/build/leste-base.img}
# the root partition's first sector in the official image (40-make-rootfs.sh)
ROOT_START=${ROOT_START:-264192}
CHROOT=${CHROOT:-$WORK/build/leste-chroot}
MIN_FREE_GB=${MIN_FREE_GB:-10}
L=$CHROOT/lower R=$CHROOT/root
# what mount changed in binfmt_misc, for umount to put back
STATE=$CHROOT/binfmt-changed
B=/proc/sys/fs/binfmt_misc
log() { echo ">> $*"; }

# GiB free on the disk of each path (or of its parent while it is missing)
free_ok() {
  local need=$1 p a
  shift
  for p; do
    p=$(readlink -m "$p")
    while [ ! -e "$p" ]; do p=$(dirname "$p"); done
    a=$(df -B1M --output=avail "$p" | tail -1)
    if [ "$a" -lt $((need * 1024)) ]; then
      echo "!! $((a / 1024)) GiB free on the disk of $p, $need wanted" >&2
      return 1
    fi
  done
}

# The binfmt registered on Arch points at the dynamically linked
# qemu-aarch64, whose libraries are missing in the chroot: the static one
# while it is mounted, as 40-make-rootfs.sh does
AARCH64_MAGIC='\x7f\x45\x4c\x46\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00'
AARCH64_MASK='\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff'
binfmt_use_static() {
  local changed=
  [ ! -e "$STATE" ] || return 0
  [ -x /usr/bin/qemu-aarch64-static ] || { echo "qemu-aarch64-static missing"; exit 1; }
  if [ -e $B/qemu-aarch64 ] &&
     grep -q 'interpreter /usr/bin/qemu-aarch64-static' $B/qemu-aarch64; then
    return 0
  fi
  if [ -e $B/qemu-aarch64 ] && grep -qx enabled $B/qemu-aarch64; then
    sudo -n sh -c "echo 0 > $B/qemu-aarch64"
    changed="qemu-aarch64"
  fi
  if [ ! -e $B/pro1x-aarch64 ]; then
    sudo -n sh -c "echo ':pro1x-aarch64:M::$AARCH64_MAGIC:$AARCH64_MASK:/usr/bin/qemu-aarch64-static:PF' > $B/register"
    changed="$changed pro1x-aarch64"
  fi
  echo "$changed" > "$STATE"
}
binfmt_restore() {
  [ -e "$STATE" ] || return 0
  case " $(cat "$STATE") " in *" pro1x-aarch64 "*)
    [ ! -e $B/pro1x-aarch64 ] || sudo -n sh -c "echo -1 > $B/pro1x-aarch64" ;;
  esac
  case " $(cat "$STATE") " in *" qemu-aarch64 "*)
    [ ! -e $B/qemu-aarch64 ] || sudo -n sh -c "echo 1 > $B/qemu-aarch64" ;;
  esac
  rm -f "$STATE"
}

in_target() {
  mountpoint -q "$R/proc" || { echo "not mounted: $0 mount"; exit 1; }
  sudo -n chroot "$R" /usr/bin/env -i \
    PATH=/usr/local/chroot-bin:/usr/sbin:/usr/bin:/sbin:/bin HOME=/root LANG=C.UTF-8 \
    TERM="${TERM:-dumb}" DEBIAN_FRONTEND=noninteractive "$@"
}

kill_chroot() {
  sudo -n sh -c 'for p in /proc/[0-9]*; do
    [ "$(readlink "$p/root" 2> /dev/null)" = "$1" ] && kill -TERM "${p#/proc/}" 2> /dev/null
  done; true' sh "$R"
}

do_mount() {
  if [ ! -e "$BASE_IMG" ]; then
    log "checking $BASE_XZ"
    (cd "$(dirname "$BASE_XZ")" && sha256sum -c --quiet "${BASE_XZ##*/}.sha256")
    free_ok $((MIN_FREE_GB + 3)) "$BASE_IMG"
    # (a symlink's target)
    local img
    img=$(readlink -m "$BASE_IMG")
    log "unpacking to $img (sparse)"
    mkdir -p "$(dirname "$img")"
    # xz makes holes of the zeros where it writes to a file
    xz -dc "$BASE_XZ" > "$img.part"
    mv "$img.part" "$img"
    du -h "$img" | cut -f1 | sed 's/^/   used: /'
  fi
  free_ok "$MIN_FREE_GB" "$BASE_IMG" "$CHROOT"
  mkdir -p "$L" "$R" "$CHROOT/upper" "$CHROOT/work"
  if ! mountpoint -q "$L"; then
    sudo -n mount -o ro,loop,offset=$((ROOT_START * 512)) "$BASE_IMG" "$L" 2> /dev/null ||
      sudo -n mount -o ro,noload,loop,offset=$((ROOT_START * 512)) "$BASE_IMG" "$L"
  fi
  [ -f "$L/etc/debian_version" ] || { echo "no root file system at sector $ROOT_START"; exit 1; }
  mountpoint -q "$R" || sudo -n mount -t overlay overlay \
    -o "lowerdir=$L,upperdir=$CHROOT/upper,workdir=$CHROOT/work" "$R"
  mountpoint -q "$R/proc" || sudo -n mount -t proc proc "$R/proc"
  mountpoint -q "$R/sys" || sudo -n mount -t sysfs sys "$R/sys"
  mountpoint -q "$R/dev" || sudo -n mount --bind /dev "$R/dev"
  # a slave of the PC's /dev: a bind of a shared mount is its peer, and
  # unmounting the chroot's dev/pts then unmounted the PC's /dev/pts too
  # (2026-10-30: no new terminals on the PC)
  sudo -n mount --make-rslave "$R/dev"
  mountpoint -q "$R/dev/pts" || sudo -n mount --bind /dev/pts "$R/dev/pts"
  sudo -n rm -f "$R/etc/resolv.conf"
  sudo -n cp -L /etc/resolv.conf "$R/etc/resolv.conf"
  # leste-extras' InRelease breaks apt under qemu (getline: ENOMEM), not
  # needed here
  local f
  for f in "$R"/etc/apt/sources.list.d/*.list; do
    [ -f "$f" ] && sudo -n sed -i '/git\.maemo\.org/s/^deb /# deb /' "$f"
  done
  # no services started, no udev triggered by package scripts; downloaded
  # packages not kept (the upper layer stays small)
  printf '#!/bin/sh\nexit 101\n' | sudo -n tee "$R/usr/sbin/policy-rc.d" > /dev/null
  sudo -n chmod 755 "$R/usr/sbin/policy-rc.d"
  sudo -n mkdir -p "$R/usr/local/chroot-bin"
  printf '#!/bin/sh\nexit 0\n' | sudo -n tee "$R/usr/local/chroot-bin/udevadm" > /dev/null
  sudo -n chmod 755 "$R/usr/local/chroot-bin/udevadm"
  echo 'DPkg::Post-Invoke { "rm -f /var/cache/apt/archives/*.deb /var/cache/apt/archives/partial/*.deb || true"; };' |
    sudo -n tee "$R/etc/apt/apt.conf.d/00pro1x-chroot-clean" > /dev/null
  binfmt_use_static
  in_target /bin/true || { echo "the chroot does not run (binfmt)"; exit 1; }
  log "mounted: $R"
}

do_umount() {
  local m
  kill_chroot
  sleep 1
  for m in "$R/dev/pts" "$R/dev" "$R/proc" "$R/sys" "$R" "$L"; do
    if mountpoint -q "$m"; then sudo -n umount "$m" || sudo -n umount -l "$m"; fi
  done
  binfmt_restore
  log "unmounted, binfmt as before"
}

cmd=${1:-status}
[ $# = 0 ] || shift
case $cmd in
mount) do_mount ;;
umount) do_umount ;;
repo)
  log "repository at $(git -C "$REPO" rev-parse --short HEAD) to /root/pro1x-repo"
  [ -z "$(git -C "$REPO" status --porcelain -- scripts tools apps patches rootfs-overlay meego)" ] ||
    log "note: uncommitted changes are not in it (git archive HEAD)"
  in_target rm -rf /root/pro1x-repo
  in_target mkdir -p /root/pro1x-repo
  git -C "$REPO" archive HEAD | sudo -n tar -x -C "$R/root/pro1x-repo"
  ;;
run) in_target "$@" ;;
shell) in_target /bin/bash -l ;;
kill) kill_chroot ;;
space) free_ok "$MIN_FREE_GB" "$BASE_IMG" "$CHROOT" ;;
status)
  for m in "$L" "$R" "$R/proc"; do
    if mountpoint -q "$m"; then echo "mounted:     $m"; else echo "not mounted: $m"; fi
  done
  [ ! -e "$STATE" ] || echo "binfmt changed: $(cat "$STATE")"
  ls -l "$BASE_IMG" 2> /dev/null || echo "no $BASE_IMG"
  [ ! -e "$BASE_IMG" ] || du -h "$(readlink -f "$BASE_IMG")" | cut -f1 | sed 's/^/  image used: /'
  [ ! -d "$CHROOT/upper" ] || sudo -n du -sh "$CHROOT/upper" | cut -f1 | sed 's/^/  upper: /'
  df -h "$(dirname "$(readlink -f "$BASE_IMG")")" "$WORK" | sed 's/^/  /'
  ;;
delete)
  mountpoint -q "$R" || mountpoint -q "$L" && { echo "mounted: $0 umount first"; exit 1; }
  sudo -n rm -rf "$CHROOT/upper" "$CHROOT/work"
  rmdir "$L" "$R" "$CHROOT" 2> /dev/null || true
  [ ! -e "$BASE_IMG" ] || rm -f "$(readlink -f "$BASE_IMG")"
  rm -f "$BASE_IMG"
  log "removed"
  ;;
*) sed -n '2,/^set /p' "$0" | sed '$d'; exit 1 ;;
esac
