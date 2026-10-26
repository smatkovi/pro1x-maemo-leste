#!/bin/sh
# try-zfsroot.sh /path/root.zfs: in the FreeBSD test VM (vm.sh), as root:
# what the Pro1X's RAM root does with root.zfs (mfsroot/etc/rc): the file
# as a read-only md, the pool imported read only, a reroot into
# pro1x/ROOT/default. The VM then runs the Pro1X's system (its console
# shows "pro1x-freebsd login:"); vm.sh run starts the VM's own again.
# The image goes into memory first: a file on the VM's own root would be
# gone with the reroot (on the Pro1X userdata stays mounted, patch 0012).
set -e
img=${1:?root.zfs}
kldstat -q -m zfs || kldload zfs
md=$(mdconfig -a -t malloc -s "$(stat -f %z "$img")")
dd if="$img" of=/dev/$md bs=1m status=none
zpool import -f -N -o readonly=on -o cachefile=none -d /dev/$md pro1x
zfs list -o name,used,refer,compressratio,readonly
kenv vfs.root.mountfrom=zfs:pro1x/ROOT/default
kenv vfs.root.mountfrom.options=ro
echo "=== rerooting into $img"
reboot -r
