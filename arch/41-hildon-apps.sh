#!/bin/bash
# The apps for the Hildon and MeeGo sessions on Arch: what the FreeBSD
# packages have (freebsd/hildon/apps.sh: Leste's libraries the apps need,
# the apps Leste installs, its extras, the Maemo 5 ports, the MeeGo clock,
# MeeCast), built natively in the image after arch/40-hildon.sh.
#
# apps.sh's recipes are read for /usr instead of FreeBSD's /usr/local, its
# build helpers (qmake, CMake) are replaced by native ones with a Maemo
# spec as Leste's (Q_WS_MAEMO_5); Debian's patches and the HiDPI and MeeGo
# patches are applied, the FreeBSD ones are not. Not built here: the web
# engines (no QtWebEngine for Arch Linux ARM, QtWebKit would build for
# hours) and the apps on them (Jib, Dorian), and the DSP userland, which
# step 20 takes from Leste. A part that fails is listed at the end and
# does not stop the others.
#
#   bash arch/41-hildon-apps.sh            # as root in Leste, image on $MNT
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
MNT=${MNT:-/mnt/arch}
JOBS=${JOBS:-$(nproc)}
B=/usr/src/hildon
# (Pure Maps and OSM Scout Server: Arch has them as Flatpaks; countdowntimer
# needs Phonon, which Arch Linux ARM no longer has for Qt 5)
SKIP=" ffmpeg8-libs qt5-webengine qt5-webkit jib dorian hexagonrpc libqrtr-glib libqmi libssc qrtr rmtfs tqftpserv pd-mapper maplibre-native-qt nemo-qml-plugin-dbus mapbox-gl-qml pure-maps osmscout-server countdowntimer "
log() { echo ">> $*"; }

mountpoint -q "$MNT" || { echo "$MNT is not mounted"; exit 1; }
for m in proc sys dev dev/pts; do
	mountpoint -q "$MNT/$m" || mount --bind "/$m" "$MNT/$m"
done
cleanup() {
	chroot "$MNT" gpgconf --homedir /etc/pacman.d/gnupg --kill all 2> /dev/null || true
	for m in dev/pts dev sys proc; do
		! mountpoint -q "$MNT/$m" || umount "$MNT/$m" || umount -l "$MNT/$m"
	done
}
trap cleanup EXIT
rm -f "$MNT/etc/resolv.conf"
cp /etc/resolv.conf "$MNT/etc/resolv.conf"

# apps.sh for /usr (its recipes, the list and the revisions)
mkdir -p "$MNT$B/src" "$MNT$B/patches"
sed -e 's|/usr/local|/usr|g' -e 's|libdata/pkgconfig|lib/pkgconfig|g' \
	"$REPO/freebsd/hildon/apps.sh" > "$MNT$B/apps-arch.sh"
mkdir -p "$MNT$B/patches/hidpi" "$MNT$B/patches/meego"
cp -a "$REPO/patches/hidpi/." "$MNT$B/patches/hidpi/"
cp -a "$REPO/patches/meego/." "$MNT$B/patches/meego/"
# those of FreeBSD's patches that are no FreeBSD matter (Qt 5.15, today's
# compilers); the others are its paths and its interfaces
FBSD_PATCHES="easylist/0001-missing-semicolon.patch
	qt-components/0002-qt-5-15-install.patch
	qt-components/0003-theme-images-directory.patch
	qt-components/0004-harmattan-fonts.patch
	qt-components/0005-qt-5-15-orientation.patch
	qt-components/0006-window-takes-its-own-size.patch
	qt-components/0007-hidden-info-banner-takes-no-taps.patch
	qt-components/0008-derived-styles-set-the-base-property.patch
	qt-components/0009-closed-dialog-takes-no-taps.patch"
rm -rf "${MNT:?}$B/patches/fbsd"
for q in $FBSD_PATCHES; do
	mkdir -p "$MNT$B/patches/fbsd/${q%%/*}"
	cp "$REPO/freebsd/hildon/patches/$q" "$MNT$B/patches/fbsd/$q"
done
bash -c "declare -A REV URL; . '$MNT$B/apps-arch.sh'; declare -p APPS REV URL" > "$MNT$B/apps-tables.sh"
. "$MNT$B/apps-tables.sh"

log "packages"
chroot "$MNT" /bin/bash -euo pipefail <<'EOF'
# only those Arch Linux ARM has (qt5-location and qt5-websockets it has
# not): one unknown name would make pacman install none of them. dpkg for
# dpkg-parsechangelog, which configure.ac files call for their version.
have=
for p in cmake qt5-base qt5-svg qt5-declarative \
	qt5-quickcontrols qt5-quickcontrols2 qt5-graphicaleffects qt5-multimedia \
	qt5-x11extras qt5-xmlpatterns qt5-location qt5-tools qt5-websockets \
	poppler-glib libical espeak-ng expat libxkbfile curl libsigc++ \
	evolution-data-server sdl12-compat sdl_mixer sdl_image gnuchess \
	python python-pip gettext libevent json-c protobuf-c dpkg; do
	if pacman -Si "$p" > /dev/null 2>&1; then have="$have $p"
	else echo "   (Arch Linux ARM has no $p)"; fi
done
# shellcheck disable=SC2086
pacman -S --noconfirm --needed $have > /dev/null
gpgconf --homedir /etc/pacman.d/gnupg --kill all 2> /dev/null || true
# Leste's Qt spec: Linux with Maemo 5's define and platform
d=$(qmake-qt5 -query QT_INSTALL_ARCHDATA)/mkspecs/linux-maemo
mkdir -p "$d"
printf 'include(../linux-g++/qmake.conf)\nQMAKE_PLATFORM += maemo5\nDEFINES += Q_WS_MAEMO_5\n' > "$d/qmake.conf"
printf '#include "../linux-g++/qplatformdefs.h"\n' > "$d/qplatformdefs.h"
EOF

# GTK 2's VTE (0.28, libvte.so.9), which osso-xterm is written for; Arch
# has only the GTK 3 and 4 ones
VTE=vte-0.28.2
VTE_SHA256=86cf0b81aa023fa93ed415653d51c96767f20b2d7334c893caba71e42654b0ae
if [ ! -f "$MNT/usr/lib/pkgconfig/vte.pc" ]; then
	log "$VTE (GTK 2)"
	mkdir -p "$MNT$B/dl"
	f=$MNT$B/dl/$VTE.tar.xz
	[ -f "$f" ] || curl -sSfL -o "$f" "https://download.gnome.org/sources/vte/0.28/$VTE.tar.xz"
	echo "$VTE_SHA256  $f" | sha256sum -c --quiet -
	rm -rf "${MNT:?}$B/build/$VTE"
	mkdir -p "$MNT$B/build"
	tar -xJf "$f" -C "$MNT$B/build"
	chroot "$MNT" /bin/bash -euo pipefail -s "$B/build/$VTE" "$JOBS" <<'EOF'
cd "$1"
export CFLAGS="-O2 -pipe -std=gnu17 -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration -Wno-error=int-conversion"
# (its config.guess is older than aarch64)
./configure --build=aarch64-unknown-linux-gnu --prefix=/usr --libexecdir=/usr/lib/vte --disable-static \
	--with-gtk=2.0 --disable-introspection --disable-python --disable-gtk-doc \
	> configure.log 2>&1 || { tail -20 configure.log; exit 1; }
make -j"$2" > make.log 2>&1 || { grep -m20 -E "error:|Error [0-9]" make.log; exit 1; }
make install > install.log 2>&1 || { tail -20 install.log; exit 1; }
ldconfig
EOF
fi

# libical 3 (the calendar backend is written for it; Arch has libical 4,
# whose API changed), as FreeBSD has it: below /opt/libical3, beside
# Arch's, for the calendar backend and the apps on it only (the build loop)
ICAL=libical-3.0.20
ICAL_SHA256=e73de92f5a6ce84c1b00306446b290a2b08cdf0a80988eca0a2c9d5c3510b4c2
if [ ! -f "$MNT/opt/libical3/lib/pkgconfig/libical.pc" ]; then
	log "$ICAL (/opt/libical3)"
	mkdir -p "$MNT$B/dl" "$MNT$B/build"
	f=$MNT$B/dl/$ICAL.tar.gz
	[ -f "$f" ] || curl -sSfL -o "$f" "https://github.com/libical/libical/releases/download/v${ICAL#libical-}/$ICAL.tar.gz"
	echo "$ICAL_SHA256  $f" | sha256sum -c --quiet -
	rm -rf "${MNT:?}$B/build/$ICAL"
	tar -xzf "$f" -C "$MNT$B/build"
	chroot "$MNT" /bin/bash -euo pipefail -s "$B/build/$ICAL" "$JOBS" <<'EOF'
cd "$1"
cmake -S . -B _build -DCMAKE_INSTALL_PREFIX=/opt/libical3 -DCMAKE_INSTALL_LIBDIR=lib \
	-DCMAKE_BUILD_TYPE=Release -DICAL_GLIB=OFF -DICAL_BUILD_DOCS=OFF \
	-DGOBJECT_INTROSPECTION=OFF -DICAL_GLIB_VAPI=OFF -DLIBICAL_BUILD_TESTING=OFF \
	-DWITH_CXX_BINDINGS=OFF -DSHARED_ONLY=ON > cmake.log 2>&1 || { tail -20 cmake.log; exit 1; }
cmake --build _build -j"$2" > make.log 2>&1 || { grep -m20 -E "error:|Error [0-9]" make.log; exit 1; }
cmake --install _build > install.log 2>&1 || { tail -20 install.log; exit 1; }
echo /opt/libical3/lib > /etc/ld.so.conf.d/libical3.conf
ldconfig
EOF
fi

# Qt 5's positioning module (meecast's GPS place), which Arch Linux ARM
# does not have (qt5-location): from Qt's qtlocation of qt5-base's version,
# the library and its GeoClue 2 plugin only, into the Qt of the system;
# what it put there is listed in $B/qt5-positioning.files
QTLOC=qtlocation-everywhere-opensource-src-5.15.19
QTLOC_SHA256=d521e36220e061ebfc4109fcbfa4092e861ed1ca54229ffee2eafa1e26f45fd8
if ! chroot "$MNT" pacman -Qq qt5-location > /dev/null 2>&1 &&
    [ ! -f "$MNT$B/qt5-positioning.files" ]; then
	log "QtPositioning (${QTLOC##*-})"
	mkdir -p "$MNT$B/dl" "$MNT$B/build"
	f=$MNT$B/dl/$QTLOC.tar.xz
	[ -f "$f" ] || curl -sSfL -o "$f" "https://download.qt.io/archive/qt/5.15/${QTLOC##*-}/submodules/$QTLOC.tar.xz"
	echo "$QTLOC_SHA256  $f" | sha256sum -c --quiet -
	rm -rf "${MNT:?}$B/build/qtlocation"
	mkdir "$MNT$B/build/qtlocation"
	tar -xJf "$f" -C "$MNT$B/build/qtlocation" --strip-components=1
	chroot "$MNT" /bin/bash -euo pipefail -s "$B" "$JOBS" <<'EOF'
cd "$1/build/qtlocation"
mkdir _build && cd _build
qmake-qt5 .. > qmake.log 2>&1 || { tail -20 qmake.log; exit 1; }
make qmake_all > qmake_all.log 2>&1 || { tail -20 qmake_all.log; exit 1; }
make -C src -j"$2" sub-positioning > make.log 2>&1 || { grep -m20 -E "error:|Error [0-9]" make.log; exit 1; }
make -C src/plugins/position/geoclue2 -j"$2" >> make.log 2>&1 || { grep -m20 -E "error:|Error [0-9]" make.log; exit 1; }
st=$PWD/_stage
for d in src/positioning src/plugins/position/geoclue2; do
	make -C "$d" install INSTALL_ROOT="$st" >> install.log 2>&1 || { tail -20 install.log; exit 1; }
done
(cd "$st" && find . ! -type d | sed 's/^\.//') > "$1/qt5-positioning.files.new"
cp -a "$st/." /
ldconfig
mv "$1/qt5-positioning.files.new" "$1/qt5-positioning.files"
EOF
fi

log "sources"
for p in "${APPS[@]}"; do
	name=${p%%|*}
	case $SKIP in *" $name "*) continue ;; esac
	url=${URL[$name]:-https://github.com/maemo-leste/$name.git}
	d=$MNT$B/src/$name
	case $url in *.tar.*|pkg:*) continue ;; esac
	[ -d "$d/.git" ] || git clone -q "$url" "$d" || { echo "   $name: clone failed"; continue; }
	git -C "$d" cat-file -e "${REV[$name]}^{commit}" 2> /dev/null || git -C "$d" fetch -q origin || true
	git -C "$d" checkout -q -f "${REV[$name]}" || { echo "   $name: no ${REV[$name]}"; continue; }
	git -C "$d" submodule update -q --init --recursive 2> /dev/null || true
done

log "build"
printf '%s\n' "${APPS[@]}" > "$MNT$B/apps-list"
chroot "$MNT" /bin/bash -uo pipefail -s "$JOBS" "$SKIP" <<'EOF'
jobs=$1 skip=$2
B=/usr/src/hildon
declare -A REV URL
# apps.sh's FreeBSD-only paths (the DSP headers) ask for the repository
REPO=$B
. $B/apps-arch.sh
# native in place of the FreeBSD build's cross helpers
JOBS=$jobs CC=gcc CXX=g++ AR=ar SYSROOT= TARGET=aarch64-linux-gnu HOSTTOOLS=()
H=$B BUILD=$B/build SRC=$B/src
CPPFLAGS=
CFLAGS="-O2 -pipe -std=gnu17 -Wno-error=incompatible-pointer-types \
-Wno-error=implicit-function-declaration -Wno-error=int-conversion \
-Wno-error=implicit-int -Wno-error=return-mismatch"
CXXFLAGS="-O2 -pipe -fpermissive"
LDFLAGS=
export CFLAGS CXXFLAGS
qt_host() { :; }
usr_to_local() { :; }
unsysroot() { :; }
qt_qmake() {
	local pro=${1:-}
	[ $# -gt 0 ] && shift
	qmake-qt5 -spec linux-maemo PREFIX=/usr "$@" $pro > qmake.out 2>&1 ||
	    { tail -20 qmake.out; exit 1; }
	make -j"$JOBS" > make.out 2>&1 || { grep -m20 -E "error:|Error [0-9]|\*\*\*" make.out; exit 1; }
	make install INSTALL_ROOT="$PR" > install.out 2>&1 || { tail -20 install.out; exit 1; }
}
qt_cmake() {
	cmake -S . -B _build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release \
	    -DCMAKE_CXX_FLAGS="-DQ_WS_MAEMO_5 $CXXFLAGS" "$@" > cmake.out 2>&1 ||
	    { tail -30 cmake.out; exit 1; }
	cmake --build _build -j"$JOBS" > make.out 2>&1 || { grep -m20 -E "error:|Error [0-9]|\*\*\*" make.out; exit 1; }
	DESTDIR="$PR" cmake --install _build > install.out 2>&1 || { tail -20 install.out; exit 1; }
}
failed=
while IFS= read -r p; do
	name=${p%%|*} rest=${p#*|}
	args=${rest%%|*}
	extra=; case $rest in *"|"*) extra=${rest#*|} ;; esac
	case $skip in *" $name "*) continue ;; esac
	[ -d "$B/src/$name/.git" ] || { failed="$failed $name(source)"; continue; }
	rev=$(git -C $B/src/$name rev-parse HEAD)
	if [ "$(cat $B/$name.done 2> /dev/null)" = "$rev" ]; then
		echo "   $name: built"; continue
	fi
	echo "   $name"
	PR=$B/root/$name
	rm -rf "$B/build/$name" "$PR"; mkdir -p "$B/build" "$PR"
	cp -a "$B/src/$name" "$B/build/$name"
	(
		set -e
		cd "$B/build/$name"
		# the calendar on libical 3 (above)
		case $name in
		calendar-backend|qalendar|cal-home-widget)
			export PKG_CONFIG_PATH=/opt/libical3/lib/pkgconfig CPATH=/opt/libical3/include
			;;
		# Arch has espeak-ng only: its espeak API under espeak's names
		mstardict)
			c=$B/compat/espeak
			mkdir -p "$c/include/espeak" "$c/lib"
			echo '#include <espeak-ng/speak_lib.h>' > "$c/include/espeak/speak_lib.h"
			ln -sfn /usr/lib/libespeak-ng.so "$c/lib/libespeak.so"
			export CPATH=$c/include LIBRARY_PATH=$c/lib
			;;
		esac
		if [ -f debian/patches/series ]; then
			grep -v '^#' debian/patches/series | while read -r q _; do
				[ -n "$q" ] && patch -s -p1 < "debian/patches/$q"
			done
		fi
		for q in "$B/patches/fbsd/$name"/*.patch; do
			[ ! -f "$q" ] || patch -s -p1 < "$q"
		done
		for d in hidpi meego; do
			[ ! -f "$B/patches/$d/$name.patch" ] || patch -s -p1 < "$B/patches/$d/$name.patch"
		done
		case $args in
		"!")	"custom_${name//-/_}" ;;
		make)	custom_make ;;
		*)
			for f in NEWS README AUTHORS ChangeLog; do [ -e "$f" ] || : > "$f"; done
			autoreconf -fi > autoreconf.log 2>&1 || { tail -15 autoreconf.log; exit 1; }
			# shellcheck disable=SC2086
			CPPFLAGS="$extra" ./configure --prefix=/usr --sysconfdir=/etc \
				--localstatedir=/var --disable-static --disable-gtk-doc $args \
				> configure.log 2>&1 || { tail -25 configure.log; exit 1; }
			find . -name Makefile -exec sed -i 's/ -Werror\([=a-z-]*\)\?//g' {} +
			make -j"$JOBS" > make.log 2>&1 || { grep -m20 -E "error:|Error [0-9]|\*\*\*" make.log; exit 1; }
			make install DESTDIR="$PR" > install.log 2>&1 || { tail -20 install.log; exit 1; }
			;;
		esac
	) < /dev/null > "$B/build/$name.log" 2>&1 || { failed="$failed $name"; tail -5 "$B/build/$name.log" | sed 's/^/     /'; continue; }
	# Arch's /bin, /sbin, /usr/sbin are links to /usr/bin, /lib to /usr/lib
	for d in usr/sbin sbin bin; do
		if [ -d "$PR/$d" ] && [ ! -L "$PR/$d" ]; then
			mkdir -p "$PR/usr/bin"
			cp -a "$PR/$d/." "$PR/usr/bin/"
			rm -rf "${PR:?}/$d"
		fi
	done
	if [ -d "$PR/lib" ] && [ ! -L "$PR/lib" ]; then
		mkdir -p "$PR/usr/lib"
		cp -a "$PR/lib/." "$PR/usr/lib/"
		rm -rf "${PR:?}/lib"
	fi
	cp -a "$PR/." / || { failed="$failed $name(install)"; continue; }
	ldconfig
	echo "$rev" > "$B/$name.done"
done < $B/apps-list
echo "failed:${failed:- none}"
EOF
