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

# pool_fixup: a boot environment with rc.conf as root images before
# 2026-11-09 had it (root_rw_mount off, the pool's part without it) gets
# root_rw_mount on for the writable pool, once; RCCONF (the repository's
# rc.conf, copied to the VM) must stay as it is
fixup_try()
{
	local name=$1 want=$2 n
	zfs create -o canmount=noauto -o mountpoint=/ tp/ROOT/$name
	mkdir -p /tmp/mfsroot-test-be
	mount -t zfs tp/ROOT/$name /tmp/mfsroot-test-be
	mkdir -p /tmp/mfsroot-test-be/etc
	cat > /tmp/mfsroot-test-be/etc/rc.conf
	cp /tmp/mfsroot-test-be/etc/rc.conf /tmp/mfsroot-test-rc.before
	umount /tmp/mfsroot-test-be
	$C /rescue/sh -c ". /etc/pro1x-pool.sh; pool_fixup tp/ROOT/$name;
	    pool_fixup tp/ROOT/$name" || fail "pool_fixup $name"
	mount -t zfs tp/ROOT/$name /tmp/mfsroot-test-be
	n=$(grep -c "pool_fixup" /tmp/mfsroot-test-be/etc/rc.conf)
	[ "$n" = "$want" ] || fail "pool_fixup $name: $n blocks, $want wanted"
	[ "$want" = 1 ] || cmp -s /tmp/mfsroot-test-rc.before \
	    /tmp/mfsroot-test-be/etc/rc.conf || fail "pool_fixup $name changed it"
	# as on the phone after the reroot into the pool (the VM's kernel
	# environment for this one check)
	kenv pro1x.rootrw=1 > /dev/null
	[ "$(sh -c '. /tmp/mfsroot-test-be/etc/rc.conf; echo $root_rw_mount')" = YES ] ||
	    fail "pool_fixup $name: root_rw_mount not YES on the pool"
	kenv -u pro1x.rootrw
	umount /tmp/mfsroot-test-be
}
zfs create -o canmount=noauto -o mountpoint=none tp/ROOT
fixup_try old 1 <<'RC'
root_rw_mount="NO"
varmfs="YES"
if [ "$(/bin/kenv -q pro1x.rootrw 2> /dev/null)" = 1 ]; then
	varmfs="NO"
fi
RC
if [ -f "${RCCONF:-/tmp/rc.conf}" ]; then
	fixup_try new 0 < "${RCCONF:-/tmp/rc.conf}"
else
	echo "(no ${RCCONF:-/tmp/rc.conf}: the repository's rc.conf not tried)"
fi
rm -f /tmp/mfsroot-test-rc.before
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
