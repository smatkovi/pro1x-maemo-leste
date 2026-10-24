#!/bin/sh
# Brings the rootfs image onto the device for the first time and grows it there.
# Runs ON THE DEVICE as root (pulls via ssh from the build host).
#
# For the FIRST transfer, not for updates: the image is 24 GiB, which makes
# rsync over Wi-Fi unbearable (rsync reads the full apparent size, measured at
# 1.5 MB/s -> a good four hours). So it is shrunk on the build host with
# resize2fs -M, streamed compressed, and grown back here.
# For later changes: 56-update-rootfs.sh (rsync delta).
#
# Purely additive: the only result is a new file on userdata.
set -e
HOST=${HOST:?set HOST=user@buildhost}
SRC=${SRC:?set SRC=/path/out/maemo-leste.img on the build host}
DST=${DST:-/userdata/maemo-leste.img}
SIZE_GIB=${SIZE_GIB:-24}
SSH_KEY=${SSH_KEY:-/home/defaultuser/.ssh/id_ed25519}
SSH_KNOWN=${SSH_KNOWN:-/home/defaultuser/.ssh/known_hosts}

# The build host shrinks the image to its minimum size before sending.
echo ">> shrinking the source on $HOST"
ssh -o BatchMode=yes -i "$SSH_KEY" -o UserKnownHostsFile="$SSH_KNOWN" "$HOST" "
  set -e
  e2fsck -fp '$SRC' || true
  resize2fs -M '$SRC' >/dev/null
  blocks=\$(dumpe2fs -h '$SRC' 2>/dev/null | awk -F: '/Block count/{gsub(/ /,\"\",\$2); print \$2}')
  bs=\$(dumpe2fs -h '$SRC' 2>/dev/null | awk -F: '/Block size/{gsub(/ /,\"\",\$2); print \$2}')
  truncate -s \$((blocks * bs)) '$SRC'
  ls -la '$SRC'"

echo ">> transferring $HOST:$SRC -> $DST (zstd-compressed)"
ssh -o BatchMode=yes -i "$SSH_KEY" -o UserKnownHostsFile="$SSH_KNOWN" "$HOST" \
  "zstd -c -T0 -3 '$SRC'" | zstd -dc > "$DST"

# Careful: busybox on Sailfish has no truncate, hence python3.
echo ">> growing to ${SIZE_GIB} GiB"
python3 -c "open('$DST','r+b').truncate($SIZE_GIB*1024**3)"
e2fsck -fp "$DST" || true
resize2fs "$DST"

# Same geometry on both sides, so that 56-update-rootfs.sh can later really
# transfer only deltas.
echo ">> bringing the source on $HOST to ${SIZE_GIB} GiB as well"
ssh -o BatchMode=yes -i "$SSH_KEY" -o UserKnownHostsFile="$SSH_KNOWN" "$HOST" "
  set -e
  truncate -s ${SIZE_GIB}G '$SRC'
  e2fsck -fp '$SRC' || true
  resize2fs '$SRC' >/dev/null
  echo source-done"

echo ">> result"
dumpe2fs -h "$DST" 2>/dev/null | grep -E 'Filesystem volume name|Block count|Free blocks|Filesystem state'
ls -la "$DST"
