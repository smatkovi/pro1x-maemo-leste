#!/bin/bash
# The Nokia N9's user interface in its Qt 5 port (github.com/smatkovi/
# harmattan-qt5) as Maemo Leste packages: one arm64 .deb per part,
# harmattan-<directory>, and Sailfish OS's ngfd and libngf (the event
# sound daemon the system UI plays through), built natively on the phone.
# The parts, their order and their build arguments are arch/30-meego.sh's.
#
# The build runs in a chroot of an overlay on meego-leste.img, mounted
# read-only: the image stays as it is, and Leste's own root, which changes
# while it runs, is not the lower layer. The build dependencies and each
# part's package, installed before the next part builds (its headers, and
# the Depends of the parts after it), go to the overlay's upper layer
# below $W. The packages go to $OUT.
#
# Runs as root on the phone in Maemo Leste (apt and git need the network):
#
#   bash meego/50-harmattan-qt5-debs.sh
#   ONLY="meegotouchhome system-ui" bash meego/50-harmattan-qt5-debs.sh
#
# A part built at its revision (and PKGREV) is skipped; ONLY builds those
# parts again. HQ5_REV is FreeBSD's pin (freebsd/hildon/harmattan.sh). The
# N9's theme (blanco), fonts and sounds are Nokia's and not in the
# packages; meegotouchtheme has the base theme and the Pro1X's.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
LOWER=${LOWER:-/host/meego-leste.img}
W=${W:-/host/hq5-leste}
OUT=${OUT:-$W/debs}
HQ5_URL=${HQ5_URL:-https://github.com/smatkovi/harmattan-qt5.git}
HQ5_REV=${HQ5_REV:-$(sed -n 's/^HQ5_REV=//p' "$REPO/freebsd/hildon/harmattan.sh")}
# the packages' revision: <upstream version>+pro1x<PKGREV>
PKGREV=${PKGREV:-1}
JOBS=${JOBS:-$(nproc)}
ONLY=${ONLY:-}
log() { echo ">> $*"; }

[ -f "$LOWER" ] || { echo "no $LOWER"; exit 1; }
[ -n "$HQ5_REV" ] || { echo "no HQ5_REV"; exit 1; }
[ -z "$(losetup -j "$LOWER")" ] ||
    { echo "$LOWER is attached to a loop device (losetup -j): in use, not a lower layer"; exit 1; }

R=$W/root
mkdir -p "$W/lower" "$W/upper" "$W/work" "$R" "$OUT"
cleanup() {
	for m in dev/pts dev sys proc; do
		! mountpoint -q "$R/$m" || umount "$R/$m" || umount -l "$R/$m"
	done
	! mountpoint -q "$R" || umount "$R"
	! mountpoint -q "$W/lower" || umount "$W/lower"
}
trap cleanup EXIT
mount -o loop,ro "$LOWER" "$W/lower" 2> /dev/null || mount -o loop,ro,noload "$LOWER" "$W/lower"
mount -t overlay overlay -o "lowerdir=$W/lower,upperdir=$W/upper,workdir=$W/work" "$R"
for m in proc sys dev dev/pts; do
	mount --bind "/$m" "$R/$m"
done
rm -f "$R/etc/resolv.conf"
cp -L /etc/resolv.conf "$R/etc/resolv.conf"
# no services started by the packages' scripts in the chroot
printf '#!/bin/sh\nexit 101\n' > "$R/usr/sbin/policy-rc.d"
chmod 755 "$R/usr/sbin/policy-rc.d"
# icu-config, which Debian 12's ICU no longer has: libmlocale's and
# libmeegotouch's configure scripts look for it (without, they build
# without ICU: no calendars, collation or number formats of their own)
mkdir -p "$R/usr/local/bin"
cat > "$R/usr/local/bin/icu-config" <<'ICU'
#!/bin/sh
# icu-config from ICU's pkg-config files (meego/50-harmattan-qt5-debs.sh)
for a; do
	case $a in
	--exists) pkg-config --exists icu-i18n icu-uc || exit 1 ;;
	--version) pkg-config --modversion icu-uc ;;
	--cflags|--cxxflags|--cppflags|--cppflags-searchpath) pkg-config --cflags icu-i18n icu-uc ;;
	--ldflags|--ldflags-searchpath|--ldflags-libsonly) pkg-config --libs icu-i18n icu-uc ;;
	--bindir|--sbindir) echo /usr/bin ;;
	--prefix) pkg-config --variable=prefix icu-uc ;;
	--icudata) pkg-config --variable=ICUDATA_NAME icu-uc ;;
	*) echo "icu-config: $a is not known here" >&2; exit 1 ;;
	esac
done
ICU
chmod 755 "$R/usr/local/bin/icu-config"

log "build dependencies"
chroot "$R" /bin/bash -euo pipefail <<'EOF'
export DEBIAN_FRONTEND=noninteractive
apt-get -q update > /tmp/apt-update.log 2>&1 || { tail -20 /tmp/apt-update.log; exit 1; }
apt-get -y -q --no-install-recommends install \
	build-essential git pkg-config file dpkg-dev fakeroot autoconf automake libtool \
	python3 doxygen \
	qtbase5-dev qtbase5-private-dev qtdeclarative5-dev libqt5x11extras5-dev \
	libqt5svg5-dev libqt5opengl5-dev qttools5-dev qttools5-dev-tools \
	libicu-dev icu-devtools libdbus-glib-1-dev libxml2-dev libpolkit-gobject-1-dev intltool \
	gtk-doc-tools gobject-introspection libgirepository1.0-dev libcdb-dev \
	libxdamage-dev libxcomposite-dev libxrandr-dev libxtst-dev libxi-dev libx11-xcb-dev \
	libxcb-shape0-dev libxcb-render0-dev libdsme0.2.0-dev libxkbfile-dev \
	libpulse-dev libsndfile1-dev libgconf2-dev gconf2 libprofile-dev \
	check libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libcanberra-dev \
	> /tmp/apt-install.log 2>&1 || { tail -20 /tmp/apt-install.log; exit 1; }
EOF

log "harmattan-qt5 $HQ5_REV"
rc=0
chroot "$R" /bin/bash -uo pipefail -s "$HQ5_URL" "$HQ5_REV" "$JOBS" "$PKGREV" "$ONLY" <<'EOF' || rc=$?
url=$1 rev=$2 jobs=$3 pkgrev=$4 only=$5
export DEBIAN_FRONTEND=noninteractive QT_SELECT=qt5 LC_ALL=C.UTF-8
# Qt 5's qmake, which the configure scripts call too
export PATH=/usr/lib/qt5/bin:$PATH
S=/usr/src/harmattan-qt5 B=/usr/src/hq5-build D=/usr/src/hq5-debs
set -e
[ -d $S/.git ] || git clone -q "$url" $S
git -C $S cat-file -e "$rev^{commit}" 2> /dev/null || git -C $S fetch -q origin
git -C $S checkout -q -f "$rev"
set +e
mkdir -p $B $D
# libmeegotouch and the others install their qmake features below Qt's
# data directory (/usr/share/qt5), where Debian's qmake does not look
export QMAKEFEATURES=$(qmake -query QT_INSTALL_DATA)/mkspecs/features
failed=

# mkdeb NAME VERSION ROOT [control lines...]: ROOT as package NAME; its
# Depends from the libraries its ELF files need (dpkg -S: Leste's
# packages and the parts installed before it)
mkdeb() {
	local name=$1 version=$2 root=$3 d f deps so p
	shift 3
	d=$B/pkg/$name
	rm -rf "$d"
	mkdir -p "$d/DEBIAN"
	cp -a "$root/." "$d/"
	find "$d" -type f ! -path "$d/DEBIAN/*" -exec sh -c \
	    'for f; do file -b "$f" | grep -q "^ELF" && strip --strip-unneeded "$f" 2> /dev/null; done; true' sh {} +
	deps=$(find "$d" -type f ! -path "$d/DEBIAN/*" -exec sh -c \
	    'for f; do file -b "$f" | grep -q "^ELF" && objdump -p "$f" | awk "/NEEDED/ {print \$2}"; done' sh {} + |
	    sort -u | while read -r so; do
		[ -n "$(find "$d" -name "$so" -print -quit)" ] && continue
		p=$(dpkg -S "*/$so" 2> /dev/null | grep -v '^diversion' | head -1 | cut -d: -f1)
		[ -n "$p" ] && echo "$p"
	done | sort -u | paste -sd, - | sed 's/,/, /g')
	{
		echo "Package: $name"
		echo "Version: $version"
		echo "Architecture: arm64"
		echo "Section: x11"
		echo "Priority: optional"
		echo "Maintainer: Sebastian Matkovich <sebastian.matkovich@gmail.com>"
		echo "Homepage: https://github.com/smatkovi/harmattan-qt5"
		echo "Installed-Size: $(du -sk --exclude=DEBIAN "$d" | cut -f1)"
		[ -z "$deps" ] || echo "Depends: $deps"
		for f; do echo "$f"; done
		echo "Description: Nokia N9 user interface (Qt 5 port): $name"
	} > "$d/DEBIAN/control"
	# configuration files, libraries (ldconfig), GConf schemas
	[ ! -d "$d/etc" ] || (cd "$d" && find etc -type f | sed 's|^|/|') > "$d/DEBIAN/conffiles"
	[ -s "$d/DEBIAN/conffiles" ] || rm -f "$d/DEBIAN/conffiles"
	[ -z "$(find "$d" -name '*.so.*' -print -quit)" ] ||
	    echo "activate-noawait ldconfig" > "$d/DEBIAN/triggers"
	if [ -d "$d/usr/share/gconf/schemas" ]; then
		printf '#!/bin/sh\nset -e\nif [ "$1" = configure ] && command -v gconftool-2 > /dev/null; then\n\tGCONF_CONFIG_SOURCE=xml:merged:/etc/gconf/gconf.xml.defaults gconftool-2 --makefile-install-rule %s > /dev/null || true\nfi\n' \
		    "$(cd "$d" && ls usr/share/gconf/schemas/*.schemas | sed 's|^|/|' | paste -sd' ' -)" \
		    > "$d/DEBIAN/postinst"
		chmod 755 "$d/DEBIAN/postinst"
	fi
	dpkg-deb --root-owner-group -Zxz --build "$d" "$D/${name}_${version}_arm64.deb" > /dev/null &&
	    dpkg -i "$D/${name}_${version}_arm64.deb" > "$B/$name.dpkg.log" 2>&1 &&
	    ldconfig
}

# want NAME STAMP: build it (not built at STAMP, or in ONLY)
want() {
	case " $only " in *" $1 "*) return 0 ;; esac
	[ "$(cat $B/$1.done 2> /dev/null)" != "$2" ] && return 0
	echo "   $1: built"
	return 1
}

# part NAME KIND [arguments]: the part built in a copy of its source,
# installed into its own root, packaged and installed (arch/30-meego.sh's
# part(), into a root of its own instead of /)
part() {
	local name=$1 kind=$2 stamp v pr
	shift 2
	stamp="$(git -C $S log -1 --format=%H -- "$name") $pkgrev"
	want "$name" "$stamp" || return 0
	echo "   $name"
	rm -rf "$B/$name"
	cp -a "$S/$name" "$B/$name"
	pr=$B/root/$name
	rm -rf "$pr"
	mkdir -p "$pr"
	(
		cd "$B/$name" || exit 1
		case $kind in
		configure)
			./configure "$@" > configure.log 2>&1 || { tail -20 configure.log; exit 1; } ;;
		qmake)
			# NOTESTS for most parts, notests for the reaction maps
			qmake -r "$@" CONFIG+=NODOCS CONFIG+=NOTESTS CONFIG+=notests INCLUDEPATH+=/usr/include/meegotouch \
			    INCLUDEPATH+=/usr/include/duicontrolpanel > qmake.log 2>&1 ||
			    { tail -20 qmake.log; exit 1; } ;;
		autotools)
			autoreconf -i -f > autoreconf.log 2>&1 || { tail -20 autoreconf.log; exit 1; }
			./configure --prefix=/usr --sysconfdir=/etc "$@" > configure.log 2>&1 ||
			    { tail -20 configure.log; exit 1; } ;;
		esac
		make -j"$jobs" > make.log 2>&1 ||
		    { grep -m10 -e "error:" -e "\*\*\*" make.log; exit 1; }
		# qmake's Makefiles take INSTALL_ROOT (DESTDIR is their build
		# output there), the others DESTDIR
		case $kind in
		configure|qmake) make install INSTALL_ROOT="$pr" > install.log 2>&1 ;;
		*) make install DESTDIR="$pr" > install.log 2>&1 ;;
		esac || { tail -20 install.log; exit 1; }
	) || { failed="$failed $name"; echo "     $name failed"; return 1; }
	v=$(cd "$B/$name" && dpkg-parsechangelog -S Version 2> /dev/null) || v=
	[ -n "$v" ] || v=0.1
	v=${v#*:}
	part_fixup "$name" "$pr"
	mkdeb "harmattan-$name" "$v+pro1x$pkgrev" "$pr" ${CONTROL:+"$CONTROL"} ||
	    { failed="$failed $name(package)"; tail -5 "$B/harmattan-$name.dpkg.log" 2> /dev/null; return 1; }
	echo "$stamp" > "$B/$name.done"
}

# what a part's root needs before it is packaged
part_fixup() {
	case $1 in
	meegofeedback-reactionmaps)
		# the XRecord source only: meegofeedbackd plays every source it
		# loads, and the kernel one reads the touch screen only in the
		# multi-touch protocol A
		rm -f "$2"/usr/lib/meegofeedbackd/sources/libmeegofeedback-reactionmap.so ;;
	esac
	# the test suites (Nokia's, some on stub libraries of their build)
	find "$2" -depth -type d -name '*-tests' -exec rm -rf {} + 2> /dev/null
	return 0
}

# upstream NAME TAG [configure arguments]: Sailfish OS's ngfd and libngf
# at their tags (Debian 12 has neither), packaged as NAME
upstream() {
	local name=$1 tag=$2 pr
	shift 2
	want "$name" "$tag $pkgrev" || return 0
	echo "   $name $tag"
	rm -rf "$B/$name"
	pr=$B/root/$name
	rm -rf "$pr"
	mkdir -p "$pr"
	(
		git clone -q --branch "$tag" "https://github.com/sailfishos/$name.git" "$B/$name" || exit 1
		cd "$B/$name" || exit 1
		git submodule update -q --init
		./autogen.sh > autogen.log 2>&1 || autoreconf -i -f > autogen.log 2>&1
		./configure --prefix=/usr --sysconfdir=/etc "$@" > configure.log 2>&1 ||
		    { tail -20 configure.log; exit 1; }
		make -j"$jobs" > make.log 2>&1 || { grep -m10 -E "error:|\*\*\*" make.log; exit 1; }
		make install DESTDIR="$pr" > install.log 2>&1 || { tail -20 install.log; exit 1; }
	) || { failed="$failed $name"; echo "     $name failed"; return 1; }
	# ngfd with the plugins that run here (harmattan-qt5/ngfd-meego)
	[ "$name" != ngfd ] || install -D -m 644 $S/ngfd-meego/ngfd.ini "$pr/usr/share/ngfd/ngfd.ini"
	find "$pr" -name '*.la' -delete
	mkdeb "$name" "${tag#v}+pro1x$pkgrev" "$pr" ||
	    { failed="$failed $name(package)"; return 1; }
	echo "$tag $pkgrev" > "$B/$name.done"
}

part libmlocale configure -prefix /usr -release -nomake demos -nomake tests -nomake benchmarks
part libmeegotouch configure -prefix /usr -release -no-werror -nomake demos -nomake tests -nomake benchmarks
part contextkit autotools --disable-doc --disable-coverage
part libcontentaction autotools --disable-doc --disable-coverage
part mcompositor qmake mcompositor.pro
# libdsme: Leste's (libdsme0.2.0, which Leste's dsme uses; the same
# sonames as Harmattan's), qmsystem2 is built against it
# Harmattan's mce names are newer than Leste's mce-dev (1.10): in its place
CONTROL="Provides: mce-dev
Conflicts: mce-dev
Replaces: mce-dev" part mce-dev make
part qmsystem2 qmake libqmsystem2.pro
upstream libngf 0.29
CPPFLAGS='-DMCE_LED_PATTERN_DEACTIVATED_SIG=\"led_pattern_deactivated_ind\"' \
	upstream ngfd 1.4.13 --disable-systemd
part duicontrolpanel qmake projects.pro
part system-ui qmake project.pro DEFINES+=HAVE_LIBNGF
part statusmenu-plugins qmake statusmenu-plugins.pro
part meegotouchhome qmake projects.pro
part meegotouch-controlpanelapplets qmake meegotouch-controlpanelapplets.pro
part meegofeedbackd qmake meegofeedbackd.pro
part meegofeedback-reactionmaps qmake meegofeedback-reactionmaps.pro
part meego-im-framework qmake maliit-framework.pro CONFIG+=enable-legacy CONFIG+=notests \
	CONFIG+=enable-contextkit CONFIG+=nodoc
part libmeegoimengine qmake meegoimengine.pro
part meego-keyboard qmake m-keyboard.pro
part meego-im-settings-applet qmake mim-settings-applet.pro
part meegotouchtheme qmake theme.pro

missing=
for f in mcompositor sysuid meegotouchhome meegofeedbackd ngfd meego-im-uiserver; do
	command -v $f > /dev/null || missing="$missing $f"
done
# (grep reads it all: grep -q would end the pipe early, SIGPIPE under pipefail)
ldconfig -p | grep libmeegotouchcore > /dev/null || missing="$missing libmeegotouchcore"
echo "failed:${failed:- none}"
echo "missing:${missing:- none}"
[ -z "$failed$missing" ]
EOF
log "packages to $OUT"
cp -a "$R/usr/src/hq5-debs/." "$OUT/"
ls "$OUT"
exit $rc
