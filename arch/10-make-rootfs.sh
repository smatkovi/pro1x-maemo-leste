#!/bin/bash
# Arch Linux ARM rootfs image for the Pro1X boot menu (/host/arch.img).
#
# Runs as root on the phone, in Maemo Leste (native arm64, no emulation):
# Arch Linux ARM's generic aarch64 root filesystem (its signature checked
# against the Arch Linux ARM build key) into an ext4 image, which the
# initramfs (pro1x-loop-root) mounts as root like Leste's. The Pro1X
# hardware setup and the phone shell follow in 20-pro1x-hardware.sh.
#
#   bash arch/10-make-rootfs.sh            # IMG=/host/arch.img SIZE=16G
set -euo pipefail
IMG=${IMG:-/host/arch.img}
SIZE=${SIZE:-16G}
MNT=${MNT:-/mnt/arch}
DL=${DL:-/host/arch-dl}
URL=${URL:-http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz}
# Arch Linux ARM Build System <builder@archlinuxarm.org>
KEY=68B3537F39A313B3E574D06777193F152BDBE6A6
KEYSERVER=${KEYSERVER:-hkps://keyserver.ubuntu.com}

[ -e "$IMG" ] && { echo "$IMG exists"; exit 1; }
log() { echo ">> $*"; }

# bsdtar: the tarball is libarchive's, with file capabilities and ACLs
# that GNU tar drops
command -v bsdtar > /dev/null ||
	DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends libarchive-tools > /dev/null

mkdir -p "$DL"
T=$DL/${URL##*/}
[ -f "$T" ] || { log "downloading $URL"; curl -sSfL -o "$T.part" "$URL"; mv "$T.part" "$T"; }
curl -sSfL -o "$T.sig" "$URL.sig"

log "checking the signature"
G=$(mktemp -d)
gpg --homedir "$G" --batch --quiet --keyserver "$KEYSERVER" --recv-keys "$KEY"
# a good signature by exactly that key
gpg --homedir "$G" --batch --status-fd 1 --verify "$T.sig" "$T" 2> /dev/null |
	grep -q "^\[GNUPG:\] VALIDSIG $KEY " ||
	{ echo "the tarball is not signed by $KEY"; rm -rf "$G"; exit 1; }
rm -rf "$G"

log "image $IMG ($SIZE)"
truncate -s "$SIZE" "$IMG"
mkfs.ext4 -q -L arch "$IMG"
mkdir -p "$MNT"
mount -o loop "$IMG" "$MNT"
bsdtar --numeric-owner -xpf "$T" -C "$MNT"
log "done: $IMG mounted on $MNT"
