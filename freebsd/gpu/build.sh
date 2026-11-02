#!/bin/bash
# The Linux DRM core for the Adreno 610 (step "GPU 1", freebsd/gpu/README.md):
# drm-kmod 6.12-lts (github.com/freebsd/drm-kmod, pinned to $DRM_KMOD_COMMIT)
# with drm-kmod-patches/, cross-built as arm64 kernel modules against the
# PRO1X kernel in $WORK/obj (its opt_*.h, the releng/15.1 sources in
# $WORK/src/sys, the toolchain of freebsd/build.sh), into
# $WORK/out/gpu/modules:
#   dmabuf.ko          dma-buf, dma-fence (array, chain, unwrap), dma-resv,
#                      sync_file
#   drm.ko             the DRM core: GEM, PRIME, syncobj, the GPU scheduler
#                      (drm_sched), drm_exec, GPUVM, KMS and its helpers
#   linuxkpi_video.ko  from the base sources: drm.ko depends on it (HDMI
#                      infoframes, aperture, video= command line)
#   lindebugfs.ko      from the base sources: drm.ko depends on it (debugfs)
# linuxkpi.ko is freebsd/build.sh's (out/modules); the kernel has the rest
# drm.ko depends on (pci, iicbus, iic, iicbb, mem, pseudofs).
# Then the Linux v6.12 sources the msm port starts from, $WORK/gpu/linux-6.12
# (drivers/gpu/drm/msm, the GPU clock controller and what its files include,
# mdt_loader; streamed from kernel.org's tarball, SHA-256 checked, only these
# paths kept), and msm's register headers from its XML, generated/ (as
# Linux's drivers/gpu/drm/msm/Makefile makes them).
#
#   WORK=/path/to/freebsd-build bash freebsd/gpu/build.sh [modules|headers|all]
#   CLEAN=1 ...   the modules from scratch ($WORK/gpu/obj removed first)
#
# Needs freebsd/build.sh's kernel toolchain and PRO1X kernel in $WORK/obj
# (built once). The modules are built in the build's environment (make.py
# buildenv: its clang, ld.lld, bmake and share/mk) with SYSDIR and
# KERNBUILDDIR pointing at that kernel, objects in $WORK/gpu/obj; the kernel
# itself is not rebuilt. Holds $WORK/.kbuild-lock while make runs (waits
# while another build holds it).
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
WORK=${WORK:?set WORK=<build directory>}
WHAT=${1:-all}
JOBS=${JOBS:-$(nproc)}
LLVM_BIN=${LLVM_BIN:-/usr/bin}
SRC=$WORK/src
GPU=$WORK/gpu
OUT=$WORK/out/gpu
DRM_KMOD=$GPU/drm-kmod
DRM_KMOD_URL=https://github.com/freebsd/drm-kmod.git
DRM_KMOD_BRANCH=6.12-lts
DRM_KMOD_COMMIT=1d054ae55685c45c23aa86541427dfc72afa9ca1
KMODS="dmabuf drm"
BASE_MODULES="linuxkpi_video lindebugfs"
LINUX=${LINUX:-$GPU/linux-6.12}
LINUX_URL=https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.12.tar.xz
LINUX_SHA256=b1a2562be56e42afb3f8489d4c2a7ac472ac23098f1ef1c1e40da601f54625eb
# The paths kept from it: msm (with registers/: the XML and gen_header.py),
# its uapi header and the SMMU interface adreno_gpu.c uses, the SM6115 GPU
# clock controller with the qcom clock files it is built from (and the
# headers these include), mdt_loader for the zap shader
LINUX_PATHS="drivers/gpu/drm/msm include/uapi/drm/msm_drm.h
	include/linux/adreno-smmu-priv.h
	drivers/clk/qcom/gpucc-sm6115.c drivers/clk/qcom/gdsc.c
	drivers/clk/qcom/gdsc.h drivers/clk/qcom/clk-alpha-pll.c
	drivers/clk/qcom/clk-alpha-pll.h drivers/clk/qcom/clk-rcg2.c
	drivers/clk/qcom/clk-rcg.h drivers/clk/qcom/clk-branch.c
	drivers/clk/qcom/clk-branch.h drivers/clk/qcom/clk-regmap.h
	drivers/clk/qcom/clk-regmap-divider.h drivers/clk/qcom/clk-regmap-mux.h
	drivers/clk/qcom/common.h drivers/clk/qcom/reset.h
	include/dt-bindings/clock/qcom,sm6115-gpucc.h
	drivers/soc/qcom/mdt_loader.c include/linux/soc/qcom/mdt_loader.h"
# msm's register headers (its Makefile's ADRENO_HEADERS and DISPLAY_HEADERS)
MSM_XML="adreno/a2xx adreno/a3xx adreno/a4xx adreno/a5xx adreno/a6xx
	adreno/a6xx_gmu adreno/adreno_common adreno/adreno_pm4
	display/dsi_phy_7nm display/dsi_phy_10nm display/dsi_phy_14nm
	display/dsi_phy_20nm display/dsi_phy_28nm_8960 display/dsi_phy_28nm
	display/dsi display/hdmi display/mdp4 display/mdp5 display/mdp_common
	display/sfpb"
log() { echo ">> $*"; }
case $WHAT in
modules|headers|all|--in-buildenv) ;;
*) echo "usage: WORK=... $0 [modules|headers|all]" >&2; exit 2 ;;
esac

# --- inside make.py's buildenv (called below as its BUILDENV_SHELL) ----------
if [ "$WHAT" = --in-buildenv ]; then
	# objects outside the drm-kmod checkout and outside $WORK/obj's kernel
	unset MAKEOBJDIR
	export MAKEOBJDIRPREFIX=$GPU/obj
	mkdir -p "$MAKEOBJDIRPREFIX"
	M=(make -j"$JOBS" SYSDIR="$SRC/sys" KERNBUILDDIR="$KOBJ")
	"${M[@]}" -C "$DRM_KMOD" KMODS="$KMODS"
	for m in $BASE_MODULES; do
		"${M[@]}" -C "$SRC/sys/modules/$m"
	done
	exit 0
fi

# --- drm-kmod with its patches -----------------------------------------------
if [ "$WHAT" = modules ] || [ "$WHAT" = all ]; then
	if [ ! -d "$DRM_KMOD/.git" ]; then
		log "drm-kmod $DRM_KMOD_BRANCH"
		git clone -q -b "$DRM_KMOD_BRANCH" "$DRM_KMOD_URL" "$DRM_KMOD"
	fi
	if [ "$(git -C "$DRM_KMOD" rev-parse HEAD)" != "$DRM_KMOD_COMMIT" ]; then
		git -C "$DRM_KMOD" fetch -q origin "$DRM_KMOD_BRANCH"
		git -C "$DRM_KMOD" checkout -q "$DRM_KMOD_COMMIT"
	fi
	for p in "$REPO"/freebsd/gpu/drm-kmod-patches/*.patch; do
		[ -f "$p" ] || continue
		mark=$DRM_KMOD/.pro1x-applied-$(basename "$p" .patch)
		[ -f "$mark" ] && continue
		log "patch $(basename "$p")"
		git -C "$DRM_KMOD" apply "$p"
		touch "$mark"
	done

	KOBJ=$(find "$WORK/obj" -path '*/arm64.aarch64/sys/PRO1X/kernel' -type f | head -1)
	[ -f "$KOBJ" ] || { echo "no PRO1X kernel in $WORK/obj (freebsd/build.sh)"; exit 1; }
	KOBJ=$(dirname "$KOBJ")
	export KOBJ JOBS SRC GPU DRM_KMOD KMODS BASE_MODULES

	lock=$WORK/.kbuild-lock
	while [ -e "$lock" ]; do
		echo ">> $lock held: $(cat "$lock" 2>/dev/null); waiting"
		sleep 30
	done
	echo "gpu agent (freebsd/gpu/build.sh) $(date -Iseconds)" > "$lock"
	trap 'rm -f "$lock"' EXIT
	log "modules $KMODS $BASE_MODULES"
	[ -n "${CLEAN:-}" ] && rm -rf "$GPU/obj"
	mkdir -p "$GPU"
	# buildenv takes no -j (the make inside does)
	export PATH=$WORK/hostbin:$LLVM_BIN:$PATH MAKEOBJDIRPREFIX=$WORK/obj
	(cd "$SRC" && ./tools/build/make.py --cross-bindir="$LLVM_BIN" \
	    --cross-compiler-type=clang --host-bindir="$LLVM_BIN" \
	    --host-compiler-type=clang TARGET=arm64 TARGET_ARCH=aarch64 \
	    -DNO_WERROR WERROR= KERNCONF=PRO1X buildenv \
	    BUILDENV_SHELL="/bin/bash $REPO/freebsd/gpu/build.sh --in-buildenv") \
	    > "$GPU/build-modules.log" 2>&1 || { tail -40 "$GPU/build-modules.log"; exit 1; }
	rm -f "$lock"
	trap - EXIT

	rm -rf "$OUT/modules"
	mkdir -p "$OUT/modules"
	for m in $KMODS; do
		cp "$GPU/obj$DRM_KMOD/$m/$m.ko" "$OUT/modules/"
	done
	for m in $BASE_MODULES; do
		cp "$GPU/obj$SRC/sys/modules/$m/$m.ko" "$OUT/modules/"
	done
	KLDXREF=$(find "$WORK/obj" -path '*/tmp/legacy/bin/kldxref' -type f | head -1)
	[ -x "$KLDXREF" ] && "$KLDXREF" "$OUT/modules"
	ls -la "$OUT/modules"
fi

# --- the modules' symbols against the kernel and their dependencies ------------
# A module's undefined symbols must be in the kernel or in a module it
# depends on (MODULE_DEPEND); __this_linker_file is the linker's own.
if [ "$WHAT" = modules ] || [ "$WHAT" = all ]; then
	LKPI=$WORK/out/modules/linuxkpi.ko
	if [ ! -f "$LKPI" ]; then
		echo ">> no $LKPI (freebsd/build.sh): symbols not checked"
	else
		T=$(mktemp -d)
		def() { "$LLVM_BIN/llvm-nm" -D --defined-only "$1" | awk '{print $3}'; }
		def "$KOBJ/kernel" > "$T/kernel"
		def "$LKPI" > "$T/linuxkpi"
		for m in $KMODS $BASE_MODULES; do def "$OUT/modules/$m.ko" > "$T/$m"; done
		missing=0
		for m in "dmabuf linuxkpi" "linuxkpi_video linuxkpi" \
		    "lindebugfs linuxkpi" "drm linuxkpi linuxkpi_video dmabuf lindebugfs"; do
			set -- $m
			ko=$1
			deps=("$T/kernel" "$T/$ko")
			shift
			for d in "$@"; do deps+=("$T/$d"); done
			sort -u "${deps[@]}" > "$T/avail"
			"$LLVM_BIN/llvm-nm" -D -u "$OUT/modules/$ko.ko" | awk '$1 == "U" {print $2}' |
			    grep -vx __this_linker_file | sort -u > "$T/undef"
			if [ -s "$T/undef" ] && comm -23 "$T/undef" "$T/avail" | grep -q .; then
				echo ">> $ko.ko: undefined, in neither the kernel nor $*:"
				comm -23 "$T/undef" "$T/avail"
				missing=1
			fi
		done
		rm -rf "$T"
		[ "$missing" = 0 ] && log "all symbols of $KMODS $BASE_MODULES resolved"
	fi
fi

# --- Linux v6.12: the msm driver's sources ---------------------------------------
if [ "$WHAT" = headers ] || [ "$WHAT" = all ]; then
	if [ ! -f "$LINUX/.sha256" ]; then
		log "Linux v6.12 (msm and the GPU clocks) from $LINUX_URL"
		rm -rf "$LINUX.new"
		mkdir -p "$LINUX.new"
		members=()
		for p in $LINUX_PATHS; do members+=("linux-6.12/$p"); done
		# the tarball (140 MB) only streams through: tar keeps the paths,
		# sha256sum checks all of it
		mkfifo "$LINUX.new/.stream"
		tar -xJf "$LINUX.new/.stream" -C "$LINUX.new" --strip-components=1 \
		    "${members[@]}" &
		tarpid=$!
		curl -sSfL "$LINUX_URL" | tee "$LINUX.new/.stream" | sha256sum |
		    cut -d' ' -f1 > "$LINUX.new/.sum"
		wait "$tarpid"
		rm -f "$LINUX.new/.stream"
		[ "$(cat "$LINUX.new/.sum")" = "$LINUX_SHA256" ] ||
			{ echo "linux-6.12.tar.xz: SHA-256 $(cat "$LINUX.new/.sum")"; exit 1; }
		mv "$LINUX.new/.sum" "$LINUX.new/.sha256"
		rm -rf "$LINUX"
		mv "$LINUX.new" "$LINUX"
	fi
	# the register headers, as the msm Makefile's headergen makes them:
	# run from the tree's top as kbuild does, flat in generated/ (msm's
	# include path), validated against rules-fd.xsd when python3 has lxml
	# (the headers name their XML by absolute path: made again after a move)
	R=drivers/gpu/drm/msm/registers
	val=--no-validate
	python3 -I -c 'import lxml' 2> /dev/null && val=--validate
	log "msm register headers into $LINUX/generated ($val)"
	rm -rf "$LINUX/generated"
	mkdir -p "$LINUX/generated"
	for x in $MSM_XML; do
		(cd "$LINUX" && python3 -I "$R/gen_header.py" $val --rnn "$R" \
		    --xml "$R/$x.xml" c-defines > "generated/${x#*/}.xml.h")
	done
	ls -la "$LINUX/generated"
fi
log "done"
