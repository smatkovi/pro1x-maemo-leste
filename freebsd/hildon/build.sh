#!/bin/bash
# Builds Maemo Leste's Hildon desktop for FreeBSD/arm64 on a Linux PC, by
# cross-compiling: clang for aarch64-unknown-freebsd15.0 against a sysroot
# made of the release's base system and FreeBSD's packages (the libraries
# Hildon builds on: gtk2, gconf2, dbus-glib, ...). The Hildon parts come
# from github.com/maemo-leste, with FreeBSD changes in patches/<part>/
# and the HiDPI changes Leste gets too in ../../patches/hidpi/<part>.patch.
#
#   WORK=/path/to/freebsd-build bash freebsd/hildon/build.sh [part...]
#
# Parts: the desktop (PARTS below) and the apps (apps.sh), each at a
# pinned revision; "apps" builds all apps, "data" Leste's theme, icons
# and sounds. Each part is installed into $WORK/hildon/pkgroot/<part>
# (package.sh makes FreeBSD packages of them), and from there into
# $WORK/hildon/stage (under /usr/local), which rootfs.sh puts into the
# root image, and into the sysroot, for the parts built after it. An app
# that fails is reported at the end; a desktop part stops the build.
# BUILD=<directory> puts the build trees elsewhere (QtWebKit's needs some
# 6 GB).
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
WORK=${WORK:?set WORK=<build directory>}
H=$WORK/hildon
SRC=$H/src
SYSROOT=$H/sysroot
STAGE=$H/stage
BUILD=${BUILD:-$H/build}
TARGET=aarch64-unknown-freebsd15.0
ABI=FreeBSD:15:aarch64
PKGURL=${PKGURL:-https://pkg.freebsd.org/$ABI/quarterly}
JOBS=${JOBS:-$(nproc)}
log() { echo ">> $*"; }

# What the parts build on (and the root image needs at run time)
. "$REPO/freebsd/hildon/deps.sh"

# --- sysroot -----------------------------------------------------------
# Base system once; packages as HILDON_PKGS asks for them. A package is
# unpacked only once: the parts built here replace some (gtk2).
if [ ! -f "$SYSROOT/.base" ]; then
	log "sysroot: base system"
	rm -rf "$SYSROOT"
	mkdir -p "$SYSROOT"
	bsdtar -xf "$WORK/dl/base.txz" -C "$SYSROOT" ./usr/include ./usr/lib \
	    ./lib ./usr/libdata ./usr/share/keys
	: > "$H/sysroot.list"
	touch "$SYSROOT/.base"
fi
[ -f "$WORK/pkgs/site/packagesite.yaml" ] ||
	{ echo "run rootfs.sh once first (package index)"; exit 1; }
x=()
for p in $HILDON_PRUNE; do x+=(-x "$p"); done
python3 -I "$REPO/freebsd/pkgfetch.py" "${x[@]}" "$WORK/pkgs/site" \
    "$WORK/pkgs/cache" "$PKGURL" $HILDON_PKGS $APPS_PKGS $HARMATTAN_PKGS > "$H/sysroot.want"
while read -r f; do
	grep -qxF "$f" "$H/sysroot.list" && continue
	log "sysroot: $(basename "$f")"
	bsdtar -xf "$f" -C "$SYSROOT" --exclude '+*' 2> /dev/null
	echo "$f" >> "$H/sysroot.list"
done < "$H/sysroot.want"

# --- host tools ----------------------------------------------------------
# Many configure.ac files take their version from debian/changelog.
mkdir -p "$H/hostbin"
cat > "$H/hostbin/dpkg-parsechangelog" <<'SHIM'
#!/bin/sh
# dpkg-parsechangelog, what configure.ac files ask of it
f=debian/changelog field=
while [ $# -gt 0 ]; do
	case "$1" in
	-l) f=$2; shift ;;
	-l*) f=${1#-l} ;;
	-S|--show-field) field=$2; shift ;;
	-S*) field=${1#-S} ;;
	esac
	shift
done
v=$(sed -n '1s/^[^(]*(\([^)]*\)).*/\1/p' "$f")
src=$(sed -n '1s/^\([^ ]*\) .*/\1/p' "$f")
case "$field" in
Version) echo "$v" ;;
Source) echo "$src" ;;
*) echo "Source: $src"; echo "Version: $v" ;;
esac
SHIM
chmod 755 "$H/hostbin/dpkg-parsechangelog"
# pkg-config with the sysroot (PKG_CONFIG_SYSROOT_DIR) for -I and -L,
# without it for variables: those are paths on the phone (plugin and data
# directories that configure scripts compile in or install to)
cat > "$H/hostbin/pkg-config" <<'SHIM'
#!/bin/sh
case " $* " in
*" --variable"*)
	# .pc files relative to themselves (${pcfiledir}) name the sysroot too
	s=$PKG_CONFIG_SYSROOT_DIR
	out=$(PKG_CONFIG_SYSROOT_DIR= /usr/bin/pkg-config "$@") || exit
	[ -n "$s" ] && out=$(printf '%s\n' "$out" | sed "s|$s||g")
	printf '%s\n' "$out"
	exit 0
	;;
esac
exec /usr/bin/pkg-config "$@"
SHIM
chmod 755 "$H/hostbin/pkg-config"
export PATH=$H/hostbin:$PATH
# Autoconf macros of the packages built on (gconf-2.m4, ...)
export ACLOCAL_PATH=$H/sysroot/usr/local/share/aclocal
# GConf schemas are registered on the phone, not into the build tree
export GCONF_DISABLE_MAKEFILE_SCHEMA_INSTALL=1

# --- cross environment -------------------------------------------------
export CC="clang --target=$TARGET --sysroot=$SYSROOT"
export CXX="clang++ --target=$TARGET --sysroot=$SYSROOT"
export CPP="$CC -E"
export AR=llvm-ar RANLIB=llvm-ranlib STRIP=llvm-strip NM=llvm-nm
export CPPFLAGS="-isystem $SYSROOT/usr/local/include -idirafter $REPO/freebsd/hildon/compat/include -include errno.h -include $REPO/freebsd/hildon/compat/glibc-compat.h"
# Old code: C17 (autoconf picks C23), pointer-type mismatches not fatal
# The build's paths stay out of the files (__FILE__ in assertions)
export CPPFLAGS="$CPPFLAGS -ffile-prefix-map=$SYSROOT= -ffile-prefix-map=$BUILD/=/usr/src/"
export CFLAGS="-O2 -pipe -std=gnu17 -Wno-error -Wno-incompatible-pointer-types -Wno-incompatible-function-pointer-types -Wno-int-conversion -Wno-error=implicit-function-declaration -Wno-implicit-int"
export CXXFLAGS="-O2 -pipe"
# lld: version scripts may name symbols a library does not have (GNU ld
# allows it; libtool lists the linker's own _end and friends)
export LDFLAGS="-fuse-ld=lld -Wl,--undefined-version -L$SYSROOT/usr/local/lib -Wl,-rpath-link,$SYSROOT/usr/local/lib -Wl,-rpath-link,$SYSROOT/usr/lib -Wl,-rpath-link,$SYSROOT/lib"
export PKG_CONFIG_LIBDIR=$SYSROOT/usr/local/lib/pkgconfig:$SYSROOT/usr/local/libdata/pkgconfig:$SYSROOT/usr/libdata/pkgconfig:$SYSROOT/usr/local/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$SYSROOT
export PKG_CONFIG_PATH=
# glib's code generators from the host: pkg-config names the sysroot's,
# which are FreeBSD's (scripts for its Python)
HOSTTOOLS=(GLIB_MKENUMS=/usr/bin/glib-mkenums GLIB_GENMARSHAL=/usr/bin/glib-genmarshal
	GLIB_COMPILE_RESOURCES=/usr/bin/glib-compile-resources
	GLIB_COMPILE_SCHEMAS=/usr/bin/glib-compile-schemas
	GDBUS_CODEGEN=/usr/bin/gdbus-codegen GDK_PIXBUF_CSOURCE=/usr/bin/gdk-pixbuf-csource
	GTK_UPDATE_ICON_CACHE=/usr/bin/gtk-update-icon-cache)
export "${HOSTTOOLS[@]}"

# --- the parts -----------------------------------------------------------
# name|extra configure arguments[|extra CPPFLAGS, as Leste's debian/rules]
PARTS=(
	"clutter-0.8|--with-flavour=eglx --with-gles=2.0 --with-imagebackend=gdk-pixbuf"
	"mce-dev|!"
	"gtk|--with-xinput=yes --disable-introspection --disable-cups --disable-papi --without-libjasper"
	"sapwood|"
	"libmatchbox2|--enable-static --enable-png-theme --enable-xcomposite --with-pango"
	"libhildon|--disable-tests --disable-examples"
	"libosso|"
	"iphbd|"
	"libhildondesktop|"
	"libhildonmime|"
	"profiled|!"
	"osso-bookmark-engine|!"
	"hildon-thumbnail|--disable-gstreamer"
	"libhildonfm|"
	"hildon-status-menu|"
	"hildon-home|"
	"hildon-desktop|--enable-static-mb2=yes MB2_STATIC_LIB=$SYSROOT/usr/local/lib/libmatchbox2-0.1.a|-DG_DEBUG_DISABLE -DG_DISABLE_CHECKS"
)

# The revisions built (fetch_src); URL where a part is not maemo-leste's,
# SPARSE where it is some directories of a big repository
declare -A URL SPARSE REV=(
	[clutter-0.8]=ead56341b457 [mce-dev]=d3260521eaf1 [gtk]=bf9a7dfb8f69
	[sapwood]=6849cd690ae2 [libmatchbox2]=208c59d7a3ba [libhildon]=78c268e51200
	[libosso]=ebd2f1804c29 [iphbd]=bad0de1ff36d [libhildondesktop]=d05587180c24
	[libhildonmime]=bfedaf83ff01 [profiled]=8e16f3dc9d42
	[osso-bookmark-engine]=0a8ff94e3cfe [hildon-thumbnail]=86bd10f3f620
	[libhildonfm]=6df73d0c97f2 [hildon-status-menu]=796dbd7ca973
	[hildon-home]=8dd16c59f867 [hildon-desktop]=7f8ce2104a48
)
# mce's D-Bus names from mce itself: the mce-dev repository stopped in
# 2021, before the display's brightness and timeout calls
URL[mce-dev]=https://github.com/maemo-leste/mce.git
# The apps: APPS, their REV and URL
. "$REPO/freebsd/hildon/apps.sh"
. "$REPO/freebsd/hildon/harmattan.sh"

# Parts without autotools: "name|!" builds with custom_<name> in the tree
custom_profiled() {
	local m=(PREFIX=/usr/local ETCDIR=/usr/local/etc/profiled
	    PKGCFGDIR=/usr/local/libdata/pkgconfig "${HOSTTOOLS[@]}")
	make -j"$JOBS" "${m[@]}" LDLIBS="$(pkg-config --libs dbus-glib-1 dbus-1 glib-2.0) -linotify" \
	    libprofile.so.0 libprofile.a profiled profile.pc profile-data.pc > make.out 2>&1 ||
	    { grep -m20 "error" make.out; exit 1; }
	make "${m[@]}" ROOT="$PR" install-libprofile install-libprofile-dev \
	    install-profiled install-profile-data install-profile-data-dev \
	    > install.out 2>&1 || { tail install.out; exit 1; }
}

custom_mce_dev() {
	local v
	v=$(sed -n '1s/^[^(]*(\([^)]*\)).*/\1/p' debian/changelog)
	install -d "$PR/usr/local/include/mce" "$PR/usr/local/libdata/pkgconfig"
	install -m 644 src/include/mce/*.h "$PR/usr/local/include/mce/"
	printf 'prefix=/usr/local\nincludedir=${prefix}/include\n\nName: mce\nDescription: Mode Control Entity\nVersion: %s\nCflags: -I${includedir}\n' \
	    "$v" > "$PR/usr/local/libdata/pkgconfig/mce.pc"
}

# One source file; the Makefile goes through the host's libtool, which
# links with the host's compiler
custom_osso_bookmark_engine() {
	local deps="glib-2.0 gconf-2.0 libxml-2.0 gio-2.0" d
	# shellcheck disable=SC2046
	$CC $CPPFLAGS $CFLAGS -fPIC -shared $LDFLAGS \
	    -Wl,-soname,libbookmarkengine.so.0 -o libbookmarkengine.so.0 \
	    $(pkg-config --cflags $deps) src/bookmark_parser.c \
	    $(pkg-config --libs $deps) > make.out 2>&1 ||
	    { grep -m20 "error" make.out; exit 1; }
	sed 's|^prefix=.*|prefix=/usr/local|' osso-bookmark-engine.pc > pc.out
	install -D -m 755 libbookmarkengine.so.0 \
	    "$PR/usr/local/lib/libbookmarkengine.so.0"
	ln -sf libbookmarkengine.so.0 "$PR/usr/local/lib/libbookmarkengine.so"
	install -D -m 644 src/osso_bookmark_parser.h \
	    "$PR/usr/local/include/osso_bookmark_parser.h"
	install -D -m 644 pc.out \
	    "$PR/usr/local/libdata/pkgconfig/osso-bookmark-engine.pc"
}

# The index of one of FreeBSD's package sets ($WORK/pkgs-<set>/site), signed
# with a key the base system trusts, as rootfs.sh checks quarterly's
pkg_site() {
	local d=$WORK/pkgs-$1 trusted
	[ -f "$d/site/packagesite.yaml" ] && return 0
	mkdir -p "$d/site"
	curl -sSfL -o "$d/packagesite.pkg" "https://pkg.freebsd.org/$ABI/$1/packagesite.pkg"
	bsdtar -xf "$d/packagesite.pkg" -C "$d/site"
	trusted=$(sed -n 's/^fingerprint: "\(.*\)"/\1/p' \
	    "$SYSROOT"/usr/share/keys/pkg/trusted/pkg.freebsd.org.*)
	echo "$trusted" | grep -qx "$(sha256sum < "$d/site/packagesite.yaml.pub" | cut -d' ' -f1)" &&
	    sha256sum < "$d/site/packagesite.yaml" | cut -d' ' -f1 | tr -d '\n' |
	    openssl dgst -sha256 -verify "$d/site/packagesite.yaml.pub" \
	    -signature "$d/site/packagesite.yaml.sig" > /dev/null ||
	    { rm -rf "$d/site"; echo "$1: package index does not verify"; return 1; }
}

# The source at its pinned revision (REV), from URL or maemo-leste. URL
# may also be a release tarball (REV: its SHA-256), unpacked to
# $SRC/<part>, or "pkg:<set>", FreeBSD's package of that name from one of
# its package sets (REV: its SHA-256, as that set's index has it), for a
# part FreeBSD no longer builds for quarterly (kept as $SRC/<part>.pkg)
fetch_src() {
	local name=$1 url=${URL[$1]:-https://github.com/maemo-leste/$1.git}
	# a part of harmattan-qt5: its directory of the one checkout
	if [ "${kind:-}" = harmattan ]; then
		local d
		[ -d "$SRC/harmattan-qt5/.git" ] || git clone -q "$HQ5_URL" "$SRC/harmattan-qt5"
		git -C "$SRC/harmattan-qt5" cat-file -e "$HQ5_REV^{commit}" 2> /dev/null ||
		    git -C "$SRC/harmattan-qt5" fetch -q origin
		if [ "$(cat "$SRC/$name/.hq5rev" 2> /dev/null)" != "$HQ5_REV" ]; then
			git -C "$SRC/harmattan-qt5" checkout -q -f "$HQ5_REV"
			rm -rf "${SRC:?}/$name"
			# (the applications are below apps/)
			d=${name#harmattan-}
			[ -d "$SRC/harmattan-qt5/$d" ] || d=apps/$d
			cp -a "$SRC/harmattan-qt5/$d" "$SRC/$name"
			echo "$HQ5_REV" > "$SRC/$name/.hq5rev"
		fi
		return
	fi
	local rev=${REV[$1]:?no pinned revision for $1} f
	case $url in
	pkg:*)
		pkg_site "${url#pkg:}" || return 1
		f=$(python3 -I "$REPO/freebsd/pkgfetch.py" -n "$WORK/pkgs-${url#pkg:}/site" \
		    "$WORK/pkgs-${url#pkg:}/cache" "https://pkg.freebsd.org/$ABI/${url#pkg:}" \
		    "$name" 2> /dev/null)
		[ "$(sha256sum < "$f" | cut -d' ' -f1)" = "$rev" ] ||
		    { echo "$name: not the pinned package ($f)"; return 1; }
		ln -sf "$f" "$SRC/$name.pkg"
		return ;;
	*.tar.*)
		f=$WORK/dl/${url##*/}
		[ -f "$f" ] || curl -sSfL -o "$f" "$url"
		echo "$rev  $f" | sha256sum -c --quiet - ||
		    { echo "$f: not the pinned tarball"; return 1; }
		if [ "$(cat "$SRC/$name/.sha256" 2> /dev/null)" != "$rev" ]; then
			rm -rf "${SRC:?}/$name"
			mkdir -p "$SRC/$name"
			bsdtar -xf "$f" -C "$SRC/$name" --strip-components 1
			echo "$rev" > "$SRC/$name/.sha256"
		fi
		return ;;
	esac
	# a part of a big repository: only its directories (SPARSE), the
	# others' files not even downloaded
	if [ -n "${SPARSE[$1]:-}" ]; then
		[ -d "$SRC/$name/.git" ] ||
		    git clone -q --filter=blob:none --no-checkout "$url" "$SRC/$name" || return 1
		# shellcheck disable=SC2086
		git -C "$SRC/$name" sparse-checkout set ${SPARSE[$1]} || return 1
	fi
	[ -d "$SRC/$name/.git" ] || git clone -q "$url" "$SRC/$name"
	git -C "$SRC/$name" cat-file -e "$rev^{commit}" 2> /dev/null ||
	    git -C "$SRC/$name" fetch -q origin
	# a revision of a pull request only (GitHub)
	git -C "$SRC/$name" cat-file -e "$rev^{commit}" 2> /dev/null ||
	    git -C "$SRC/$name" fetch -q origin '+refs/pull/*/head:refs/remotes/origin/pr/*'
	git -C "$SRC/$name" checkout -q -f "$rev"
	# submodules at their pinned commits only (maplibre-native-qt's
	# maplibre-native has more than 6 GB of history)
	if [ -f "$SRC/$name/.gitmodules" ]; then
		git -C "$SRC/$name" submodule update -q --init --recursive --depth 1 --jobs 8
	fi
}

# Into the part's own root, then the stage and the sysroot
build_part() {
	local name=$1 args=${2%%|*} b=$BUILD/$1 p PREBUILT=
	local CPPFLAGS="$CPPFLAGS"
	PR=$H/pkgroot/$1
	case "$2" in *"|"*) CPPFLAGS="$CPPFLAGS ${2#*|}" ;; esac
	# Harmattan's mce names (harmattan-mce-dev) beside Leste's, first for
	# the N9's parts
	[ "${kind:-}" != harmattan ] ||
	    CPPFLAGS="-I$SYSROOT/usr/local/include/harmattan-mce $CPPFLAGS"
	log "$name"
	rm -rf "$b" "$PR"
	mkdir -p "$PR"
	# FreeBSD's own package (fetch_src): its files as they are, its
	# manifest for pkgmeta.py
	if [ "$args" = pkg ]; then
		PREBUILT=1
		bsdtar -xf "$SRC/$name.pkg" -C "$PR" --exclude '+*'
		bsdtar -xOf "$SRC/$name.pkg" +MANIFEST > "$PR.manifest"
		finish_part
		return
	fi
	cp -a "$SRC/$name" "$b"
	# Debian's own patches (quilt), but those patches/<part>/skip-debian lists
	if [ -f "$b/debian/patches/series" ]; then
		grep -v '^#' "$b/debian/patches/series" | while read -r p _; do
			[ -n "$p" ] || continue
			grep -qx "$p" "$REPO/freebsd/hildon/patches/$name/skip-debian" \
			    2> /dev/null && continue
			patch -s -d "$b" -p1 < "$b/debian/patches/$p" || exit 1
		done || return 1
	fi
	for p in "$REPO"/freebsd/hildon/patches/"$name"/*.patch; do
		[ -f "$p" ] || continue
		patch -s -d "$b" -p1 < "$p" || return 1
	done
	# HiDPI (tools/pro1x-hidpi), shared with Leste
	p=$REPO/patches/hidpi/$name.patch
	if [ -f "$p" ]; then patch -s -d "$b" -p1 < "$p" || return 1; fi
	# MeeGo-like home (patches/meego), shared with Leste
	p=$REPO/patches/meego/$name.patch
	if [ -f "$p" ]; then patch -s -d "$b" -p1 < "$p" || return 1; fi
	# FreeBSD keeps environ in the program (crt1), where glib and libc find
	# it only if it is exported: programs that export just main for
	# maemo-launcher (export.map) export it too
	find "$b" \( -name export.map -o -name '*.version' \) -exec grep -l 'local:' {} + |
	    xargs -r sed -i 's/^\([[:space:]]*global:\)/\1\n\tenviron;\n\t__progname;/'
	# Leste's gettext texts are below /usr/local/share/locale here; sources
	# that name /usr/share/locale (bindtextdomain) would find none
	grep -rl --include='*.c' --include='*.cpp' --include='*.h' --include='*.vala' \
	    '"/usr/share/locale' "$b" 2> /dev/null |
	    xargs -r sed -i 's|"/usr/share/locale|"/usr/local/share/locale|g' || true
	# A part's host tools, if it needs some (pre_<name>)
	if declare -F "pre_${name//-/_}" > /dev/null; then
		"pre_${name//-/_}" || return 1
	fi
	if [ "$args" = "!" ] || [ "$args" = make ]; then
		if [ "$args" = make ]; then p=custom_make; else p=custom_${name//-/_}; fi
		(cd "$b" && "$p") || return 1
		finish_part
		return
	fi
	(
		cd "$b"
		# automake (gnu strictness) wants these; Debian's trees may lack them
		for f in NEWS README AUTHORS ChangeLog; do [ -e "$f" ] || : > "$f"; done
		autoreconf -fi > autoreconf.out 2>&1 || { tail -15 autoreconf.out; exit 1; }
		# shellcheck disable=SC2086
		./configure --host=$TARGET --build=x86_64-pc-linux-gnu \
		    --prefix=/usr/local --sysconfdir=/usr/local/etc \
		    --localstatedir=/var --mandir=/usr/local/share/man \
		    --disable-static --disable-gtk-doc $args > configure.out 2>&1 ||
		    { tail -30 configure.out; exit 1; }
		# Warnings of today's compiler on old code are no errors here.
		find . -name Makefile -exec sed -i 's/ -Werror\([=a-z-]*\)\?//g' {} +
		make -j"$JOBS" "${HOSTTOOLS[@]}" > make.out 2>&1 ||
		    { grep -m20 -E "error:|Error [0-9]" make.out; tail -20 make.out; exit 1; }
		make install "${HOSTTOOLS[@]}" DESTDIR="$PR" > install.out 2>&1 ||
		    { tail -20 install.out; exit 1; }
	) || return 1
	finish_part
}

finish_part() {
	# Programs to /usr/local/bin, where FreeBSD's packages keep theirs
	# (some Leste .pro files install to /usr/bin), and data to
	# /usr/local/share (/usr/share is the base system's); fix_entries
	# follows, patches/<part> correct paths built into programs
	local x
	for x in bin sbin share; do
		if [ -d "$PR/usr/$x" ]; then
			mkdir -p "$PR/usr/local/$x"
			cp -a "$PR/usr/$x/." "$PR/usr/local/$x/"
			rm -rf "${PR:?}/usr/$x"
		fi
	done
	# FreeBSD's own packages stay as FreeBSD built them
	[ -n "${PREBUILT:-}" ] || fix_elf
	unsysroot "$PR"
	find "$PR" -name '*.la' -delete
	# FreeBSD keeps pkg-config files in libdata
	if [ -d "$PR/usr/local/lib/pkgconfig" ]; then
		mkdir -p "$PR/usr/local/libdata/pkgconfig"
		mv "$PR"/usr/local/lib/pkgconfig/* "$PR/usr/local/libdata/pkgconfig/"
		rmdir "$PR/usr/local/lib/pkgconfig"
	fi
	# FreeBSD's additions to a part (extra/<part>: D-Bus activation where
	# Leste starts a daemon from its init scripts, ...)
	if [ -d "$REPO/freebsd/hildon/extra/$name" ]; then
		cp -a "$REPO/freebsd/hildon/extra/$name/." "$PR/"
	fi
	fix_entries "$PR"
	if [ -d "$SRC/$name/.git" ]; then git -C "$SRC/$name" rev-parse HEAD
	else echo "${REV[$name]}"; fi > "$PR.rev"
	case ${URL[$name]:-} in
	pkg:*) echo "https://pkg.freebsd.org/$ABI/${URL[$name]#pkg:}/All/Hashed/$(basename "$(readlink "$SRC/$name.pkg")")" ;;
	*) echo "${URL[$name]:-https://github.com/maemo-leste/$name.git}" ;;
	esac > "$PR.url"
	cp -a "$PR/." "$STAGE/"
	cp -a "$PR/." "$SYSROOT/"
	# the qmake features of the N9's parts, for the parts built on them
	[ "${kind:-}" != harmattan ] || hq5_sysroot_features "$PR"
}

# The ELF files: no build paths in them
fix_elf() {
	# No run path into the sysroot (libtool adds one)
	find "$PR" -type f \( -name '*.so*' -o -perm -u+x \) -exec sh -c \
	    'for f; do if head -c4 "$f" | grep -q ELF &&
	    llvm-readelf -d "$f" | grep -q "RUNPATH.*$0"; then patchelf --remove-rpath "$f"; fi; done' \
	    "$SYSROOT" {} +
	# No dependencies on a file by its build path (CMake links Qt's
	# plugins, which have no soname, so; Qt loads them itself)
	find "$PR" -type f \( -name '*.so*' -o -perm -u+x \) -exec sh -c \
	    'for f; do head -c4 "$f" | grep -q ELF || continue
	    for n in $(llvm-readelf -d "$f" | sed -n "s/.*(NEEDED).*\[\(\/.*\)\]/\1/p"); do
		patchelf --remove-needed "$n" "$f"; done; done' sh {} +
	# No debug information (it names the build's paths)
	find "$PR" -type f \( -name '*.so*' -o -perm -u+x \) -exec sh -c \
	    'for f; do if head -c4 "$f" | grep -q ELF; then llvm-strip --strip-debug "$f"; fi; done' sh {} +
	# Functions called without a declaration: on arm64 a pointer they
	# return loses its upper half, worth a look
	grep -ho "warning: call to undeclared function '[^']*'" \
	    "$BUILD/$name"/make.out "$BUILD/$name"/*/make.out 2> /dev/null |
	    sort -u > "$PR.implicit" || true
	[ -s "$PR.implicit" ] && log "$name: $(sed "s/.*function //" "$PR.implicit" | tr '\n' ' ')"
	return 0
}

# Menu entries and D-Bus services name programs and files below /usr
# that went to /usr/local (prefix), and Leste's process supervisor
# (dsmetool -t), which FreeBSD has not: the paths as installed
fix_entries() {
	python3 - "$1" <<'PY'
import os, re, sys
root = sys.argv[1]
for dp, _, fs in os.walk(root):
    for f in fs:
        if not f.endswith(('.desktop', '.service', '.conf')):
            continue
        p = os.path.join(dp, f)
        if os.path.islink(p):
            continue
        s = open(p, errors='replace').read()
        t = s.replace('/usr/sbin/dsmetool -t ', '')
        # maemo-launcher's client, which FreeBSD has not: the program itself
        t = re.sub(r'(Exec=)(?:/usr/bin/)?maemo-invoker\s+', r'\1', t)
        def local(m):
            path = m.group(0)
            if os.path.lexists(root + '/usr/local/' + path[len('/usr/'):]) and \
               not os.path.lexists(root + path):
                return '/usr/local/' + path[len('/usr/'):]
            return path
        t = re.sub(r'/usr/(?!local/)[A-Za-z0-9_./+-]+', local, t)
        # maemo-launcher's NAME.launch is the plain program NAME here
        def launch(m):
            path = m.group(0)
            if not os.path.lexists(root + path) and os.path.lexists(root + path[:-7]):
                return path[:-7]
            return path
        t = re.sub(r'/usr/local/s?bin/[A-Za-z0-9_.+-]+\.launch\b', launch, t)
        if t != s:
            open(p, 'w').write(t)
PY
}

# HiDPI: the theme scaler and its session script (tools/pro1x-hidpi),
# shared with Leste
hidpi_tool() {
	local b=$BUILD/pro1x-hidpi
	log "pro1x-hidpi"
	mkdir -p "$b"
	# shellcheck disable=SC2046
	$CC $CPPFLAGS $CFLAGS $LDFLAGS -o "$b/pro1x-theme-scale" \
	    "$REPO/tools/pro1x-hidpi/pro1x-theme-scale.c" \
	    $(pkg-config --cflags --libs gdk-pixbuf-2.0) -lm
	PR=$H/pkgroot/pro1x-hidpi
	rm -rf "$PR"
	install -D -m 755 "$b/pro1x-theme-scale" "$PR/usr/local/bin/pro1x-theme-scale"
	install -D -m 755 "$REPO/tools/pro1x-hidpi/pro1x-hidpi" "$PR/usr/local/bin/pro1x-hidpi"
	cp -a "$PR/." "$STAGE/"
}

# The control panel's Desktop applet (apps/pro1x-look): Maemo's or the
# N9's style of Hildon, or Xfce
look_applet() {
	local b=$BUILD/pro1x-look src=$REPO/apps/pro1x-look
	log "pro1x-look"
	mkdir -p "$b"
	# shellcheck disable=SC2046
	$CC $CPPFLAGS $CFLAGS $LDFLAGS -fPIC -shared -DLOCALEDIR='"/usr/local/share/locale"' \
	    -o "$b/libpro1x-look.so" "$src/pro1x-look.c" \
	    $(pkg-config --cflags --libs hildon-1 hildon-control-panel gconf-2.0 libosso gtk+-2.0) \
	    > "$b/make.out" 2>&1 || { grep -m20 error "$b/make.out"; return 1; }
	msgfmt -o "$b/de.mo" "$src/de.po"
	PR=$H/pkgroot/pro1x-look
	rm -rf "$PR"
	install -D -m 644 "$b/libpro1x-look.so" "$PR/usr/local/lib/hildon-control-panel/libpro1x-look.so"
	install -D -m 644 "$src/pro1x-look.desktop" \
	    "$PR/usr/local/share/applications/hildon-control-panel/pro1x-look.desktop"
	install -D -m 644 "$b/de.mo" "$PR/usr/local/share/locale/de/LC_MESSAGES/pro1x-look.mo"
	cp -a "$PR/." "$STAGE/"
}

# This repository's small tools (tools/): the step counter and the
# proximity sensor (libssc), the modem's GNSS for gpsd (QMI), the stand-by
# screen, with their files (extra/<tool>)
dsp_tool() {
	local name=$1 bindir=$2 deps=$3 b=$BUILD/$1
	log "$name"
	mkdir -p "$b"
	# shellcheck disable=SC2046,SC2086
	$CC $CFLAGS $LDFLAGS -o "$b/$name" "$REPO/tools/$name/$name.c" \
	    $(pkg-config --cflags --libs $deps) > "$b/make.out" 2>&1 ||
	    { grep -m20 error "$b/make.out"; return 1; }
	PR=$H/pkgroot/$name
	rm -rf "$PR"
	install -D -m 755 "$b/$name" "$PR/usr/local/$bindir/$name"
	if [ -d "$REPO/freebsd/hildon/extra/$name" ]; then
		cp -a "$REPO/freebsd/hildon/extra/$name/." "$PR/"
	fi
	cp -a "$PR/." "$STAGE/"
}

# pkg-config puts the sysroot in front of path variables, which some
# parts install to: those files back where they belong
unsysroot() {
	local d=$1
	[ -d "$d$SYSROOT" ] || return 0
	cp -a "$d$SYSROOT/." "$d/"
	rm -rf "${d:?}$SYSROOT"
	# the now empty directories of the build path
	find "$d/${SYSROOT#/}" -depth -type d -empty -delete 2> /dev/null || true
	p=${SYSROOT#/}; p=${p%%/*}
	[ -d "$d/$p" ] && find "$d/$p" -depth -type d -empty -delete 2> /dev/null || true
}

# --- data from Leste's archive ----------------------------------------
# The theme, icons and sounds are architecture-independent packages of
# Leste's archive: InRelease checked with Leste's key (keys/), the index
# and the packages with the checksums it signs. /usr/share goes to
# /usr/local/share.
LESTE=https://maedevu.maemo.org/leste
LESTE_DIST=daedalus
# the Maemo 5 sounds as Leste has them: the user interface, calls and
# chat, the ringtones, the chess game. The osso-sounds-* files are Nokia's
# (debian/copyright: all rights reserved) and maemo-ringtones-mr0 names no
# licence; the packages pass them on as Leste's archive does.
LESTE_DATA="hildon-theme-alpha osso-icons-default osso-sounds-ui osso-sounds-rtc maemo-ringtones-mr0 osso-sounds-game-chess"

leste_data() {
	local d=$H/leste f sum
	log "Leste data: $LESTE_DATA"
	mkdir -p "$d"
	curl -sSfL -o "$d/InRelease" "$LESTE/dists/$LESTE_DIST/InRelease"
	gpgv --keyring "$REPO/freebsd/hildon/keys/maemo-leste-archive-leste-2020-automatic.gpg" \
	    --output "$d/Release" "$d/InRelease" 2> /dev/null ||
	    { echo "Leste's InRelease does not verify"; exit 1; }
	sum=$(awk '/^SHA256:/ {s = 1; next} /^[A-Z]/ {s = 0}
	    s && $3 == "main/binary-arm64/Packages.gz" {print $1}' "$d/Release")
	curl -sSfL -o "$d/Packages.gz" "$LESTE/dists/$LESTE_DIST/main/binary-arm64/Packages.gz"
	echo "$sum  $d/Packages.gz" | sha256sum -c --quiet - ||
	    { echo "Leste's package index does not match"; exit 1; }
	for p in $LESTE_DATA; do
		read -r f sum < <(zcat "$d/Packages.gz" | awk -v p="$p" '
		    /^Package: / {n = $2} /^Filename: / {f = $2}
		    /^SHA256: / {s = $2} /^$/ {if (n == p) {print f, s; exit}}')
		[ -n "$f" ] || { echo "$p not in Leste's archive"; exit 1; }
		[ -f "$d/${f##*/}" ] || curl -sSfL -o "$d/${f##*/}" "$LESTE/$f"
		echo "$sum  $d/${f##*/}" | sha256sum -c --quiet - ||
		    { echo "$p does not match the index"; exit 1; }
		rm -rf "$d/x"
		mkdir -p "$d/x"
		bsdtar -xOf "$d/${f##*/}" 'data.tar.*' | bsdtar -xf - -C "$d/x"
		bsdtar -xOf "$d/${f##*/}" 'control.tar.*' | bsdtar -xOf - ./control \
		    > "$d/$p.control"
		PR=$H/pkgroot/$p
		rm -rf "$PR"
		mkdir -p "$PR/usr/local"
		[ -d "$d/x/usr/share" ] && cp -a "$d/x/usr/share" "$PR/usr/local/"
		[ -d "$d/x/etc" ] && cp -a "$d/x/etc" "$PR/usr/local/"
		rm -rf "$PR/usr/local/share/doc" "$PR/usr/local/share/lintian"
		# hicolor's index: FreeBSD's package owns it; Leste's lists the
		# hildon directories. Both merged, found first by hildon-session.
		x=$PR/usr/local/share/icons/hicolor/index.theme
		if [ -f "$x" ]; then
			mkdir -p "$PR/usr/local/share/pro1x/hicolor-hildon/icons/hicolor"
			python3 - "$x" "$SYSROOT/usr/local/share/icons/hicolor/index.theme" \
			    > "$PR/usr/local/share/pro1x/hicolor-hildon/icons/hicolor/index.theme" <<'PY'
import re, sys
def parse(p):
    secs, cur = {}, None
    for line in open(p, errors='replace'):
        m = re.match(r'\[(.*)\]', line.strip())
        if m:
            cur = m.group(1); secs[cur] = []
        elif cur is not None and line.strip():
            secs[cur].append(line.rstrip('\n'))
    return secs
leste, fbsd = parse(sys.argv[1]), parse(sys.argv[2])
def dirs(s):
    for l in s.get('Icon Theme', []):
        if l.startswith('Directories='):
            return [d for d in l.split('=', 1)[1].split(',') if d]
    return []
alld = dirs(fbsd) + [d for d in dirs(leste) if d not in dirs(fbsd)]
head = [l for l in fbsd['Icon Theme'] if not l.startswith('Directories=')]
print('[Icon Theme]'); print('\n'.join(head)); print('Directories=' + ','.join(alld)); print()
for d in alld:
    body = fbsd.get(d) or leste.get(d) or []
    print('[%s]' % d); print('\n'.join(body)); print()
PY
			rm -f "$x"
		fi
		echo "deb ${f##*/} $sum" > "$PR.rev"
		cp -a "$PR/." "$STAGE/"
	done
	rm -rf "$d/x"
}

# The user interface's texts (Leste's *-l10n-<language> packages, all from
# one archive index): English (mr0 is the master the others translate,
# engb, enus) and German, one package per language
LESTE_L10N="en:mr0,engb,enus de:dede"

leste_l10n() {
	local d=$H/leste lang codes p f sum c
	for lang in $LESTE_L10N; do
		codes=${lang#*:}
		PR=$H/pkgroot/leste-l10n-${lang%%:*}
		rm -rf "$PR"
		mkdir -p "$PR/usr/local"
		: > "$PR.rev"
		for c in ${codes//,/ }; do
			zcat "$d/Packages.gz" | awk -v c="$c" '/^Package: / {n = $2}
			    /^Filename: / {f = $2} /^SHA256: / {s = $2}
			    /^$/ {if (n ~ "-l10n-" c "$") print n, f, s}' |
			while read -r p f sum; do
				[ -f "$d/${f##*/}" ] || curl -sSfL -o "$d/${f##*/}" "$LESTE/$f"
				echo "$sum  $d/${f##*/}" | sha256sum -c --quiet - ||
				    { echo "$p does not match the index"; exit 1; }
				bsdtar -xOf "$d/${f##*/}" 'data.tar.*' |
				    bsdtar -xf - -C "$PR/usr/local" --strip-components 2 \
				    './usr/share' 2> /dev/null
				echo "deb ${f##*/} $sum" >> "$PR.rev"
			done || exit 1
		done
		rm -rf "$PR/usr/local/share/doc" "$PR/usr/local/share/lintian"
		log "Leste texts ${lang%%:*}: $(wc -l < "$PR.rev") packages"
		cp -a "$PR/." "$STAGE/"
	done
}

mkdir -p "$SRC" "$BUILD" "$H/pkgroot" "$STAGE/usr/local/share" "$STAGE/usr/local/etc"
wanted() {
	[ $# -eq 1 ] && return 0
	case " ${*:2} " in *" $1 "*) return 0 ;; esac
	[ "$2" = apps ] && [ "$kind" = app ] && return 0
	[ "$2" = harmattan ] && [ "$kind" = harmattan ]
}
kind=desktop
for entry in "${PARTS[@]}"; do
	name=${entry%%|*}
	wanted "$name" "$@" || continue
	fetch_src "$name"
	build_part "$name" "${entry#*|}"
done
kind=app
failed=()
for entry in "${APPS[@]}"; do
	name=${entry%%|*}
	wanted "$name" "$@" || continue
	fetch_src "$name"
	# shellcheck disable=SC2015
	(build_part "$name" "${entry#*|}") || failed+=("$name")
done
if [ $# -eq 0 ] || [[ " $* " == *" data "* ]]; then
	leste_data
	leste_l10n
fi
if [ $# -eq 0 ] || [[ " $* " == *" pro1x-hidpi "* ]]; then
	hidpi_tool
fi
if [ $# -eq 0 ] || [[ " $* " == *" pro1x-look "* ]]; then
	look_applet || failed+=(pro1x-look)
fi
if [ $# -eq 0 ] || [[ " $* " == *" pro1x-steps "* ]]; then
	dsp_tool pro1x-steps sbin "libssc gio-unix-2.0" || failed+=(pro1x-steps)
fi
if [ $# -eq 0 ] || [[ " $* " == *" pro1x-gnss "* ]]; then
	dsp_tool pro1x-gnss bin "qmi-glib qrtr-glib" || failed+=(pro1x-gnss)
fi
if [ $# -eq 0 ] || [[ " $* " == *" pro1x-proximity "* ]]; then
	dsp_tool pro1x-proximity sbin "libssc gio-unix-2.0" ||
	    failed+=(pro1x-proximity)
fi
if [ $# -eq 0 ] || [[ " $* " == *" pro1x-standby "* ]]; then
	dsp_tool pro1x-standby bin "x11 xext xi cairo pangocairo sqlite3 dbus-1" ||
	    failed+=(pro1x-standby)
fi
# the N9's user interface (harmattan.sh): in order, each on the ones before
kind=harmattan
for entry in "${HARMATTAN[@]}"; do
	name=harmattan-${entry%%|*}
	[ $# -gt 0 ] && wanted "$name" "$@" || continue
	fetch_src "$name"
	(build_part "$name" "${entry#*|}") || { failed+=("$name"); break; }
done
[ ${#failed[@]} -eq 0 ] || log "apps that failed: ${failed[*]}"
log "done: $STAGE"
