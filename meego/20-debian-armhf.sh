#!/bin/bash
# Build the Debian trixie armhf tree that supplies a current glibc and
# Mesa (freedreno for the Adreno 610, llvmpipe as fallback) to Harmattan.
#
# Harmattan calls itself armel, but it is ARMv7 + VFPv3 + NEON built for the
# hard-float calling convention (Tag_ABI_VFP_args: VFP registers), which is
# exactly Debian's armhf. Only its loader name differs (ld-linux.so.3), and
# its objects carry no float-ABI flag, so armhf's loader accepts them.
# Runs on the Pro1X under Maemo Leste: the CPU executes AArch32 natively,
# no qemu needed.
#
#   D=/host/debian-armhf bash meego/20-debian-armhf.sh
set -eu

D=${D:-/host/debian-armhf}
MIRROR=${MIRROR:-http://deb.debian.org/debian}
KEYRING=/usr/share/keyrings/debian-archive-keyring.gpg

# Everything Harmattan needs from Debian. The graft script copies the
# runtime dependency closure of these packages.
PACKAGES=$(sed 's/#.*//' "$(dirname "$0")/debian-packages.list")

if [ ! -x "$D/usr/bin/apt-get" ]; then
	command -v debootstrap >/dev/null || apt-get install -y debootstrap debian-archive-keyring
	mkdir -p "$D"
	debootstrap --arch=armhf --variant=minbase --keyring="$KEYRING" trixie "$D" "$MIRROR"
fi

for m in proc sys dev; do
	mountpoint -q "$D/$m" || mount --bind "/$m" "$D/$m"
done
cp /etc/resolv.conf "$D/etc/resolv.conf"

chroot "$D" apt-get update -q
chroot "$D" env DEBIAN_FRONTEND=noninteractive \
	apt-get install -y -q --no-install-recommends $PACKAGES

# Locales Harmattan's UI is likely to ask for.
cat > "$D/etc/locale.gen" <<'EOF'
en_US.UTF-8 UTF-8
en_GB.UTF-8 UTF-8
de_AT.UTF-8 UTF-8
de_DE.UTF-8 UTF-8
EOF
chroot "$D" locale-gen

for m in dev sys proc; do
	umount "$D/$m" 2>/dev/null || true
done
echo "Debian armhf tree ready in $D"
