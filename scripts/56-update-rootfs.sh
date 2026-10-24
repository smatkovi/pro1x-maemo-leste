#!/bin/sh
# Updates the rootfs image on the device -- as an rsync delta.
# Runs ON THE DEVICE as root.
#
# Here rsync is the right tool: after the first deployment both sides hold the
# same 24 GiB image, and usually only a fraction has changed (new kernel, new
# modules, a few packages). With --inplace and --no-whole-file rsync
# transfers only the differing blocks instead of the whole file.
#
# Precondition: both sides have the same file size/geometry
# (55-deploy-rootfs.sh establishes that). Without --inplace rsync would create
# a 24 GiB copy next to it -- userdata has room for that, but it is slower and
# unnecessary.
set -e
HOST=${HOST:?set HOST=user@buildhost}
SRC=${SRC:?set SRC=/path/out/maemo-leste.img on the build host}
DST=${DST:-/userdata/maemo-leste.img}
SSH_KEY=${SSH_KEY:-/home/defaultuser/.ssh/id_ed25519}
SSH_KNOWN=${SSH_KNOWN:-/home/defaultuser/.ssh/known_hosts}

[ -f "$DST" ] || { echo "$DST missing -- run 55-deploy-rootfs.sh first"; exit 1; }

# The image must not be mounted during the update.
if grep -q "$DST" /proc/mounts 2>/dev/null; then
  echo "$DST is mounted -- unmount it first"; exit 1
fi
for l in $(ls /sys/block/loop*/loop/backing_file 2>/dev/null); do
  if [ "$(cat "$l" 2>/dev/null)" = "$DST" ]; then
    echo "$DST is attached to a loop device -- detach it first (losetup -d)"; exit 1
  fi
done

echo ">> rsync delta $HOST:$SRC -> $DST"
rsync -a --inplace --no-whole-file --info=progress2 \
  -e "ssh -o BatchMode=yes -i $SSH_KEY -o UserKnownHostsFile=$SSH_KNOWN" \
  "$HOST:$SRC" "$DST"

e2fsck -fp "$DST" || true
dumpe2fs -h "$DST" 2>/dev/null | grep -E 'Block count|Free blocks|Filesystem state'
