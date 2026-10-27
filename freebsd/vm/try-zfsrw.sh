#!/bin/sh
# try-zfsrw.sh: in the FreeBSD VM (vm.sh), as root: the writable pool
# through its map, as the Pro1X's RAM root does (mfsroot/etc/pro1x-pool.sh
# and userdata.img.zst from vm/zfsrw-userdata.sh in /tmp; the kernel's
# geom_nop with patches/0029, kldload'ed). Wrong maps must be refused;
# the pool imported read-write, written, scrubbed, imported again; a boot
# environment; then /tmp/userdata-after.img.zst to check on the PC
# (e2fsck, before.sha).
set -u
. /tmp/pro1x-pool.sh
cd /tmp && zstd -q -d -f userdata.img.zst -o userdata.img
md=$(mdconfig -a -t vnode -f /tmp/userdata.img); sleep 2
ls -la /dev/gpt/userdata
mkdir -p /mnt/h /mnt/z
mount -t ext2fs -o ro /dev/gpt/userdata /mnt/h && echo "userdata mounted ro"
echo "=== 1: wrong map (inode)"
head -1 /mnt/h/freebsd/pool.map | sed 's/^pro1x-pool-map 1 [0-9]*/pro1x-pool-map 1 999/' > /tmp/bad1.map
sed 1d /mnt/h/freebsd/pool.map >> /tmp/bad1.map
pool_rw /mnt/h /dev/gpt/userdata /tmp/bad1.map && echo "!!! IMPORTED (wrong)" || echo "refused: ok"
echo "=== 2: wrong map (ranges shifted by 4 KiB)"
head -1 /mnt/h/freebsd/pool.map > /tmp/bad2.map
sed 1d /mnt/h/freebsd/pool.map | while read o l; do echo "$((o + 4096)) $l"; done >> /tmp/bad2.map
pool_rw /mnt/h /dev/gpt/userdata /tmp/bad2.map && echo "!!! IMPORTED (wrong)" || echo "refused: ok"
geom nop list 2>/dev/null | grep -c "Name: pro1xpool" ; ls /dev/concat 2>/dev/null
echo "=== 3: the right map"
pool_rw /mnt/h /dev/gpt/userdata && echo "imported read-write"
zpool list -v pro1x; zpool get -H bootfs,autoexpand,readonly pro1x
echo "bootfs chosen: $(pool_bootfs < /dev/null 2>/dev/null)"
zpool export pro1x
echo "=== 4: writes through it"
zpool import -R /mnt/z -o cachefile=none -d /dev/concat/pro1xpool pro1x
cat /mnt/z/etc/motd; [ "$(sha256 -q /mnt/z/var/db/blob)" = "$(cat /mnt/z/etc/blob.sha)" ] && echo "old data ok"
dd if=/dev/random of=/mnt/z/var/db/blob2 bs=1m count=300 status=none
sha256 -q /mnt/z/var/db/blob2 > /mnt/z/etc/blob2.sha; echo "300 MiB written"
bectl -r pro1x/ROOT create -e default testbe && bectl -r pro1x/ROOT list
zpool scrub -w pro1x; zpool status pro1x | sed -n '/scan:/p;/errors:/p'
zpool export pro1x; pool_undo
mount -t ext2fs -o ro /dev/gpt/userdata /mnt/h
echo "=== 5: again, the data still there"
pool_rw /mnt/h /dev/gpt/userdata && zpool export pro1x
zpool import -R /mnt/z -o cachefile=none -d /dev/concat/pro1xpool pro1x
[ "$(sha256 -q /mnt/z/var/db/blob2)" = "$(cat /mnt/z/etc/blob2.sha)" ] && echo "new data ok after re-import"
zfs list -o name,used -r pro1x
zpool list -o name,size,alloc pro1x
zpool export pro1x; pool_undo
mdconfig -d -u ${md#md}
zstd -q -f -T0 userdata.img -o userdata-after.img.zst && ls -la userdata-after.img.zst
