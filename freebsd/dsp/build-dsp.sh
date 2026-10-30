#!/bin/bash
# Builds the DSP stack's modules and tools into $WORK/dsp-test, laid out as
# /host/freebsd/ wants them for the device runs (freebsd/dsp/README.md):
# modules/ (found before the root image's), dsp-tools/ (pro1x-dsp, the
# test script) and cmdline.dsp-* (one per run, each with its watchdog
# lifetime), without touching out/ (freebsd/build.sh builds the same
# modules into out/modules with the rest).
#
#   WORK=/path/to/freebsd-build bash freebsd/dsp/build-dsp.sh [module|tools|all]
#
# The modules are built against the PRO1X kernel in $WORK/obj (KERNFAST:
# the kernel itself is only relinked if its sources changed); a device run
# needs the out/kernel.bin built from the same tree. Holds
# $WORK/.kbuild-lock while make runs.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
WORK=${WORK:?set WORK=<build directory>}
WHAT=${1:-all}
SRC=$WORK/src
T=$WORK/dsp-test
JOBS=${JOBS:-$(nproc)}
MODS="qcom_dsp qrtr"
mkdir -p "$T/modules" "$T/dsp-tools"

lock=$WORK/.kbuild-lock
take_lock() {
	while [ -e "$lock" ]; do
		echo ">> $lock held: $(cat "$lock" 2>/dev/null); waiting"
		sleep 30
	done
	echo "dsp agent (freebsd/dsp/build-dsp.sh) $(date -Iseconds)" > "$lock"
	trap 'rm -f "$lock"' EXIT
}

if [ "$WHAT" = module ] || [ "$WHAT" = all ]; then
	take_lock
	cp -a "$REPO/freebsd/dsp/sys/." "$SRC/sys/"
	export PATH=$WORK/hostbin:$PATH MAKEOBJDIRPREFIX=$WORK/obj
	(cd "$SRC" && ./tools/build/make.py --cross-bindir=/usr/bin \
	    --cross-compiler-type=clang --host-bindir=/usr/bin \
	    --host-compiler-type=clang TARGET=arm64 TARGET_ARCH=aarch64 \
	    -j"$JOBS" KERNCONF=PRO1X MODULES_OVERRIDE="$MODS" \
	    -DWITH_KERNEL_BIN KERNFAST=1 -DNO_KERNELCLEAN buildkernel) \
	    > "$T/build-module.log" 2>&1 || { tail -40 "$T/build-module.log"; exit 1; }
	rm -f "$lock"
	trap - EXIT
	for m in $MODS; do
		ko=$(find "$WORK/obj" -path "*/sys/PRO1X/modules/*/$m.ko" | head -1)
		cp "$ko" "$T/modules/"
	done
	grep -E ": (warning|error):" "$T/build-module.log" | grep -E "qualcomm/dsp" || true
	k=$(find "$WORK/obj" -path '*/sys/PRO1X/kernel.bin' | head -1)
	if cmp -s "$k" "$WORK/out/kernel.bin"; then
		echo ">> the modules fit out/kernel.bin"
	else
		echo ">> obj's kernel.bin differs from out/kernel.bin (newer" \
		    "sources there): copy that kernel too, or check the symbols"
	fi
	ls -la "$T/modules"
fi

if [ "$WHAT" = tools ] || [ "$WHAT" = all ]; then
	BASEROOT=$WORK/base-sysroot
	[ -f "$BASEROOT/.done" ] || { echo "no $BASEROOT (freebsd/build.sh makes it)"; exit 1; }
	clang --target=aarch64-unknown-freebsd15.0 --sysroot="$BASEROOT" \
	    -fuse-ld=lld -O2 -Wall -Wextra -Wno-unused-parameter \
	    -I"$REPO/freebsd/dsp/sys/arm64/qualcomm/dsp" \
	    -o "$T/dsp-tools/pro1x-dsp" "$REPO/freebsd/dsp/tools/pro1x-dsp/pro1x-dsp.c"
	cp "$REPO/freebsd/rootfs/usr/local/libexec/pro1x-dsptest" "$T/dsp-tools/"
	# One command line per device run (README's table): the watchdog ends
	# a hang after the lifetime (s); pro1x.pool=off keeps userdata mounted
	# at /dev/pro1x-host, where the newer modules and tools are found
	for run in load:120 adsp:180 sensors:300 modem:300 modemcx:300 \
	    adopt:300 rpm:120 wifi:420 wpa:480; do
		extra=
		[ "${run%:*}" = modemcx ] && extra=" hw.qcom_pas.proxy_votes=1"
		echo "pro1x.test=dsp pro1x.dsp=${run%:*} pro1x.pool=off" \
		    "hint.qcom_wdt.0.lifetime=${run#*:}$extra" > "$T/cmdline.dsp-${run%:*}"
	done
	ls -la "$T/dsp-tools" "$T"/cmdline.dsp-*
fi
