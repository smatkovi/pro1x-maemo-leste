#!/bin/bash
# zfsrw-userdata.sh DIR: a test userdata for vm/try-zfsrw.sh, on the PC
# (sudo): DIR/userdata.img, a GPT disk whose partition "userdata" is ext4
# with the Pro1X's userdata features, its free space fragmented on purpose
# (FRAG=0: not), freebsd/root.zfs (DIR/root.zfs, a pool "pro1x", e.g. a
# small one made in the VM) and pool.zfs + pool.map made by
# tools/pro1x-freebsd-pool; DIR/before.sha: checksums of every other file,
# to compare after FreeBSD wrote the pool; DIR/userdata.img.zst to copy.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
T=$REPO/tools/pro1x-freebsd-pool/pro1x-freebsd-pool
Z=$(cd "${1:?DIR}" && pwd)
cd "$Z"
rm -f userdata.img userdata.img.zst
truncate -s 3G userdata.img
sgdisk -o -n1:2048:0 -c1:userdata userdata.img > /dev/null
FIRST=2048 LAST=6291422
# the partition only: mkfs over the whole file would hit the backup GPT
L=$(sudo -n losetup --show -f -o $((FIRST*512)) --sizelimit $(((LAST-FIRST+1)*512)) userdata.img)
trap 'mountpoint -q mnt && sudo -n umount mnt; sudo -n losetup -d $L' EXIT
sudo -n mkfs.ext4 -q -F -b 4096 -I 256 -O ^metadata_csum,^metadata_csum_seed,^orphan_file,uninit_bg,huge_file,64bit,dir_nlink,extra_isize -L userdata $L
mkdir -p mnt
sudo -n mount $L mnt
sudo -n mkdir -p mnt/freebsd mnt/fill
sudo -n cp root.zfs mnt/freebsd/root.zfs
for i in $(seq 1 ${FILL:-600}); do sudo -n dd if=/dev/urandom of=mnt/fill/f$i bs=1M count=1 status=none; done
[ "${FRAG:-1}" = 1 ] && for i in $(seq 1 2 ${FILL:-600}); do sudo -n rm mnt/fill/f$i; done
sudo -n sync
sudo -n python3 $T create --host $Z/mnt --size 1G
cat mnt/freebsd/pool.map
(cd mnt && sudo -n sha256sum freebsd/root.zfs fill/* > $Z/before.sha)
sudo -n umount mnt
sudo -n e2fsck -fn $L | tail -1
zstd -q -f -T0 userdata.img -o userdata.img.zst
