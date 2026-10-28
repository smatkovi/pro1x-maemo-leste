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
# (Pure Maps and OSM Scout Server: Arch has them as Flatpaks)
SKIP=" ffmpeg8-libs qt5-webengine qt5-webkit jib dorian hexagonrpc libqrtr-glib libqmi libssc qrtr rmtfs tqftpserv pd-mapper maplibre-native-qt nemo-qml-plugin-dbus mapbox-gl-qml pure-maps osmscout-server "
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
bash -c "declare -A REV URL; . '$MNT$B/apps-arch.sh'; declare -p APPS REV URL" > "$MNT$B/apps-tables.sh"
. "$MNT$B/apps-tables.sh"

log "packages"
chroot "$MNT" /bin/bash -euo pipefail <<'EOF'
pacman -S --noconfirm --needed cmake qt5-base qt5-svg qt5-declarative \
	qt5-quickcontrols qt5-quickcontrols2 qt5-graphicaleffects qt5-multimedia \
	qt5-x11extras qt5-xmlpatterns qt5-location qt5-tools qt5-websockets \
	poppler-glib libical espeak-ng expat libxkbfile curl libsigc++ \
	evolution-data-server sdl12-compat sdl_mixer sdl_image gnuchess \
	python python-pip gettext libevent json-c protobuf-c > /dev/null || true
gpgconf --homedir /etc/pacman.d/gnupg --kill all 2> /dev/null || true
# Leste's Qt spec: Linux with Maemo 5's define and platform
d=$(qmake-qt5 -query QT_INSTALL_ARCHDATA)/mkspecs/linux-maemo
mkdir -p "$d"
printf 'include(../linux-g++/qmake.conf)\nQMAKE_PLATFORM += maemo5\nDEFINES += Q_WS_MAEMO_5\n' > "$d/qmake.conf"
printf '#include "../linux-g++/qplatformdefs.h"\n' > "$d/qplatformdefs.h"
EOF

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
		if [ -f debian/patches/series ]; then
			grep -v '^#' debian/patches/series | while read -r q _; do
				[ -n "$q" ] && patch -s -p1 < "debian/patches/$q"
			done
		fi
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
	cp -a "$PR/." /
	ldconfig
	echo "$rev" > "$B/$name.done"
done < $B/apps-list
echo "failed:${failed:- none}"
EOF
