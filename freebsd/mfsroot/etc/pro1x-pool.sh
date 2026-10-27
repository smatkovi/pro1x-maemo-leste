# FreeBSD on the Pro1X: the writable ZFS pool, sourced by /etc/rc (and by
# freebsd/vm/try-zfsrw.sh). /rescue's tools only.
#
# The pool is freebsd/pool.zfs on userdata, a file Leste's
# pro1x-freebsd-pool wrote out in full and made immutable; its blocks on
# the partition are listed in freebsd/pool.map:
#   pro1x-pool-map 1 <inode> <size>
#   <offset> <length>        (bytes on the partition, one line per range)
# userdata's file system is never written: ZFS reaches the file's blocks
# through GEOM nop providers over exactly these ranges, concatenated. They
# are shared nops (kern.geom.nop.shared, patches/0029): GEOM allows one
# exclusive writer per provider, so ordinary nops over several ranges of
# userdata could not all be written. The map must belong to that very file
# (inode, size) and the result must hold a pool named pro1x with all four
# labels, else nothing is imported and nothing written. ext2fs holds the
# partition exclusively even read only, which would keep ZFS from writing:
# userdata is unmounted for the pool.
#
# pool_rw HOST PARTITION [MAP]: the pool imported read-write, not mounted,
# userdata no longer at HOST (0); or everything undone, userdata mounted
# read only at HOST again (1). MAP (tests): another map of the file.

POOL_GEOMS=""
POOL_HOST=""
POOL_PART=""

pool_undo()
{
	local g
	[ -e /dev/concat/pro1xpool ] && geom concat destroy pro1xpool
	for g in $POOL_GEOMS; do
		geom nop destroy $g 2> /dev/null
	done
	POOL_GEOMS=""
	if [ -n "$POOL_HOST" ]; then
		mount -t ext2fs -o ro $POOL_PART $POOL_HOST
		POOL_HOST=""
	fi
	return 1
}

pool_rw()
{
	local host=$1 part=$2 map file dev ranges i provs
	map=${3:-$host/freebsd/pool.map}
	file=$host/freebsd/pool.zfs
	[ -f $map ] && [ -f $file ] || return 1
	set -- $(head -1 $map)
	if [ "$1 $2" != "pro1x-pool-map 1" ] || [ -z "$4" ]; then
		echo "!! $map: not a pool map"
		return 1
	fi
	local inode=$3 size=$4
	set -- $(ls -i $file)
	[ "$1" = "$inode" ] || { echo "!! $map: for inode $inode, $file is $1"; return 1; }
	set -- $(ls -ln $file)
	[ "$5" = "$size" ] || { echo "!! $map: for $size bytes, $file has $5"; return 1; }
	ranges=$(sed 1d $map)
	set -- $ranges
	[ $# -ge 2 ] || { echo "!! $map: no ranges"; return 1; }
	sysctl -q kern.geom.nop.shared > /dev/null ||
	    { echo "!! the kernel's nops cannot be shared (patches/0029)"; return 1; }
	kldstat -q -m g_concat 2> /dev/null || geom concat load 2> /dev/null
	umount $host || { echo "!! $host: busy"; return 1; }
	POOL_HOST=$host
	POOL_PART=$part
	sysctl -q kern.geom.nop.shared=1 > /dev/null
	i=0
	provs=""
	while [ $# -ge 2 ]; do
		# (gnop names the provider <name>.nop)
		if ! geom nop create -Z pro1xpool$i -o $1 -s $2 $part; then
			sysctl -q kern.geom.nop.shared=0 > /dev/null
			pool_undo
			return 1
		fi
		POOL_GEOMS="$POOL_GEOMS pro1xpool$i.nop"
		provs="$provs /dev/pro1xpool$i.nop"
		i=$((i + 1))
		shift 2
	done
	sysctl -q kern.geom.nop.shared=0 > /dev/null
	geom concat create pro1xpool $provs || pool_undo || return 1
	dev=/dev/concat/pro1xpool
	if [ -z "$(zdb -l $dev | sed -n "s/^ *labels = 0 1 2 3 *$/ok/p")" ] ||
	    [ -z "$(zdb -l $dev | sed -n "s/^ *name: 'pro1x'$/ok/p")" ]; then
		echo "!! $map: no pool pro1x with all four labels there"
		pool_undo
		return 1
	fi
	if ! zpool import -f -N -o cachefile=none -d $dev pro1x; then
		echo "!! $dev: the pool would not import"
		pool_undo
		return 1
	fi
	# the file may be larger than the pool image it was made from
	zpool set autoexpand=on pro1x
	zpool online -e pro1x $dev 2> /dev/null
	POOL_HOST=""
	return 0
}

# pool_bootfs: the boot environment to start: kenv pro1x.be=<name> or the
# pool's bootfs; with b + Enter within 5 s a menu of them
pool_bootfs()
{
	local bootfs be key n i
	bootfs=$(zpool get -H -o value bootfs pro1x)
	be=$(kenv -q pro1x.be)
	if [ -n "$be" ] && zfs list -H -o name pro1x/ROOT/$be > /dev/null 2>&1; then
		bootfs=pro1x/ROOT/$be
	fi
	echo "--- boot environments (bectl):" > /dev/console
	bectl list > /dev/console 2>&1
	echo "=== ${bootfs#pro1x/ROOT/}; b + Enter within 5 s: another one" > /dev/console
	if read -t 5 key < /dev/console && [ "${key#b}" != "$key" ]; then
		i=0
		for n in $(zfs list -H -o name -d 1 pro1x/ROOT); do
			[ $n = pro1x/ROOT ] && continue
			i=$((i + 1))
			echo "  $i  ${n#pro1x/ROOT/}" > /dev/console
		done
		echo "number + Enter:" > /dev/console
		if read -t 60 key < /dev/console; then
			i=0
			for n in $(zfs list -H -o name -d 1 pro1x/ROOT); do
				[ $n = pro1x/ROOT ] && continue
				i=$((i + 1))
				[ "$i" = "$key" ] && bootfs=$n
			done
		fi
	fi
	echo $bootfs
}
