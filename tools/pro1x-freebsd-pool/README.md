# pro1x-freebsd-pool

FreeBSD's writable ZFS pool on the F(x)tec Pro1X, made on Leste: the file
`/host/freebsd/pool.zfs` (from `root.zfs`, every byte written, immutable)
and its block map `pool.map`, which FreeBSD's RAM root uses to reach the
file's blocks without ever writing ext4. How it fits together:
`freebsd/README.md`, "Writable ZFS, boot environments, a way back".

    pro1x-freebsd-pool create [--size 8G]   # as root, FreeBSD not running
    pro1x-freebsd-pool status
    pro1x-freebsd-pool map                  # the map again
    pro1x-freebsd-pool remove               # the pool is gone with it

Tried on a test userdata on the PC and in the FreeBSD VM; not on the phone.
