#!/bin/bash
# Builds the IPA's kernel module (qcom_ipa.ko) for the Pro1X into its own
# output directory, without touching out/ (freebsd/build.sh's): the sources
# into $WORK/src/sys (new files only), then the kernel build with
# MODULES_OVERRIDE=qcom_ipa in $WORK/obj under $WORK/.kbuild-lock.
#
#   WORK=/path/to/freebsd-build bash freebsd/data/build.sh
#   OUT=...      where the module goes (default $WORK/data-out)
#
# freebsd/build.sh builds it with the others (MODULES has qcom_ipa).
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
WORK=${WORK:?set WORK=<build directory>}
OUT=${OUT:-$WORK/data-out}
SRC=$WORK/src
JOBS=${JOBS:-$(nproc)}
LOCK=$WORK/.kbuild-lock
log() { echo ">> $*"; }

[ -d "$SRC/sys" ] || { echo "no $SRC: run freebsd/build.sh once"; exit 1; }
while [ -e "$LOCK" ]; do
	log "waiting for $(cat "$LOCK" 2>/dev/null)"
	sleep 30
done
echo "IPA/mobile-data agent: freebsd/data/build.sh (qcom_ipa), $(date "+%F %T")" > "$LOCK"
trap 'rm -f "$LOCK"' EXIT

cp -a "$REPO/freebsd/data/sys/." "$SRC/sys/"
# (the DSP stack's headers, qcom_dsp.h, qcom_qmi.h, ..., as freebsd/build.sh
# last copied them)
export PATH=$WORK/hostbin:$PATH MAKEOBJDIRPREFIX=$WORK/obj
MAKE=(./tools/build/make.py --cross-bindir=/usr/bin --cross-compiler-type=clang
      --host-bindir=/usr/bin --host-compiler-type=clang
      TARGET=arm64 TARGET_ARCH=aarch64 -j"$JOBS" -DNO_WERROR WERROR=)
log "qcom_ipa"
(cd "$SRC" && "${MAKE[@]}" KERNCONF=PRO1X MODULES_OVERRIDE=qcom_ipa \
	-DWITH_KERNEL_BIN KERNFAST=1 -DNO_KERNELCLEAN buildkernel)
ko=$(find "$WORK/obj" -path "*/sys/PRO1X/modules/*/qcom_ipa.ko" | head -1)
[ -f "$ko" ] || { echo "qcom_ipa.ko not built"; exit 1; }
mkdir -p "$OUT/modules"
cp "$ko" "$OUT/modules/"
log "$OUT/modules/qcom_ipa.ko"
