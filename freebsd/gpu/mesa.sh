#!/bin/bash
# Mesa for the Adreno 610 (step "GPU 3", freebsd/gpu/README.md): Mesa 26.2.2,
# the version of the release's mesa-libs and mesa-dri packages (its SHA-256
# from graphics/mesa-dri/distinfo, ports commit 29df97d33d31 on 2026Q4), with
# the Gallium driver freedreno (OpenGL, GLES; msm_dri.so, kgsl_dri.so) and
# the Vulkan driver turnip (libvulkan_freedreno.so) next to llvmpipe and
# softpipe, which stay the fallback where there is no GPU. The packages have
# no freedreno; the image's libdrm 2.4.134 serves as it is (Mesa's freedreno
# talks to the kernel through its own src/freedreno/drm, not libdrm_freedreno).
# mesa-patches/: two of the port's FreeBSD patches (graphics/mesa-dri/files)
# and two for freedreno, which the port does not build (ENODATA, SYS_gettid).
#
# Cross-built on the Linux PC: clang for aarch64-unknown-freebsd15.1 against
# freebsd/hildon/build.sh's sysroot ($WORK/hildon/sysroot), to which this adds
# the packages Mesa builds on (MESA_PKGS, as build.sh adds its own) and, of
# FreeBSD's llvm19 package, LLVM's headers and libLLVM (llvmpipe's JIT; the
# image has the same libLLVM, rootfs.sh). meson asks LLVM's llvm-config,
# a FreeBSD program: hostbin/llvm-config gives the package's answers instead
# (from its LLVMConfig.cmake), with paths in the sysroot.
#
# Installed into $WORK/out/gpu/mesa, laid out as the two packages are
# (usr/local/lib, lib/dri, lib/gbm, libdata/pkgconfig, share/glvnd, ...):
# copied over the image's files it replaces both packages' libraries
# (libgallium-26.2.2.so, libEGL_mesa, libGLX_mesa, libgbm, dri_gbm.so, the
# DRI stubs); their other drivers' DRI stubs then point at a libgallium
# without those drivers and fail cleanly; lavapipe and RADV are separate
# libraries and keep working. The libraries find libLLVM through the
# package's run path (/usr/local/llvm19/lib). The build tree
# ($WORK/gpu/mesa) is deleted at the end (KEEP=1 keeps it).
#
#   WORK=/path/to/freebsd-build bash freebsd/gpu/mesa.sh
#   WORK=... VM="ssh ... root@host" bash freebsd/gpu/mesa.sh test
#
# "test" runs test/egl-pbuffer.c in a FreeBSD/arm64 machine (vm/vm.sh) on
# the staged libraries: GLES 2 on llvmpipe and on softpipe through EGL
# (surfaceless, a pbuffer, glReadPixels), and freedreno loaded on a file
# that is no GPU (it has to fail, not crash).
#
# Needs freebsd/hildon/build.sh's sysroot and rootfs.sh's package index
# (both once); on the PC clang and lld, meson >= 1.4, ninja, python3 with
# mako, pyyaml and packaging, bison, flex, wayland-scanner, glslangValidator,
# bsdtar. About 550 MB in $WORK/gpu/mesa while it builds; JOBS=<n> (default:
# all CPUs).
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
WORK=${WORK:?set WORK=<build directory>}
WHAT=${1:-build}
JOBS=${JOBS:-$(nproc)}
VER=26.2.2
SUM=eeb29ca7e56cfaa8e8a79538dcf834e3b18e501c31bef5145e959ea437cc4216
TAR=$WORK/dl/mesa-$VER.tar.xz
SYSROOT=$WORK/hildon/sysroot
G=$WORK/gpu/mesa
OUT=$WORK/out/gpu/mesa
TARGET=aarch64-unknown-freebsd15.1
ABI=FreeBSD:15:aarch64
PKGURL=${PKGURL:-https://pkg.freebsd.org/$ABI/quarterly}
# What Mesa builds on, beyond the base system (Mesa's meson.build for the
# options below; the packages' dependencies; turnip's window systems also
# libudev and xcb-keysyms)
MESA_PKGS="libdrm expat zstd libX11 libxcb libXext libXdamage libXfixes
	libXrandr libXxf86vm libxshmfence xorgproto libglvnd libdisplay-info
	wayland wayland-protocols libudev-devd xcb-util-keysyms"
MESA_PRUNE="llvm* spirv-llvm-translator* python3* py3*-* perl5*"
# The drivers and interfaces; the rest as the packages' ports have them
# (graphics/mesa-dri/Makefile.common, mesa-libs/Makefile)
MESON_OPTS=(
	-Dgallium-drivers=freedreno,llvmpipe,softpipe
	-Dvulkan-drivers=freedreno
	-Dfreedreno-kmds=msm
	-Dplatforms=x11,wayland
	-Degl=enabled -Dgbm=enabled -Dglx=dri -Dglvnd=enabled
	-Dgles1=enabled -Dgles2=enabled
	-Dllvm=enabled -Dshared-llvm=enabled
	-Dzstd=enabled -Dexpat=enabled
	-Dgallium-va=disabled -Dgallium-mediafoundation=disabled
	-Dlibunwind=disabled -Dlmsensors=disabled -Dvalgrind=disabled
	-Dandroid-libbacktrace=disabled -Dmicrosoft-clc=disabled
	-Dbuild-tests=false -Dtools=
)
log() { echo ">> $*"; }

# --- test: the staged libraries in a FreeBSD machine ----------------------
if [ "$WHAT" = test ]; then
	VM=${VM:?set VM=<ssh command for a FreeBSD/arm64 machine, as root>}
	[ -f "$OUT/usr/local/lib/libgallium-$VER.so" ] || { echo "build first"; exit 1; }
	T=$WORK/gpu/mesa-test
	rm -rf "$T"
	mkdir -p "$T"
	# against glvnd's libEGL and libGLESv2 (the image's), Mesa's libgbm
	clang --target=$TARGET --sysroot="$SYSROOT" -fuse-ld=lld -O2 -Wall \
	    -isystem "$SYSROOT/usr/local/include" -L"$SYSROOT/usr/local/lib" \
	    -Wl,-rpath-link,"$SYSROOT/usr/local/lib" \
	    -o "$T/egl-pbuffer" "$REPO/freebsd/gpu/test/egl-pbuffer.c" \
	    -lEGL -lGLESv2 -lgbm
	log "staged libraries to the machine (/tmp/mesa-stage)"
	$VM 'rm -rf /tmp/mesa-stage && mkdir -p /tmp/mesa-stage'
	bsdtar -cf - -C "$OUT" usr | $VM 'tar -xf - -C /tmp/mesa-stage'
	$VM 'cat > /tmp/mesa-stage/egl-pbuffer && chmod 755 /tmp/mesa-stage/egl-pbuffer' < "$T/egl-pbuffer"
	$VM sh -s <<'REMOTE'
set -e
S=/tmp/mesa-stage
L=$S/usr/local/lib
export LD_LIBRARY_PATH=$L GBM_BACKENDS_PATH=$L/gbm
export __EGL_VENDOR_LIBRARY_FILENAMES=$S/usr/local/share/glvnd/egl_vendor.d/50_mesa.json
echo "-- what the staged libraries need"
ldd $L/libgallium-*.so $L/libEGL_mesa.so.0 $L/libvulkan_freedreno.so | grep -E 'not found|LLVM|gallium|:$' || true
echo "-- llvmpipe"
$S/egl-pbuffer
echo "-- softpipe"
GALLIUM_DRIVER=softpipe $S/egl-pbuffer
echo "-- freedreno on a file that is no GPU"
MESA_LOADER_DRIVER_OVERRIDE=msm $S/egl-pbuffer gbm /dev/zero
REMOTE
	rm -rf "$T"
	log "test done"
	exit 0
fi

# --- sysroot ---------------------------------------------------------------
[ -f "$SYSROOT/.base" ] || { echo "no sysroot: run freebsd/hildon/build.sh once"; exit 1; }
[ -f "$WORK/pkgs/site/packagesite.yaml" ] ||
	{ echo "run rootfs.sh once first (package index)"; exit 1; }
x=()
for p in $MESA_PRUNE; do x+=(-x "$p"); done
# shellcheck disable=SC2086
python3 -I "$REPO/freebsd/pkgfetch.py" "${x[@]}" "$WORK/pkgs/site" \
    "$WORK/pkgs/cache" "$PKGURL" $MESA_PKGS > "$G.want"
while read -r f; do
	grep -qxF "$f" "$WORK/hildon/sysroot.list" && continue
	log "sysroot: $(basename "$f")"
	bsdtar -xf "$f" -C "$SYSROOT" --exclude '+*' 2> /dev/null
	echo "$f" >> "$WORK/hildon/sysroot.list"
done < "$G.want"
rm -f "$G.want"
# LLVM: headers, libLLVM and its CMake description, not the 1.8 GB of
# the package (clang, static libraries); in sysroot.list under its own
# name, so that the package counts as not unpacked for build.sh
f=$(python3 -I "$REPO/freebsd/pkgfetch.py" -n "$WORK/pkgs/site" \
    "$WORK/pkgs/cache" "$PKGURL" llvm19)
LLVMMARK="$f (headers, libLLVM: gpu/mesa.sh)"
if ! grep -qxF "$LLVMMARK" "$WORK/hildon/sysroot.list"; then
	log "sysroot: LLVM of $(basename "$f")"
	bsdtar -xf "$f" -C "$SYSROOT" '/usr/local/llvm19/include/llvm' \
	    '/usr/local/llvm19/include/llvm-c' '/usr/local/llvm19/lib/libLLVM*.so*' \
	    '/usr/local/llvm19/lib/cmake/llvm' 2> /dev/null
	echo "$LLVMMARK" >> "$WORK/hildon/sysroot.list"
fi
LL=$SYSROOT/usr/local/llvm19
[ -e "$LL/lib/libLLVM-19.so" ] && [ -f "$LL/lib/cmake/llvm/LLVMConfig.cmake" ] ||
	{ echo "$LL: LLVM incomplete"; exit 1; }

# --- host tools ----------------------------------------------------------
rm -rf "$G"
mkdir -p "$G/hostbin" "$WORK/dl"
# pkg-config with the sysroot for -I and -L, without it for variables (paths
# on the phone), as freebsd/hildon/build.sh's; but pkgdatadir in the
# sysroot: wayland-protocols' XML files, which the build reads
cat > "$G/hostbin/pkg-config" <<'SHIM'
#!/bin/sh
case " $* " in
*" --variable=pkgdatadir "*)
	out=$(PKG_CONFIG_SYSROOT_DIR= /usr/bin/pkg-config "$@") || exit
	printf '%s%s\n' "$PKG_CONFIG_SYSROOT_DIR" "$out"
	exit 0
	;;
*" --variable"*)
	s=$PKG_CONFIG_SYSROOT_DIR
	out=$(PKG_CONFIG_SYSROOT_DIR= /usr/bin/pkg-config "$@") || exit
	[ -n "$s" ] && out=$(printf '%s\n' "$out" | sed "s|$s||g")
	printf '%s\n' "$out"
	exit 0
	;;
esac
exec /usr/bin/pkg-config "$@"
SHIM
# llvm-config: what the package's says (llvm-config19 in a FreeBSD machine:
# shared mode, -lLLVM-19, no system libraries), from LLVMConfig.cmake
cm=$LL/lib/cmake/llvm/LLVMConfig.cmake
cmv() { sed -n "s/^set($1 \(.*\))$/\1/p" "$cm"; }
lver=$(cmv LLVM_PACKAGE_VERSION)
lmaj=${lver%%.*}
rtti=$( [ "$(cmv LLVM_ENABLE_RTTI)" = ON ] && echo YES || echo NO)
targets=$(cmv LLVM_TARGETS_TO_BUILD | tr ';' ' ')
comps=$( { echo all all-targets engine native nativecodegen $targets
	cmv LLVM_AVAILABLE_LIBS | tr ';' '\n' | sed -n 's/^LLVM//p'; } |
	tr ' ' '\n' | tr 'A-Z' 'a-z' | sort -u | tr '\n' ' ')
htriple=$(sed -n 's/^#define LLVM_HOST_TRIPLE "\(.*\)"/\1/p' "$LL/include/llvm/Config/llvm-config.h")
cat > "$G/hostbin/llvm-config" <<SHIM
#!/bin/sh
# FreeBSD's llvm$lmaj package (LLVM $lver) as installed in $SYSROOT
L=$LL
for a; do
	case \$a in
	--version) echo $lver ;;
	--shared-mode) echo shared ;;
	--link-shared) ;;
	--link-static) echo "static libraries not in the sysroot" >&2; exit 1 ;;
	--has-rtti) echo $rtti ;;
	--assertion-mode) echo OFF ;;
	--build-mode) echo Release ;;
	--prefix) echo \$L ;;
	--bindir) echo \$L/bin ;;
	--includedir) echo \$L/include ;;
	--libdir) echo \$L/lib ;;
	--cppflags|--cflags) echo "-I\$L/include -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS" ;;
	--cxxflags) echo "-I\$L/include -std=c++17 -fno-exceptions -funwind-tables -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS" ;;
	--ldflags) echo "-L\$L/lib" ;;
	--libs) echo "-lLLVM-$lmaj" ;;
	--libnames) echo "libLLVM-$lmaj.so" ;;
	--libfiles) echo "\$L/lib/libLLVM-$lmaj.so" ;;
	--system-libs) echo ;;
	--components) echo "$comps" ;;
	--targets-built) echo "$targets" ;;
	--host-target) echo $htriple ;;
	-*) echo "llvm-config (sysroot): \$a unknown" >&2; exit 1 ;;
	esac
done
SHIM
chmod 755 "$G/hostbin/pkg-config" "$G/hostbin/llvm-config"

# --- source ------------------------------------------------------------------
if [ ! -f "$TAR" ]; then
	log "source"
	curl -sSfL -o "$TAR.new" "https://archive.mesa3d.org/mesa-$VER.tar.xz"
	mv "$TAR.new" "$TAR"
fi
echo "$SUM  $TAR" | sha256sum -c --quiet - || { echo "$TAR: not the pinned tarball"; exit 1; }
mkdir -p "$G/src"
bsdtar -xf "$TAR" -C "$G/src" --strip-components 1
for p in "$REPO"/freebsd/gpu/mesa-patches/patch-*; do
	patch -s -p0 -d "$G/src" < "$p"
done

# --- build ---------------------------------------------------------------------
meson_list() {
	local w out=
	for w in "$@"; do out="$out${out:+, }'$w'"; done
	printf '%s' "$out"
}
# -O2 as the ports build; libLLVM through the package's run path
CFL=(-O2 -pipe)
LDF=(-fuse-ld=lld -Wl,--undefined-version -Wl,-rpath=/usr/local/llvm$lmaj/lib
	-L"$SYSROOT/usr/local/lib" -Wl,-rpath-link,"$SYSROOT/usr/local/lib"
	-Wl,-rpath-link,"$SYSROOT/usr/lib" -Wl,-rpath-link,"$SYSROOT/lib"
	-Wl,-rpath-link,"$LL/lib")
cat > "$G/cross.ini" <<EOF
[binaries]
c = ['clang', '--target=$TARGET', '--sysroot=$SYSROOT']
cpp = ['clang++', '--target=$TARGET', '--sysroot=$SYSROOT']
ar = 'llvm-ar'
strip = 'llvm-strip'
nm = 'llvm-nm'
objcopy = 'llvm-objcopy'
pkg-config = '$G/hostbin/pkg-config'
llvm-config = '$G/hostbin/llvm-config'

[properties]
sys_root = '$SYSROOT'
pkg_config_libdir = ['$SYSROOT/usr/local/libdata/pkgconfig', '$SYSROOT/usr/local/lib/pkgconfig', '$SYSROOT/usr/libdata/pkgconfig', '$SYSROOT/usr/local/share/pkgconfig']

[host_machine]
system = 'freebsd'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[built-in options]
c_args = [$(meson_list "${CFL[@]}")]
cpp_args = [$(meson_list "${CFL[@]}")]
c_link_args = [$(meson_list "${LDF[@]}")]
cpp_link_args = [$(meson_list "${LDF[@]}")]
EOF
log "meson (Mesa $VER: freedreno, turnip, llvmpipe, softpipe)"
# Not the shell's pkg-config settings: those of the build machine's tools
# (wayland-scanner) are the PC's. No subprojects from the net
# (nofallback): what is not in the sysroot stays out (freedreno's
# command-stream decoder wanted libarchive, a tool's)
env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_SYSROOT_DIR \
    meson setup "$G/build" "$G/src" --cross-file "$G/cross.ini" \
    --wrap-mode=nofallback \
    --prefix=/usr/local --libdir=lib --buildtype=plain -Db_ndebug=true \
    -Dstrip=true -Dwerror=false "${MESON_OPTS[@]}" > "$G/meson.out" 2>&1 ||
	{ tail -40 "$G/meson.out"; exit 1; }
sed -n '/^mesa /,$p' "$G/meson.out" | head -80
log "ninja (-j$JOBS)"
ninja -C "$G/build" -j"$JOBS" > "$G/make.out" 2>&1 ||
	{ grep -m20 -E "error:|FAILED" "$G/make.out"; tail -20 "$G/make.out"; exit 1; }

# --- stage -------------------------------------------------------------------
log "install into $OUT"
rm -rf "$OUT.new"
DESTDIR="$OUT.new" meson install -C "$G/build" --no-rebuild > "$G/install.out" 2>&1 ||
	{ tail -20 "$G/install.out"; exit 1; }
L=$OUT.new/usr/local
# pkg-config files where FreeBSD keeps them (the ports' pathfix)
if [ -d "$L/lib/pkgconfig" ]; then
	mkdir -p "$L/libdata/pkgconfig"
	mv "$L/lib/pkgconfig/"* "$L/libdata/pkgconfig/"
	rmdir "$L/lib/pkgconfig"
fi
# mesa-libs' fallback for GLX without GLX_EXT_libglvnd
mkdir -p "$L/etc/libmap.d"
echo "libGLX_indirect.so.0 libGLX_mesa.so.0" > "$L/etc/libmap.d/mesa.conf"

# Every object FreeBSD's and arm64's, and what it needs there (the stage,
# the sysroot: the base system and the packages the image has)
log "check: ELF type and NEEDED"
bad=0
while IFS= read -r -d '' o; do
	h=$(llvm-readelf -h "$o" 2>/dev/null) || continue
	if ! grep -q 'OS/ABI:.*FreeBSD' <<< "$h" || ! grep -q 'Machine:.*AArch64' <<< "$h"; then
		echo "not FreeBSD/arm64: ${o#$OUT.new}"
		bad=1
	fi
	for n in $(llvm-readelf -d "$o" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
		found=
		for d in "$L/lib" "$SYSROOT/lib" "$SYSROOT/usr/lib" "$SYSROOT/usr/local/lib" "$LL/lib"; do
			if [ -e "$d/$n" ]; then found=1; break; fi
		done
		[ -n "$found" ] || { echo "${o#$OUT.new}: $n not found"; bad=1; }
	done
done < <(find "$OUT.new" -type f \( -name '*.so*' -o -perm -u+x \) -print0)
[ "$bad" = 0 ] || { echo "check failed (the stage stays in $OUT.new)"; exit 1; }
rm -rf "$OUT"
mv "$OUT.new" "$OUT"
(cd "$OUT" && find . ! -type d | sort)
du -sh "$OUT"
if [ -z "${KEEP:-}" ]; then
	rm -rf "$G"
fi
log "done: $OUT over the image's mesa-libs and mesa-dri files"
