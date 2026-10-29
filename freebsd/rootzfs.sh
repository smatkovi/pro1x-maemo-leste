#!/bin/bash
# root.ufs (rootfs.sh) as a ZFS pool, root.zfs: pool "pro1x", the root in
# pro1x/ROOT/default, lz4 compressed (zstd packs better, but lz4 unpacks
# several times cheaper, which counts on a phone; see also the commit
# interval vfs.zfs.txg.timeout in pro1x.env), made with
# FreeBSD's own zpool and zfs in a FreeBSD machine (freebsd/vm/vm.sh):
# FreeBSD's makefs -t zfs, run on Linux, stops at the image's hard links.
#
#   WORK=/path/to/freebsd-build VM="ssh ... root@host" bash freebsd/rootzfs.sh
#
# On the phone root.zfs goes to /host/freebsd/ (root.ufs does not: the root
# is ZFS only); the RAM root (mfsroot/etc/rc) imports it, or the writable
# pool.zfs that pro1x-freebsd-pool makes from it.
set -euo pipefail
WORK=${WORK:?set WORK=<build directory>}
VM=${VM:?set VM=<ssh command for a FreeBSD machine, as root>}
OUT=$WORK/out
log() { echo ">> $*"; }

log "root.ufs to the FreeBSD machine"
$VM 'rm -rf /root/rootzfs && mkdir -p /root/rootzfs'
# sparse in the VM: the image's free space takes no room there; dd skips
# trailing zeros without writing them, so the file gets its size back
# (the link to the VM has broken a transfer now and then: checked, up to
# three tries)
size=$(stat -c %s "$OUT/root.ufs")
sum=$(sha256sum "$OUT/root.ufs" | cut -d' ' -f1)
for try in 1 2 3; do
	gzip -1 -c "$OUT/root.ufs" | $VM "gzip -dc | dd of=/root/rootzfs/root.ufs bs=1m conv=sparse status=none && truncate -s $size /root/rootzfs/root.ufs" &&
	    [ "$($VM 'sha256 -q /root/rootzfs/root.ufs')" = "$sum" ] && break
	log "transfer $try failed"
	[ "$try" = 3 ] && exit 1
done

log "pool from the UFS image"
# lz4 packs less than zstd did (2 GB then): 3 GB of pool
$VM "SIZE=${SIZE:-3g} sh -s" <<'REMOTE'
set -e
set -o pipefail
cd /root/rootzfs
u=$(mdconfig -a -t vnode -o readonly -f root.ufs)
truncate -s "$SIZE" root.zfs
z=$(mdconfig -a -t vnode -f root.zfs)
mkdir -p ufs zfs
mount -t ufs -o ro /dev/$u ufs
zpool create -f -o ashift=12 -o cachefile=none -o autoexpand=on -O compression=lz4 \
    -O atime=off -O mountpoint=none -R /root/rootzfs/zfs pro1x /dev/$z
zfs create -o mountpoint=none pro1x/ROOT
zfs create -o mountpoint=/ pro1x/ROOT/default
# root's home outside the boot environments: a rollback keeps it
zfs create -o mountpoint=/root pro1x/root
zpool set bootfs=pro1x/ROOT/default pro1x
tar -cf - -C ufs . | tar -xpf - -C zfs
zfs list -o name,used,refer,compressratio pro1x/ROOT/default
# an empty pool means the copy failed: never hand that back
used=$(zfs get -Hpo value used pro1x/ROOT/default)
[ "$used" -gt 536870912 ] || { echo "only $used bytes in the pool"; exit 1; }
umount ufs
zpool export pro1x
mdconfig -d -u "${z#md}"
mdconfig -d -u "${u#md}"
REMOTE

log "back"
# checked and up to three tries, as on the way in; the old image goes
# first, the disk may not hold two
sum=$($VM 'sha256 -q /root/rootzfs/root.zfs')
rm -f "$OUT/root.zfs" "$OUT/root.zfs.new"
for try in 1 2 3; do
	$VM 'gzip -1 -c /root/rootzfs/root.zfs' | gzip -dc > "$OUT/root.zfs.new" &&
	    [ "$(sha256sum "$OUT/root.zfs.new" | cut -d' ' -f1)" = "$sum" ] && break
	log "transfer back $try failed"
	rm -f "$OUT/root.zfs.new"
	[ "$try" = 3 ] && exit 1
done
mv "$OUT/root.zfs.new" "$OUT/root.zfs"
$VM 'rm -rf /root/rootzfs'
ls -las "$OUT/root.zfs"
log "done: copy to /host/freebsd/root.zfs on the phone"
