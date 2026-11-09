#!/bin/sh
# try-mfsroot-geom.sh: in the FreeBSD VM (vm.sh), as root: the RAM root's
# own tools build the writable pool's devices, under the RAM root's
# conditions: its image (out/mfsroot.ufs, copied to /tmp) mounted as the
# kernel mounts it, chroot, PATH=/rescue. try-zfsrw.sh runs pro1x-pool.sh
# in the full system, with /sbin/geom and its class libraries; there
# every step worked while on the phone /rescue's geom (part and label
# only) failed the first nop create, and the RAM root went on to root.zfs
# read only without a word (until 2026-11-09). This checks what the phone
# has: the image's /sbin/geom (build.sh, static, nop and concat), its
# zpool and zdb.
#
#   sh try-mfsroot-geom.sh [/tmp/mfsroot.ufs]
#
# The VM's kernel has nop, concat and ZFS as modules (the Pro1X's has them
# built in): they are loaded first. Two md disks, a nop over each, a concat
# over both, a pool on it, its labels, export and import; all of it
# destroyed again.
set -u
IMG=${1:-/tmp/mfsroot.ufs}
R=/mnt/mfsroot-test
C="env -i PATH=/rescue chroot $R"
ok=0
fail() { echo "FAIL: $*"; ok=1; }

kldload -n geom_nop geom_concat zfs
mkdir -p $R
md=$(mdconfig -a -t vnode -o readonly -f "$IMG") || exit 1
mount -o ro /dev/$md $R || { mdconfig -d -u ${md#md}; exit 1; }
mount -t devfs devfs $R/dev
truncate -s 64m /tmp/mfsroot-test-a.img /tmp/mfsroot-test-b.img
a=$(mdconfig -a -t vnode -f /tmp/mfsroot-test-a.img)
b=$(mdconfig -a -t vnode -f /tmp/mfsroot-test-b.img)

$C /rescue/sh -n /etc/pro1x-pool.sh || fail "pro1x-pool.sh does not parse"
$C /sbin/geom nop create -Z tn0 -o 1048576 -s 33554432 /dev/$a &&
    $C /sbin/geom nop create -Z tn1 -o 0 -s 33554432 /dev/$b ||
    fail "nop create"
$C /sbin/geom concat create tc /dev/tn0.nop /dev/tn1.nop ||
    fail "concat create"
[ "$(diskinfo /dev/concat/tc 2>/dev/null | awk '{print $3}')" = 67108864 ] ||
    fail "concat size"
$C /rescue/zpool create -o cachefile=none -m none tp /dev/concat/tc &&
    $C /rescue/zpool export tp || fail "zpool create/export"
$C /rescue/zdb -l /dev/concat/tc | grep -q '^ *labels = 0 1 2 3 *$' ||
    fail "zdb: not all four labels"
$C /rescue/zpool import -N -o cachefile=none -d /dev/concat/tc tp ||
    fail "zpool import"

# pool_fixup: a boot environment as root images before 2026-11-09 had it
# (rc.conf: root_rw_mount off, the pool's part without it; fstab: the UFS
# root's "/dev/md1 / ufs ro", which mount -a applied again) gets
# root_rw_mount on for the writable pool and that fstab line commented
# out, once (two runs); the repository's rc.conf and fstab (RCCONF,
# FSTAB, copied to the VM) must stay as they are
fixup_try()
{
	local name=$1 want=$2 rc=$3 fstab=$4 be=/tmp/mfsroot-test-be n
	zfs create -o canmount=noauto -o mountpoint=/ tp/ROOT/$name
	mkdir -p $be
	mount -t zfs tp/ROOT/$name $be
	mkdir -p $be/etc
	cp "$rc" $be/etc/rc.conf
	cp "$fstab" $be/etc/fstab
	umount $be
	$C /rescue/sh -c ". /etc/pro1x-pool.sh; pool_fixup tp/ROOT/$name;
	    pool_fixup tp/ROOT/$name" || fail "pool_fixup $name"
	mount -t zfs tp/ROOT/$name $be
	n=$(grep -c "pool_fixup" $be/etc/rc.conf)
	[ "$n" = "$want" ] || fail "pool_fixup $name: $n rc.conf blocks, $want wanted"
	n=$(grep -c "pool_fixup" $be/etc/fstab)
	[ "$n" = "$want" ] || fail "pool_fixup $name: $n fstab notes, $want wanted"
	grep -Eq '^/dev/md1[[:space:]]+/[[:space:]]' $be/etc/fstab &&
	    fail "pool_fixup $name: fstab still has the UFS root line"
	if [ "$want" = 0 ]; then
		cmp -s "$rc" $be/etc/rc.conf || fail "pool_fixup $name changed rc.conf"
		cmp -s "$fstab" $be/etc/fstab || fail "pool_fixup $name changed fstab"
	fi
	# as on the phone after the reroot into the pool (the VM's kernel
	# environment for this one check)
	kenv pro1x.rootrw=1 > /dev/null
	[ "$(sh -c ". $be/etc/rc.conf; echo \$root_rw_mount")" = YES ] ||
	    fail "pool_fixup $name: root_rw_mount not YES on the pool"
	kenv -u pro1x.rootrw
	umount $be
}
zfs create -o canmount=noauto -o mountpoint=none tp/ROOT
cat > /tmp/mfsroot-test-rc.old <<'RC'
root_rw_mount="NO"
varmfs="YES"
if [ "$(/bin/kenv -q pro1x.rootrw 2> /dev/null)" = 1 ]; then
	varmfs="NO"
fi
RC
printf '# Device\tMountpoint\tFStype\tOptions\t\tDump\tPass#\n/dev/md1\t/\t\tufs\tro\t\t0\t0\n' \
    > /tmp/mfsroot-test-fstab.old
fixup_try old 1 /tmp/mfsroot-test-rc.old /tmp/mfsroot-test-fstab.old
if [ -f "${RCCONF:-/tmp/rc.conf}" ] && [ -f "${FSTAB:-/tmp/fstab}" ]; then
	fixup_try new 0 "${RCCONF:-/tmp/rc.conf}" "${FSTAB:-/tmp/fstab}"
else
	echo "(no ${RCCONF:-/tmp/rc.conf} or ${FSTAB:-/tmp/fstab}: the repository's not tried)"
fi
rm -f /tmp/mfsroot-test-rc.old /tmp/mfsroot-test-fstab.old
$C /rescue/zpool destroy tp || fail "zpool destroy"

$C /sbin/geom concat destroy tc
$C /sbin/geom nop destroy tn0.nop tn1.nop
umount $R/dev
umount $R
mdconfig -d -u ${md#md}
mdconfig -d -u ${a#md}
mdconfig -d -u ${b#md}
rm -f /tmp/mfsroot-test-a.img /tmp/mfsroot-test-b.img
[ $ok = 0 ] && echo "PASS: the RAM root's geom, zpool and zdb build and import a pool"
exit $ok
