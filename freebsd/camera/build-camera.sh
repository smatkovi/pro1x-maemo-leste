#!/bin/bash
# Builds the camera's module and tools into $WORK/cam-test, laid out as
# /host/freebsd/ wants them (modules/, camera/, cmdline.cam-*), without
# touching out/ (freebsd/build.sh builds qcom_camss.ko into out/modules
# with the rest).
#
#   WORK=/path/to/freebsd-build bash freebsd/camera/build-camera.sh [module|tools|all]
#
# The module is built against the PRO1X kernel in $WORK/obj (KERNFAST: the
# kernel itself is only relinked if its sources changed); it is tied to
# that kernel (MODULE_TIED), so the device run needs the out/kernel.bin
# built from the same tree. Holds $WORK/.kbuild-lock while make runs.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
WORK=${WORK:?set WORK=<build directory>}
WHAT=${1:-all}
SRC=$WORK/src
T=$WORK/cam-test
JOBS=${JOBS:-$(nproc)}
mkdir -p "$T/modules" "$T/camera"

lock=$WORK/.kbuild-lock
take_lock() {
	while [ -e "$lock" ]; do
		echo ">> $lock held: $(cat "$lock" 2>/dev/null); waiting"
		sleep 30
	done
	echo "camera agent (freebsd/camera/build-camera.sh) $(date -Iseconds)" > "$lock"
	trap 'rm -f "$lock"' EXIT
}

if [ "$WHAT" = module ] || [ "$WHAT" = all ]; then
	take_lock
	cp -a "$REPO/freebsd/camera/sys/." "$SRC/sys/"
	export PATH=$WORK/hostbin:$PATH MAKEOBJDIRPREFIX=$WORK/obj
	(cd "$SRC" && ./tools/build/make.py --cross-bindir=/usr/bin \
	    --cross-compiler-type=clang --host-bindir=/usr/bin \
	    --host-compiler-type=clang TARGET=arm64 TARGET_ARCH=aarch64 \
	    -j"$JOBS" KERNCONF=PRO1X MODULES_OVERRIDE=qcom_camss \
	    -DWITH_KERNEL_BIN KERNFAST=1 -DNO_KERNELCLEAN buildkernel) \
	    > "$T/build-module.log" 2>&1 || { tail -30 "$T/build-module.log"; exit 1; }
	rm -f "$lock"
	trap - EXIT
	ko=$(find "$WORK/obj" -path '*/sys/PRO1X/modules/*/qcom_camss.ko' | head -1)
	cp "$ko" "$T/modules/"
	grep -E ": (warning|error):" "$T/build-module.log" || true
	k=$(find "$WORK/obj" -path '*/sys/PRO1X/kernel.bin' | head -1)
	if cmp -s "$k" "$WORK/out/kernel.bin"; then
		echo ">> the module fits out/kernel.bin"
	else
		# (other drivers' changes since out/kernel.bin was copied): the
		# module needs only stable kernel interfaces, see the symbols
		echo ">> obj's kernel.bin differs from out/kernel.bin (newer" \
		    "sources there); the module's undefined symbols:" \
		    "$(llvm-nm -u "$T/modules/qcom_camss.ko" 2>/dev/null | wc -l)"
	fi
	ls -la "$T/modules/qcom_camss.ko"
fi

if [ "$WHAT" = tools ] || [ "$WHAT" = all ]; then
	BASEROOT=$WORK/base-sysroot
	[ -f "$BASEROOT/.done" ] || { echo "no $BASEROOT (freebsd/build.sh makes it)"; exit 1; }
	clang --target=aarch64-unknown-freebsd15.0 --sysroot="$BASEROOT" \
	    -fuse-ld=lld -O2 -Wall -Wextra -Wno-unused-parameter \
	    -I"$REPO/freebsd/camera/sys/arm64/qualcomm/camss" \
	    -o "$T/camera/pro1x-cam" "$REPO"/freebsd/camera/tools/pro1x-cam/*.c -lz -lm
	cp "$REPO/freebsd/rootfs/usr/local/libexec/pro1x-camtest" "$T/camera/"
	for run in id raw jpeg; do
		lt=240
		[ $run = id ] && lt=180
		echo "pro1x.test=cam pro1x.cam=$run hint.qcom_wdt.0.lifetime=$lt" > "$T/cmdline.cam-$run"
	done
	ls -la "$T/camera"
fi
