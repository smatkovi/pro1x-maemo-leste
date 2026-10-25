#!/bin/bash
# Builds Maemo Leste's Hildon desktop for FreeBSD/arm64 on a Linux PC, by
# cross-compiling: clang for aarch64-unknown-freebsd15.0 against a sysroot
# made of the release's base system and FreeBSD's packages (the libraries
# Hildon builds on: gtk2, gconf2, dbus-glib, ...). The Hildon parts come
# from github.com/maemo-leste, with FreeBSD changes in patches/<part>/.
#
#   WORK=/path/to/freebsd-build bash freebsd/hildon/build.sh [part...]
#
# The result is $WORK/hildon/stage (installed under /usr/local), which
# rootfs.sh puts into the root image. Every part is also installed into
# the sysroot, for the parts built after it.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
WORK=${WORK:?set WORK=<build directory>}
H=$WORK/hildon
SRC=$H/src
SYSROOT=$H/sysroot
STAGE=$H/stage
BUILD=$H/build
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
    "$WORK/pkgs/cache" "$PKGURL" $HILDON_PKGS > "$H/sysroot.want"
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
export CFLAGS="-O2 -pipe -std=gnu17 -Wno-error -Wno-incompatible-pointer-types -Wno-incompatible-function-pointer-types -Wno-int-conversion -Wno-implicit-function-declaration -Wno-implicit-int"
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
	"mce-dev|"
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

# Parts without autotools: "name|!" builds with custom_<name> in the tree
custom_profiled() {
	local m=(PREFIX=/usr/local ETCDIR=/usr/local/etc/profiled
	    PKGCFGDIR=/usr/local/libdata/pkgconfig "${HOSTTOOLS[@]}")
	make -j"$JOBS" "${m[@]}" LDLIBS="$(pkg-config --libs dbus-glib-1 dbus-1 glib-2.0) -linotify" \
	    libprofile.so.0 libprofile.a profiled profile.pc > make.out 2>&1 ||
	    { grep -m20 "error" make.out; exit 1; }
	for d in "$STAGE" "$SYSROOT"; do
		make "${m[@]}" ROOT="$d" install-libprofile install-libprofile-dev \
		    install-profiled > install.out 2>&1 || { tail install.out; exit 1; }
	done
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
	for d in "$STAGE" "$SYSROOT"; do
		install -D -m 755 libbookmarkengine.so.0 \
		    "$d/usr/local/lib/libbookmarkengine.so.0"
		ln -sf libbookmarkengine.so.0 "$d/usr/local/lib/libbookmarkengine.so"
		install -D -m 644 src/osso_bookmark_parser.h \
		    "$d/usr/local/include/osso_bookmark_parser.h"
		install -D -m 644 pc.out \
		    "$d/usr/local/libdata/pkgconfig/osso-bookmark-engine.pc"
	done
}

build_part() {
	local name=$1 args=${2%%|*} b=$BUILD/$1 p
	local CPPFLAGS="$CPPFLAGS"
	case "$2" in *"|"*) CPPFLAGS="$CPPFLAGS ${2#*|}" ;; esac
	log "$name"
	rm -rf "$b"
	cp -a "$SRC/$name" "$b"
	# Debian's own patches (quilt), but those patches/<part>/skip-debian lists
	if [ -f "$b/debian/patches/series" ]; then
		grep -v '^#' "$b/debian/patches/series" | while read -r p _; do
			[ -n "$p" ] || continue
			grep -qx "$p" "$REPO/freebsd/hildon/patches/$name/skip-debian" \
			    2> /dev/null && continue
			patch -s -d "$b" -p1 < "$b/debian/patches/$p"
		done
	fi
	for p in "$REPO"/freebsd/hildon/patches/"$name"/*.patch; do
		[ -f "$p" ] && patch -s -d "$b" -p1 < "$p"
	done
	if [ "$args" = "!" ]; then
		(cd "$b" && "custom_${name//-/_}")
		return
	fi
	(
		cd "$b"
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
		make install "${HOSTTOOLS[@]}" DESTDIR="$STAGE" > install.out 2>&1 ||
		    { tail -20 install.out; exit 1; }
		make install "${HOSTTOOLS[@]}" DESTDIR="$SYSROOT" > /dev/null 2>&1
	)
	unsysroot "$STAGE"
	unsysroot "$SYSROOT"
	find "$STAGE" "$SYSROOT" -name '*.la' -delete
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
LESTE_DATA="hildon-theme-alpha osso-icons-default osso-sounds-ui"

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
		[ -d "$d/x/usr/share" ] && cp -a "$d/x/usr/share/." "$STAGE/usr/local/share/"
		[ -d "$d/x/etc" ] && cp -a "$d/x/etc/." "$STAGE/usr/local/etc/"
	done
	rm -rf "$d/x" "$STAGE/usr/local/share/doc" "$STAGE/usr/local/share/lintian"
}

mkdir -p "$SRC" "$BUILD" "$STAGE/usr/local/share" "$STAGE/usr/local/etc"
for entry in "${PARTS[@]}"; do
	name=${entry%%|*}
	if [ $# -gt 0 ]; then
		case " $* " in *" $name "*) ;; *) continue ;; esac
	fi
	[ -d "$SRC/$name" ] || git clone -q https://github.com/maemo-leste/$name.git "$SRC/$name"
	build_part "$name" "${entry#*|}"
done
if [ $# -eq 0 ] || [[ " $* " == *" data "* ]]; then
	leste_data
fi
log "done: $STAGE"
