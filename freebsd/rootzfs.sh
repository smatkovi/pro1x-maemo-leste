#!/bin/bash
# root.ufs (rootfs.sh) as a ZFS pool, root.zfs: pool "pro1x", the root in
# pro1x/ROOT/default, zstd compressed (about half the size), made with
# FreeBSD's own zpool and zfs in a FreeBSD machine (freebsd/vm/vm.sh):
# FreeBSD's makefs -t zfs, run on Linux, stops at the image's hard links.
#
#   WORK=/path/to/freebsd-build VM="ssh ... root@host" bash freebsd/rootzfs.sh
#
# On the phone root.zfs goes next to root.ufs (/host/freebsd/); the RAM
# root (mfsroot/etc/rc) prefers it when the kernel has ZFS.
set -euo pipefail
WORK=${WORK:?set WORK=<build directory>}
VM=${VM:?set VM=<ssh command for a FreeBSD machine, as root>}
OUT=$WORK/out
log() { echo ">> $*"; }

log "root.ufs to the FreeBSD machine"
$VM 'rm -rf /root/rootzfs && mkdir -p /root/rootzfs'
gzip -1 -c "$OUT/root.ufs" | $VM 'gzip -dc > /root/rootzfs/root.ufs'

log "pool from the UFS image"
$VM "SIZE=${SIZE:-2g} sh -s" <<'REMOTE'
set -e
cd /root/rootzfs
u=$(mdconfig -a -t vnode -o readonly -f root.ufs)
truncate -s "$SIZE" root.zfs
z=$(mdconfig -a -t vnode -f root.zfs)
mkdir -p ufs zfs
mount -t ufs -o ro /dev/$u ufs
zpool create -f -o ashift=12 -o cachefile=none -O compression=zstd \
    -O atime=off -O mountpoint=none -R /root/rootzfs/zfs pro1x /dev/$z
zfs create -o mountpoint=none pro1x/ROOT
zfs create -o mountpoint=/ pro1x/ROOT/default
zpool set bootfs=pro1x/ROOT/default pro1x
tar -cf - -C ufs . | tar -xpf - -C zfs
zfs list -o name,used,refer,compressratio pro1x/ROOT/default
umount ufs
zpool export pro1x
mdconfig -d -u "${z#md}"
mdconfig -d -u "${u#md}"
REMOTE

log "back"
$VM 'gzip -1 -c /root/rootzfs/root.zfs' | gzip -dc > "$OUT/root.zfs.new"
mv "$OUT/root.zfs.new" "$OUT/root.zfs"
$VM 'rm -rf /root/rootzfs'
ls -las "$OUT/root.zfs"
log "done: copy to /host/freebsd/root.zfs on the phone"
