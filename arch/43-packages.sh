#!/bin/bash
# Pacman packages of everything the Arch steps built in the image, as
# 42-meego-look.sh's OUT= makes them for its few parts: one package per
# part, of the files the part installs (into a root of its own: 41's and
# 42's are there, 30's and 40's are installed once more from their build
# trees), its version, description and licence from the part's debian/
# directory, its dependencies from the libraries it links (arch/pkgbuild.py).
# Each package is installed into the image as soon as it is made (pacman
# -U --overwrite '*': the steps put the files there without pacman), so
# that the packages after it name it as a dependency; installed in the
# order they are made, the packages need nothing but Arch Linux ARM's.
#
# As root, after the steps whose parts it packages: on the PC with the
# chroot of arch/pc-chroot.sh on $MNT, or in Leste with the image there:
#
#   OUT=/dir bash arch/43-packages.sh              every part built
#   OUT=/dir bash arch/43-packages.sh 30 41:alarmd  30's parts, one of 41's
#   CLEAN=1 ...   a part's build tree removed once it is packaged (30's
#                 and 40's then cannot be packaged again without a build)
#
# A part is STEP:NAME, a STEP alone all of that step's built parts, in
# their build order: 30 (the N9's user interface, harmattan-<part>), 40
# (Hildon), 41 (the apps), 42 (the MeeGo look's tools), extra: gconf
# (step 30's or 40's from the AUR), vte (as vte-gtk2), libical3,
# qt5-positioning and python-pyotherside (41's libraries; the last from the
# AUR), pro1x-sessions (the sessions' scripts and units). A
# part packaged before at the same build is skipped (FORCE=1: again). A
# part that fails is listed at the end and does not stop the others.
# Nokia's theme, fonts and sounds (step 30 copies them from the phone's
# own Harmattan image) are in no package.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
MNT=${MNT:-/mnt/arch}
OUT=${OUT:?OUT=/dir: where the packages go}
CLEAN=${CLEAN:-0}
FORCE=${FORCE:-0}
COMMIT=${COMMIT:-$(git -C "$REPO" rev-parse --short HEAD 2> /dev/null || echo unknown)}
B=/usr/src/hildon
H=/usr/src/harmattan-qt5-build
HS=/usr/src/harmattan-qt5
P=/usr/src/pro1x-pkg
log() { echo ">> $*"; }

mountpoint -q "$MNT" || { echo "$MNT is not mounted"; exit 1; }
# (only what it mounts is unmounted at the end: a step that runs beside it
# keeps its own)
mounted=
for m in proc sys dev dev/pts; do
	mountpoint -q "$MNT/$m" && continue
	mount --bind "/$m" "$MNT/$m"
	# a slave of the system's: unmounting dev/pts here would otherwise
	# take the system's /dev/pts along (a bind of a shared /dev is its peer)
	mount --make-rslave "$MNT/$m"
	mounted="$m $mounted"
done
cleanup() {
	[ -z "$mounted" ] || chroot "$MNT" gpgconf --homedir /etc/pacman.d/gnupg --kill all 2> /dev/null || true
	for m in $mounted; do
		! mountpoint -q "$MNT/$m" || umount "$MNT/$m" || umount -l "$MNT/$m"
	done
}
trap cleanup EXIT
in_root() { chroot "$MNT" /bin/bash "$@"; }
mkdir -p "$OUT" "$MNT$P/root" "$MNT$P/pkg" "$MNT$P/done" "$MNT$P/log"

# --- tables ------------------------------------------------------------------------
# the +pro1xN of Leste's packages of the parts with this repository's
# patches (scripts/49-hidpi.sh), the versions of its own tools
declare -A PRO1X=()
eval "$(sed -n 's/^ *"\([a-z0-9-]*\)|[0-9a-f]*|\([0-9]*\)".*/PRO1X[\1]=\2/p' "$REPO/scripts/49-hidpi.sh")"
declare -A VERSIONS=([pro1x-fm]=1.0 [pro1x-sessions]=1.0 [meego-clock]=0.1 [statusmenu-plugins]=0.1
	[libngf]=0.29 [ngfd]=1.4.13 [vte]=0.28.2 [libical3]=3.0.20 [qt5-positioning]=5.15.19)
eval "$(sed -n 's/^declare -A VERSIONS=(\(.*\))$/VERSIONS+=(\1)/p' "$REPO/scripts/mkdeb-tools.sh")"
# the parts' repositories: 40's (freebsd/hildon/build.sh), 41's (apps.sh,
# as 41 wrote them out)
declare -A URL=()
eval "$(grep '^URL\[[a-z0-9.-]*\]=' "$REPO/freebsd/hildon/build.sh")"
[ ! -f "$MNT$B/apps-tables.sh" ] ||
	eval "$(sed -n 's/^declare -A URL=/URL+=/p' "$MNT$B/apps-tables.sh")"
# package names: harmattan-<part> for 30's; Arch Linux ARM's name taken:
# <part>-maemo in its place (below); GTK 2 as FreeBSD has it
declare -A T_NAME=([gtk]=gtk2-maemo [vte]=vte-gtk2)
declare -A T_DESC=(
	[gconf]="GNOME's old configuration system (the AUR's PKGBUILD)"
	[python-pyotherside]="Asynchronous Python 3 bindings for Qt 5's QML (the AUR's PKGBUILD)"
	[vte]="GTK 2's terminal widget (VTE 0.28), which osso-xterm is written for"
	[libical3]="libical 3 below /opt/libical3 (Maemo's calendar is written for its API)"
	[qt5-positioning]="Qt 5's positioning module with its GeoClue 2 plugin (from qtlocation)"
	[pro1x-sessions]="The Hildon, MeeGo-look and N9 (Harmattan) X11 sessions of the F(x)tec Pro1X's Arch, pro1x-desktop, pro1x-hidpi"
	[pro1x-events]="The Nokia N9's Events page for Hildon's MeeGo look, with its settings applet"
	[pro1x-volume-osd]="The Nokia N9's volume bar for Hildon's MeeGo look"
	[pro1x-fm]="FM radio for Hildon: the F(x)tec Pro1X's WCN3950 receiver (/dev/radio0)"
	[libngf]="Sailfish OS's client library of the event sound daemon ngfd"
	[ngfd]="Sailfish OS's event sound and vibra daemon (Nemo's successor of the N9's)"
	[harmattan-statusmenu-plugins]="The N9's status menu rows: volume, internet connection, Bluetooth"
	[qt-components]="The MeeGo Harmattan Qt Quick components, com.nokia.meego, ported to Qt 5"
	[meego-clock]="The Nokia N9's clock for Hildon, in QML with com.nokia.meego"
	[harmattan-meego-browser]="The Nokia N9's browser in QML (Qt WebEngine or WebKit where there)"
	[harmattan-meego-calculator]="The Nokia N9's calculator in QML"
	[harmattan-meego-calendar]="The Nokia N9's calendar in QML"
	[harmattan-meego-camera]="The Nokia N9's camera in QML on GStreamer"
	[harmattan-meego-contacts]="The Nokia N9's contacts in QML"
	[harmattan-meego-gallery]="The Nokia N9's gallery in QML"
	[harmattan-meego-mail]="The Nokia N9's mail in QML"
	[harmattan-meego-maps]="The Nokia N9's maps in QML on OpenStreetMap or OSM Scout Server"
	[harmattan-meego-messages]="The Nokia N9's messages in QML"
	[harmattan-meego-music]="The Nokia N9's music player in QML"
	[harmattan-meego-notes]="The Nokia N9's notes in QML"
	[harmattan-meego-phone]="The Nokia N9's phone in QML"
	[harmattan-meego-video]="The Nokia N9's video player in QML"
)
declare -A T_LICENSE=([gconf]=LGPL-2.0-or-later [vte]=LGPL-2.0-or-later
	[libical3]="MPL-2.0 OR LGPL-2.1-only" [qt5-positioning]="LGPL-3.0-only OR GPL-2.0-or-later"
	[pro1x-sessions]=LicenseRef-pro1x-maemo-leste [pro1x-events]=LicenseRef-pro1x-maemo-leste
	[pro1x-volume-osd]=LicenseRef-pro1x-maemo-leste [pro1x-fm]=LicenseRef-pro1x-maemo-leste
	[libngf]=LGPL-2.1-or-later [ngfd]=LGPL-2.1-or-later [qt-components]=BSD-3-Clause
	[meego-clock]=BSD-3-Clause)
# Harmattan's: LGPL 2.1 (LICENSE.LGPL, the sources' headers)
for p in duicontrolpanel libcontentaction libmeegotouch libmlocale mcompositor \
    meegotouch-controlpanelapplets meegotouchhome meegotouchtheme qmsystem2 \
    statusmenu-plugins system-ui; do
	T_LICENSE[harmattan-$p]=LGPL-2.1-only
done
# what the files need besides the libraries they link (by package)
declare -A T_DEPENDS=(
	[gtk2-maemo]=gtk-update-icon-cache
	[harmattan-system-ui]=ngfd
	[harmattan-meego-phone]=libpulse
	[harmattan-meego-mail]=ca-certificates [harmattan-meego-maps]=ca-certificates
	[harmattan-meego-music]="gst-plugins-base gst-plugins-good"
	[harmattan-meego-video]="gst-plugins-base gst-plugins-good"
	[harmattan-meego-camera]="gst-plugins-base gst-plugins-good"
	[meego-clock]="alarmd clockd"
	[osso-chess-ui]=gnuchess [osso-lmarbles]=ttf-dejavu
	[gpodder-leste]="python python-pyotherside qt5-declarative qt5-multimedia qt5-graphicaleffects gst-plugins-good fontconfig"
	[wifi-assistant]="python python-dbus python-gobject"
	[yappari-leste]="dbus qt5-multimedia"
	[pro1x-fm]="libpulse alsa-utils python"
)
declare -A T_OPTDEPENDS=(
	[harmattan-meego-messages]="modemmanager:%the%modem's%SMS"
	[harmattan-meego-phone]="modemmanager:%the%modem's%calls"
	[harmattan-meego-maps]="geoclue:%the%position"
	[harmattan-meego-video]="gst-libav:%more%video%formats"
	[yappari-leste]="noto-fonts-emoji:%emoji"
)
# left out of a package (globs below its root)
declare -A T_EXCLUDE=(
	# Harmattan's libdsme beside Leste's (41's): without the development
	# files both have (as FreeBSD's harmattan-libdsme)
	[harmattan-libdsme]="usr/include/dsme/dsme_dbus_if.h usr/lib/libdsme_dbus_if.so usr/lib/pkgconfig/dsme_dbus_if.pc"
	# Arch has it in a package of its own; GTK's API documentation
	[gtk2-maemo]="usr/bin/gtk-update-icon-cache usr/share/man/man1/gtk-update-icon-cache.1* usr/share/gtk-doc"
	# the N9's restore-factory-settings script
	[harmattan-meegotouch-controlpanelapplets]="*mcpapplets-rfs.sh"
	# the event log's test suite (Leste packages it apart)
	[rtcom-eventlogger]="usr/bin/rtcom-eventlogger-testsuite usr/lib/tests"
	# the connection manager's test programs, static library and headers
	[telepathy-ring]="opt usr/lib/libmodem-glib.a usr/include/modem-glib usr/share/man/man8/telepathy-ring.8"
)

# --- the parts --------------------------------------------------------------------
parts30() {
	sed -n -e 's/^part \([a-z0-9-]*\) .*/\1/p' -e 's/^upstream \([a-z0-9-]*\) .*/\1/p' \
	    -e 's/^[^#]*upstream \(ngfd\) .*/\1/p' \
	    -e 's/^for a in \(.*\); do$/\1/p' "$REPO/arch/30-meego.sh" |
	while read -r n rest; do
		if [ -n "$rest" ]; then for a in $n $rest; do echo "meego-$a"; done
		else echo "$n"; fi
	done
}
parts_of() {
	case $1 in
	30) parts30 ;;
	40) [ ! -f "$MNT$B/parts" ] || cut -d'|' -f1 "$MNT$B/parts" ;;
	41) [ ! -f "$MNT$B/apps-list" ] || cut -d'|' -f1 "$MNT$B/apps-list" ;;
	42) printf '%s\n' pro1x-events pro1x-volume-osd pro1x-fm ;;
	extra) printf '%s\n' gconf vte libical3 qt5-positioning python-pyotherside pro1x-sessions ;;
	esac
}
# the build's stamp of a part (empty: not built)
stamp_of() {
	local step=$1 name=$2
	case $step in
	30) cat "$MNT$H/$name.done" 2> /dev/null || true ;;
	40) cat "$MNT$B/$name.done" 2> /dev/null || true ;;
	41) [ -d "$MNT$B/root/$name" ] && cat "$MNT$B/$name.done" 2> /dev/null || true ;;
	42) [ -d "$MNT$B/root/$name" ] && echo "${VERSIONS[$name]} $COMMIT" || true ;;
	extra)
		case $name in
		gconf|python-pyotherside)
			ls "$MNT/home/alarm/aur/$name/$name"-[0-9]*.pkg.tar.* 2> /dev/null | tail -1 || true ;;
		vte) [ ! -f "$MNT$B/build/vte-0.28.2/Makefile" ] || echo 0.28.2 ;;
		libical3) [ ! -d "$MNT$B/build/libical-3.0.20/_build" ] || echo 3.0.20 ;;
		qt5-positioning) [ ! -d "$MNT$B/build/qtlocation/_build/_stage" ] || echo 5.15.19 ;;
		pro1x-sessions) echo "${VERSIONS[pro1x-sessions]} $COMMIT" ;;
		esac ;;
	esac
}

# the part's files into $P/root/<package> (in the image)
stage() {
	local step=$1 name=$2 pkg=$3 pr=$P/root/$3
	rm -rf "${MNT:?}$pr"
	mkdir -p "$MNT$pr"
	case $step in
	30) in_root -euo pipefail -s "$H/$name" "$pr" "$name" "$HS" <<'EOF'
d=$1 pr=$2 name=$3 hs=$4
cd "$d"
# make runs qmake again where the files a Makefile was made from are newer
# (the packages installed before this one have makepkg's times): with the
# MeeGo features where 30 had them, or the parts link without libmeegotouch
export QMAKEFEATURES=/usr/share/qt/mkspecs/features
# qmake's Makefiles take INSTALL_ROOT (DESTDIR is their build output
# there), the others DESTDIR
if head -5 Makefile | grep qmake > /dev/null; then make install INSTALL_ROOT="$pr"
else make install DESTDIR="$pr"; fi
case $name in
meegofeedback-reactionmaps)
	# the XRecord source only, as 30 leaves it
	rm -f "$pr"/usr/lib/meegofeedbackd/sources/libmeegofeedback-reactionmap.so ;;
ngfd)
	install -D -m 644 "$hs/ngfd-meego/ngfd.ini" "$pr/usr/share/ngfd/ngfd.ini" ;;
mce-dev)
	# Harmattan's mce names beside Leste's (40's mce-dev), as FreeBSD's
	# harmattan-mce-dev: below include/harmattan-mce
	mkdir -p "$pr/usr/include/harmattan-mce"
	mv "$pr/usr/include/mce" "$pr/usr/include/harmattan-mce/mce"
	sed -e 's|^includedir=.*|includedir=${prefix}/include/harmattan-mce|' -e 's/^Name: .*/Name: harmattan-mce/' \
	    "$pr/usr/lib/pkgconfig/mce.pc" > "$pr/usr/lib/pkgconfig/harmattan-mce.pc"
	rm "$pr/usr/lib/pkgconfig/mce.pc" ;;
esac
# the test suites (Nokia's, some on stub libraries of their build)
find "$pr" -depth -type d -name '*-tests' -exec rm -rf {} +
EOF
		;;
	40) in_root -euo pipefail -s "$B/build/$name" "$pr" "$name" <<'EOF'
d=$1 pr=$2 name=$3
cd "$d"
case $name in
mce-dev)
	# (as 40 installs it: mce's D-Bus names only)
	v=$(sed -n '1s/^[^(]*(\([^)]*\)).*/\1/p' debian/changelog)
	install -d "$pr/usr/include/mce" "$pr/usr/lib/pkgconfig"
	install -m 644 src/include/mce/*.h "$pr/usr/include/mce/"
	printf 'prefix=/usr\nincludedir=${prefix}/include\n\nName: mce\nDescription: Mode Control Entity\nVersion: %s\nCflags: -I${includedir}\n' \
		"$v" > "$pr/usr/lib/pkgconfig/mce.pc" ;;
profiled) make install ROOT="$pr" ;;
*) make install DESTDIR="$pr" ;;
esac
EOF
		;;
	41|42) cp -a "$MNT$B/root/$name/." "$MNT$pr/" ;;
	extra)
		case $name in
		vte) in_root -euo pipefail -c "make -C $B/build/vte-0.28.2 install DESTDIR=$pr" ;;
		libical3) in_root -euo pipefail -c "DESTDIR=$pr cmake --install $B/build/libical-3.0.20/_build &&
			install -D -m 644 /etc/ld.so.conf.d/libical3.conf $pr/etc/ld.so.conf.d/libical3.conf" ;;
		qt5-positioning) cp -a "$MNT$B/build/qtlocation/_build/_stage/." "$MNT$pr/" ;;
		pro1x-sessions)
			install -D -m 755 "$REPO/arch/hildon/hildon-session" "$MNT$pr/usr/local/bin/hildon-session"
			install -D -m 755 "$REPO/arch/harmattan/harmattan-session" "$MNT$pr/usr/local/bin/harmattan-session"
			install -D -m 755 "$REPO/arch/pro1x-desktop" "$MNT$pr/usr/local/bin/pro1x-desktop"
			install -D -m 755 "$REPO/tools/pro1x-hidpi/pro1x-hidpi" "$MNT$pr/usr/local/bin/pro1x-hidpi"
			[ ! -x "$MNT/usr/local/bin/pro1x-theme-scale" ] ||
			    install -D -m 755 "$MNT/usr/local/bin/pro1x-theme-scale" "$MNT$pr/usr/local/bin/pro1x-theme-scale"
			install -d "$MNT$pr/etc/systemd/system"
			install -m 644 "$REPO/arch/hildon/hildon.service" "$REPO/arch/hildon/meego.service" \
			    "$REPO/arch/harmattan/harmattan.service" \
			    "$REPO/kali/rootfs-overlay/etc/systemd/system/pro1x-rtc.service" "$MNT$pr/etc/systemd/system/" ;;
		esac ;;
	esac
}

# the part's source (debian/ for version, description and licence)
src_of() {
	local step=$1 name=$2
	case $step in
	30) if [ -d "$MNT$HS/$name" ]; then echo "$HS/$name"
	    elif [ -d "$MNT$HS/apps/$name" ]; then echo "$HS/apps/$name"
	    else echo "$H/$name"; fi ;;
	40|41) echo "$B/src/$name" ;;
	*) echo "" ;;
	esac
}

# NAME of the package of STEP's part NAME
pkgname_of() {
	local step=$1 name=$2
	case $step in
	30) case $name in libngf|ngfd) echo "$name" ;; *) echo "harmattan-$name" ;; esac ;;
	*) echo "${T_NAME[$name]:-$name}" ;;
	esac
}

failed=
made=
one() {
	local step=$1 name=$2 st pkg src v url prov conf f
	st=$(stamp_of "$step" "$name")
	if [ -z "$st" ]; then echo "   $name: not built"; return 0; fi
	pkg=$(pkgname_of "$step" "$name")
	if [ "$FORCE" != 1 ] && [ "$(cat "$MNT$P/done/$pkg" 2> /dev/null)" = "$st" ] &&
	    ls "$OUT/$pkg"-[0-9]*.pkg.tar.* > /dev/null 2>&1; then
		echo "   $pkg: packaged"; return 0
	fi
	echo "   $pkg"
	if [ "$step:$name" = extra:gconf ] || [ "$step:$name" = extra:python-pyotherside ]; then
		# the AUR's package as makepkg made it
		cp "$st" "$OUT/"
		echo "$st" > "$MNT$P/done/$pkg"
		made="$made $pkg"
		return 0
	fi
	# Arch Linux ARM has a package of the name: <name>-maemo in its place
	prov= conf=
	if [ "$pkg" = gtk2-maemo ]; then prov=gtk2 conf=gtk2
	elif [ "$step" != 30 ] && [ "$step" != extra ] && in_root -c "pacman -Si -- '$pkg'" > /dev/null 2>&1; then
		prov=$pkg conf=$pkg pkg=$pkg-maemo
		echo "     (Arch Linux ARM has a $prov: $pkg)"
	fi
	stage "$step" "$name" "$pkg" > "$MNT$P/log/$pkg.stage" 2>&1 ||
	    { tail -5 "$MNT$P/log/$pkg.stage" | sed 's/^/     /'; failed="$failed $pkg(install)"; return 0; }
	src=$(src_of "$step" "$name")
	v=${VERSIONS[$name]:-}
	if [ "$name" = qt-components ]; then
		v="1.4.9+git$(git -C "$MNT$src" rev-parse --short=7 HEAD 2> /dev/null || echo 0)"
	fi
	# Leste's +pro1xN of the parts with this repository's patches
	if [ -z "$v" ] && [ -n "${PRO1X[$name]:-}" ] && [ -f "$MNT$src/debian/changelog" ]; then
		v="$(sed -n '1s/^[^(]*(\([^)]*\)).*/\1/p' "$MNT$src/debian/changelog")+pro1x${PRO1X[$name]}"
	fi
	if [ -z "$v" ] && [ "$step" = 30 ] && [ ! -f "$MNT$src/debian/changelog" ]; then v=0.1; fi
	if [ -z "$v" ] && [ -n "$src" ] && [ ! -f "$MNT$src/debian/changelog" ] && [ -d "$MNT$src/.git" ]; then
		v="0+git$(git -C "$MNT$src" rev-parse --short=7 HEAD)"
	fi
	case $step in
	30) url=https://github.com/smatkovi/harmattan-qt5 ;;
	40|41) url=${URL[$name]:-https://github.com/maemo-leste/$name.git}; url=${url%.git} ;;
	*) url=https://github.com/smatkovi/pro1x-maemo-leste ;;
	esac
	case $name in libngf|ngfd|qt-components) url=https://github.com/sailfishos/$name ;; esac
	[ "$name" != vte ] || url=https://download.gnome.org/sources/vte/0.28/
	[ "$name" != libical3 ] || url=https://github.com/libical/libical
	[ "$name" != qt5-positioning ] || url=https://download.qt.io/archive/qt/5.15/
	in_root -c "ldconfig && ldconfig -p > $P/ld.cache"
	rm -rf "${MNT:?}$P/pkg/$pkg"
	env PKGNAME="$pkg" PART="$name" SRC="$src" VERSION="$v" \
	    DESC="${T_DESC[$pkg]:-${T_DESC[$name]:-}}" LICENSE="${T_LICENSE[$pkg]:-${T_LICENSE[$name]:-}}" \
	    URL="$url" DEPENDS="${T_DEPENDS[$pkg]:-}" OPTDEPENDS="${T_OPTDEPENDS[$pkg]:-}" \
	    PROVIDES="$prov" CONFLICTS="$conf" EXCLUDE="${T_EXCLUDE[$pkg]:-}" \
	    LDCACHE="$P/ld.cache" COMMIT="$COMMIT" STAGE_IN="$P/root/$pkg" \
	    python3 -I "$REPO/arch/pkgbuild.py" "$MNT" "$P/root/$pkg" "$P/pkg/$pkg" ||
	    { failed="$failed $pkg(PKGBUILD)"; return 0; }
	in_root -euo pipefail -s "$P" "$pkg" <<'EOF' || { failed="$failed $pkg"; return 0; }
P=$1 pkg=$2
chown -R alarm:alarm "$P/pkg/$pkg"
su alarm -c "cd '$P/pkg/$pkg' && PACKAGER='pro1x-maemo-leste <sebastian.matkovich@gmail.com>' \
    makepkg -d -f --noconfirm > makepkg.log 2>&1" ||
    { tail -15 "$P/pkg/$pkg/makepkg.log" | sed 's/^/     /'; exit 1; }
f=$(ls "$P/pkg/$pkg/$pkg"-[0-9]*-aarch64.pkg.tar.* | tail -1)
pacman -U --noconfirm --overwrite '*' "$f" > "$P/log/$pkg.pacman" 2>&1 ||
    { tail -10 "$P/log/$pkg.pacman" | sed 's/^/     /'; exit 1; }
EOF
	f=$(ls "$MNT$P/pkg/$pkg/$pkg"-[0-9]*-aarch64.pkg.tar.* | tail -1)
	rm -f "$OUT/$pkg"-[0-9]*.pkg.tar.*
	cp "$f" "$OUT/"
	echo "$st" > "$MNT$P/done/$pkg"
	made="$made $pkg"
	rm -rf "${MNT:?}$P/root/$pkg"
	if [ "$CLEAN" = 1 ]; then
		case $step in
		30) rm -rf "${MNT:?}$H/$name" ;;
		40) rm -rf "${MNT:?}$B/build/$name" ;;
		41|42) rm -rf "${MNT:?}$B/build/$name" "${MNT:?}$B/root/$name" ;;
		esac
	fi
}

[ $# -gt 0 ] || set -- extra:gconf 30 40 extra:vte extra:libical3 extra:qt5-positioning \
	extra:python-pyotherside 41 42 extra:pro1x-sessions
log "packages into $OUT (pro1x-maemo-leste $COMMIT)"
for spec; do
	case $spec in
	*:*) one "${spec%%:*}" "${spec#*:}" ;;
	*) for name in $(parts_of "$spec"); do one "$spec" "$name"; done ;;
	esac
done
in_root -c "gpgconf --homedir /etc/pacman.d/gnupg --kill all" 2> /dev/null || true
echo "made:${made:- none}"
echo "failed:${failed:- none}"
