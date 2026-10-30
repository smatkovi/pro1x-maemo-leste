#!/bin/bash
# Builds the power management kernels (freebsd/pm/README.md) into
# $WORK/pm-out, next to freebsd/build.sh's out/ (which it does not touch),
# with the sources, patches and toolchain build.sh left in $WORK:
#
#   WORK=... bash freebsd/pm/build.sh [qemu] [phone]
#
#   qemu    PRO1XQ: QEMU's virt, a PL011 console, test/qemu/rc as the root
#           filesystem's /etc/rc (test/qemu/run.sh runs it)
#   phone   PRO1XPM: freebsd/PRO1X (psci_idle, pro1x_pm), the RAM root of
#           freebsd/mfsroot (its rc runs the pro1x.test=smp/idle/suspend/usb
#           device tests of mfsroot/etc/pro1x-pmtest.sh); pm-out/kernel.bin
#           goes to /host/freebsd/kernel.bin on the phone, pm-out/modules
#           (qcom_dsp.ko, pro1x_rpmsleep.ko: the RPM sleep set) to
#           /host/freebsd/pm/
#
# KERNFAST=1: the kernel again without a clean build (no config: not after a
# change to the config or the env file); NOCLEAN=1: config again, no clean
# build. Takes $WORK/.kbuild-lock
# (freebsd/build.sh's sources and objects are shared), waiting for it.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
WORK=${WORK:?set WORK=<build directory>}
SRC=$WORK/src
PMOUT=$WORK/pm-out
JOBS=${JOBS:-$(nproc)}
TARGETS=${*:-qemu phone}
log() { echo ">> $*"; }
mkdir -p "$PMOUT"

LOCK=$WORK/.kbuild-lock
while [ -e "$LOCK" ] && ! grep -q "^pm-agent" "$LOCK"; do
	log "waiting for $(cat "$LOCK")"
	sleep 30
done
mine=
if [ ! -e "$LOCK" ]; then
	echo "pm-agent (freebsd/pm/build.sh) $(date -Iseconds)" > "$LOCK"
	mine=1
fi
trap '[ -z "$mine" ] || rm -f "$LOCK"' EXIT

# the new files (and the DSP stack's, for its qcom_dsp.ko), and the
# patches build.sh has not applied yet (0032 on)
cp -a "$REPO/freebsd/pm/sys/." "$SRC/sys/"
cp -a "$REPO/freebsd/dsp/sys/." "$SRC/sys/"
for p in "$REPO"/freebsd/patches/*.patch; do
	mark=$SRC/.pro1x-applied-$(basename "$p" .patch)
	[ -f "$mark" ] && continue
	log "patch $(basename "$p")"
	git -C "$SRC" apply "$p"
	touch "$mark"
done

export PATH=$WORK/hostbin:$PATH MAKEOBJDIRPREFIX=$WORK/obj
MAKE=(./tools/build/make.py --cross-bindir=/usr/bin --cross-compiler-type=clang
      --host-bindir=/usr/bin --host-compiler-type=clang
      TARGET=arm64 TARGET_ARCH=aarch64 -j"$JOBS" -DNO_WERROR WERROR=)
MAKEFS=$(find "$WORK/obj" -path '*/tmp/legacy/*' -name makefs -type f | head -1)
[ -x "$MAKEFS" ] || { echo "makefs not built: run freebsd/build.sh first"; exit 1; }
B=$WORK/dl/base.txz

# a root filesystem in RAM: the release's /rescue plus the given tree
mkroot() {	# mkroot IMAGE DIR...
	local img=$1 root=$PMOUT/root.$$
	shift
	rm -rf "$root"
	mkdir -p "$root"/{dev,tmp,bin,sbin,etc}
	bsdtar -xpf "$B" -C "$root" ./rescue
	ln -s /rescue/sh "$root/bin/sh"
	ln -s /rescue/init "$root/sbin/init"
	for d in "$@"; do cp -a "$d/." "$root/"; done
	"$MAKEFS" -t ffs -B little -o version=2 "$img" "$root"
	rm -rf "$root"
}

# the Linux arm64 Image header's image_size: 8 MiB more (build.sh)
fixsize() {
	python3 - "$1" <<'EOF'
import struct, sys
with open(sys.argv[1], 'r+b') as f:
    h = f.read(64)
    assert h[0x38:0x3c] == b'ARMd', 'no arm64 Image header'
    size = struct.unpack_from('<Q', h, 0x10)[0]
    f.seek(0x10)
    f.write(struct.pack('<Q', size + (8 << 20)))
EOF
}

kernel() {	# kernel CONF TEMPLATE MFS ENV OUT [MODULES]
	local conf=$1 tmpl=$2 mfs=$3 env=$4 out=$5 mods=${6:-}
	sed -e "s|@MFS_IMAGE@|$mfs|" -e "s|@ENV_FILE@|$env|" "$tmpl" \
	    > "$SRC/sys/arm64/conf/$conf"
	log "kernel $conf"
	(cd "$SRC" && "${MAKE[@]}" KERNCONF="$conf" MODULES_OVERRIDE="$mods" \
	    -DWITH_KERNEL_BIN ${KERNFAST:+KERNFAST=1} ${NOCLEAN:+-DNO_KERNELCLEAN} \
	    buildkernel) \
	    > "$PMOUT/build-$conf.log" 2>&1 ||
	    { tail -30 "$PMOUT/build-$conf.log"; exit 1; }
	local k
	k=$(find "$WORK/obj" -path "*/sys/$conf/kernel.bin" | head -1)
	cp "$k" "$out"
	fixsize "$out"
	log "$out"
	[ -z "$mods" ] || mkdir -p "$PMOUT/modules"
	for m in $mods; do
		m=${m##*/}
		ko=$(find "$WORK/obj" -path "*/sys/$conf/modules/*" -name "$m.ko" |
		    head -1)
		[ -n "$ko" ] || ko=$(find "$WORK/obj" -path "*/sys/$conf/modules/*" \
		    -name "*$m.ko" | head -1)
		cp "$ko" "$PMOUT/modules/"
		log "$PMOUT/modules/${ko##*/}"
	done
}

for t in $TARGETS; do
	case $t in
	qemu)
		mkdir -p "$PMOUT/qemu-etc/etc"
		cp "$REPO/freebsd/pm/test/qemu/rc" "$PMOUT/qemu-etc/etc/rc"
		cp "$REPO/freebsd/mfsroot/etc/pro1x-pmtest.sh" "$PMOUT/qemu-etc/etc/"
		mkroot "$PMOUT/mfsroot-qemu.ufs" "$PMOUT/qemu-etc"
		kernel PRO1XQ "$REPO/freebsd/pm/test/qemu/PRO1XQ" \
		    "$PMOUT/mfsroot-qemu.ufs" "$REPO/freebsd/pm/test/qemu/pro1xq.env" \
		    "$PMOUT/kernel-qemu.bin"
		;;
	phone)
		mkroot "$PMOUT/mfsroot.ufs" "$REPO/freebsd/mfsroot"
		{ cat "$REPO/freebsd/PRO1X"; echo; echo "# freebsd/pm/build.sh"
		  for d in psci_idle pro1x_pm; do
			grep -q "^device[[:space:]]*$d" "$REPO/freebsd/PRO1X" ||
			    echo "device		$d"
		  done; } |
		    sed -e 's/^ident[[:space:]]*PRO1X$/ident		PRO1XPM/' \
		    > "$PMOUT/PRO1XPM.tmpl"
		kernel PRO1XPM "$PMOUT/PRO1XPM.tmpl" "$PMOUT/mfsroot.ufs" \
		    "$REPO/freebsd/pro1x.env" "$PMOUT/kernel.bin" \
		    "qcom_dsp pro1x_rpmsleep usb/usb usb/template usb/uether usb/cdce pro1x_usb"
		;;
	*)	echo "unknown target $t"; exit 1 ;;
	esac
done
