#!/bin/bash
# Builds and tests the random numbers' kernels (freebsd/rng/README.md) into
# $WORK/rng-out, next to freebsd/build.sh's out/ (which it does not touch),
# with the sources, patches and toolchain build.sh left in $WORK:
#
#   WORK=... bash freebsd/rng/build.sh [host] [qemu] [phone]
#
#   host    qcom_rnd's qcom,prng-ee read on the PC against a model of its
#           registers (test/qcom_rnd_ee_test.c)
#   qemu    PRO1XQRNG: QEMU's virt, a PL011 console, test/qemu/rc as the
#           root filesystem's /etc/rc (test/qemu/run.sh runs it)
#   phone   PRO1XRNG: freebsd/PRO1X as it is (with qcom_rnd), no modules:
#           that the phone's kernel builds; rng-out/kernel.bin is not
#           meant for the phone (the joint rebuild makes that one)
#
# KERNFAST=1: the kernel again without a clean build. RNGOUT=<directory>:
# the results there. Takes $WORK/.kbuild-lock (build.sh's sources and
# objects are shared), waiting for it; LOCKNAME (rng-agent): the name in
# it, a lock that starts with it is taken as one's own.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
WORK=${WORK:?set WORK=<build directory>}
SRC=$WORK/src
RNGOUT=${RNGOUT:-$WORK/rng-out}
JOBS=${JOBS:-$(nproc)}
TARGETS=${*:-host qemu phone}
log() { echo ">> $*"; }
mkdir -p "$RNGOUT"

if [ "$TARGETS" != host ]; then
	LOCK=$WORK/.kbuild-lock
	LOCKNAME=${LOCKNAME:-rng-agent}
	while [ -e "$LOCK" ] && ! grep -q "^$LOCKNAME" "$LOCK"; do
		log "waiting for $(cat "$LOCK")"
		sleep 30
	done
	mine=
	if [ ! -e "$LOCK" ]; then
		echo "$LOCKNAME (freebsd/rng/build.sh) $(date -Iseconds)" > "$LOCK"
		mine=1
	fi
	trap '[ -z "$mine" ] || rm -f "$LOCK"' EXIT
	# the patches build.sh has not applied yet (0038, 0039 on)
	for p in "$REPO"/freebsd/patches/*.patch; do
		mark=$SRC/.pro1x-applied-$(basename "$p" .patch)
		[ -f "$mark" ] && continue
		log "patch $(basename "$p")"
		git -C "$SRC" apply "$p"
		touch "$mark"
	done
fi

export PATH=$WORK/hostbin:$PATH MAKEOBJDIRPREFIX=$WORK/obj
MAKE=(./tools/build/make.py --cross-bindir=/usr/bin --cross-compiler-type=clang
      --host-bindir=/usr/bin --host-compiler-type=clang
      TARGET=arm64 TARGET_ARCH=aarch64 -j"$JOBS" -DNO_WERROR WERROR=)
B=$WORK/dl/base.txz

mkroot() {	# mkroot IMAGE DIR...
	local img=$1 root=$RNGOUT/root.$$ makefs
	makefs=$(find "$WORK/obj" -path '*/tmp/legacy/*' -name makefs -type f | head -1)
	[ -x "$makefs" ] || { echo "makefs not built: run freebsd/build.sh first"; exit 1; }
	shift
	rm -rf "$root"
	mkdir -p "$root"/{dev,tmp,bin,sbin,etc}
	bsdtar -xpf "$B" -C "$root" ./rescue
	ln -s /rescue/sh "$root/bin/sh"
	ln -s /rescue/init "$root/sbin/init"
	for d in "$@"; do cp -a "$d/." "$root/"; done
	"$makefs" -t ffs -B little -o version=2 "$img" "$root"
	rm -rf "$root"
}

# the Linux arm64 Image header's image_size: 8 MiB more (build.sh)
fixsize() {
	python3 - "$1" <<'PY'
import struct, sys
with open(sys.argv[1], 'r+b') as f:
    h = f.read(64)
    assert h[0x38:0x3c] == b'ARMd', 'no arm64 Image header'
    size = struct.unpack_from('<Q', h, 0x10)[0]
    f.seek(0x10)
    f.write(struct.pack('<Q', size + (8 << 20)))
PY
}

kernel() {	# kernel CONF TEMPLATE MFS ENV OUT
	local conf=$1 tmpl=$2 mfs=$3 env=$4 out=$5 k
	sed -e "s|@MFS_IMAGE@|$mfs|" -e "s|@ENV_FILE@|$env|" "$tmpl" \
	    > "$SRC/sys/arm64/conf/$conf"
	log "kernel $conf"
	(cd "$SRC" && "${MAKE[@]}" KERNCONF="$conf" MODULES_OVERRIDE= \
	    -DWITH_KERNEL_BIN ${KERNFAST:+KERNFAST=1} buildkernel) \
	    > "$RNGOUT/build-$conf.log" 2>&1 ||
	    { tail -30 "$RNGOUT/build-$conf.log"; exit 1; }
	grep -E "qcom_rnd|random_harvestq" "$RNGOUT/build-$conf.log" | grep -E "warning|error" || true
	k=$(find "$WORK/obj" -path "*/sys/$conf/kernel.bin" | head -1)
	cp "$k" "$out"
	fixsize "$out"
	log "$out"
}

for t in $TARGETS; do
	case $t in
	host)
		log "qcom_rnd_ee_test"
		# the header alone (sys/ itself would hide the PC's sys/*.h)
		mkdir -p "$RNGOUT/inc/dev/qcom_rnd"
		cp "$SRC/sys/dev/qcom_rnd/qcom_rnd_ee.h" "$RNGOUT/inc/dev/qcom_rnd/"
		cc -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
		    -I"$RNGOUT/inc" -o "$RNGOUT/qcom_rnd_ee_test" \
		    "$REPO/freebsd/rng/test/qcom_rnd_ee_test.c"
		"$RNGOUT/qcom_rnd_ee_test"
		;;
	qemu)
		mkdir -p "$RNGOUT/qemu-etc/etc"
		cp "$REPO/freebsd/rng/test/qemu/rc" "$RNGOUT/qemu-etc/etc/rc"
		mkroot "$RNGOUT/mfsroot-qemu.ufs" "$RNGOUT/qemu-etc"
		kernel PRO1XQRNG "$REPO/freebsd/rng/test/qemu/PRO1XQRNG" \
		    "$RNGOUT/mfsroot-qemu.ufs" "$REPO/freebsd/rng/test/qemu/pro1xqrng.env" \
		    "$RNGOUT/kernel-qemu.bin"
		;;
	phone)
		mkroot "$RNGOUT/mfsroot.ufs" "$REPO/freebsd/mfsroot"
		sed -e 's/^ident[[:space:]]*PRO1X$/ident		PRO1XRNG/' "$REPO/freebsd/PRO1X" \
		    > "$RNGOUT/PRO1XRNG.tmpl"
		grep -q "^device[[:space:]]*qcom_rnd" "$RNGOUT/PRO1XRNG.tmpl" ||
		    { echo "freebsd/PRO1X has no device qcom_rnd"; exit 1; }
		kernel PRO1XRNG "$RNGOUT/PRO1XRNG.tmpl" "$RNGOUT/mfsroot.ufs" \
		    "$REPO/freebsd/pro1x.env" "$RNGOUT/kernel.bin"
		;;
	*)	echo "unknown target $t"; exit 1 ;;
	esac
done
