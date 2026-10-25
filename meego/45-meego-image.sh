#!/bin/bash
# MeeGo's own system image: a copy of the Leste system underneath.
#
# The MeeGo boot runs Harmattan's UI on Leste's hardware layer. With its own
# copy of that layer (/host/meego-leste.img) MeeGo no longer shares the
# image of the Leste system: updates, experiments or breakage in Leste do
# not reach it. The boot menu's "MeeGo Harmattan" entry uses
# /meego-leste.img when it exists, and the Leste image otherwise
# (tools/pro1x-boot).
#
# Runs as root in the Leste or MeeGo boot, after steps 20-41 (the copy
# carries everything they installed). Copying the running system file by
# file is fine for this: the boot that follows starts it fresh.
#
# From then on the two systems are separate: what steps 39-41 install on
# the Leste side (mce, the session, the helpers) reaches the copy only when
# they run in the MeeGo boot, and a new kernel's modules (/lib/modules) have
# to go into both images. The Harmattan tree (/host/harmattan.img, steps
# 30-38) stays shared, and so does userdata on /host.
#
#   bash meego/45-meego-image.sh            # IMG=/host/meego-leste.img SIZE=12G
set -euo pipefail
IMG=${IMG:-/host/meego-leste.img}
SIZE=${SIZE:-12G}
MNT=${MNT:-/mnt/meego-image}

[ -e "$IMG" ] && { echo "$IMG exists"; exit 1; }
command -v rsync > /dev/null || apt-get install -y -q rsync

truncate -s "$SIZE" "$IMG"
mkfs.ext4 -q -L meego "$IMG"
mkdir -p "$MNT"
mount -o loop "$IMG" "$MNT"
trap 'umount "$MNT"' EXIT

# The root file system only (-x), without what is rebuilt or rewritten:
# build trees, the package cache, log files (not their directories).
rsync -aHAX -x --numeric-ids \
	--exclude=/root/src --exclude=/root/src-46 --exclude=/root/pkg \
	--exclude='/var/cache/apt/archives/*.deb' \
	--include='/var/log/**/' --exclude='/var/log/**' \
	--exclude='/tmp/*' / "$MNT/"
for d in proc sys dev run tmp host mnt media; do
	mkdir -p "$MNT/$d"
done
chmod 1777 "$MNT/tmp"
old=$(cat /etc/hostname)
echo meego > "$MNT/etc/hostname"
sed -i "s/\b$old\b/meego/g" "$MNT/etc/hosts"
sync
echo ">> $IMG done: choose \"MeeGo Harmattan\" in the boot menu"
