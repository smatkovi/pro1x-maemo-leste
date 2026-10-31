#!/bin/bash
# The MeeGo look's own parts and the FM radio for the Hildon sessions on
# Arch, as Leste has them (scripts/49-events.sh, 49-volume-osd.sh, the FM
# lines of 49-apps.sh):
#
#   pro1x-events      the Nokia N9's Events page (tools/pro1x-events: the
#                     date, the weather, notifications, the calendar's
#                     appointments through calendar-backend, a Mastodon
#                     timeline) and its settings applet (apps/pro1x-events)
#   pro1x-volume-osd  the N9's volume bar (tools/pro1x-volume-osd)
#   pro1x-fm          the FM radio (apps/pro1x-fm: the WCN3950's receiver,
#                     /dev/radio0, its sound through PipeWire's PulseAudio
#                     server) with its test tools (tools/pro1x-fm)
#   pro1x-alarmboot   alarms that switch the phone on (tools/pro1x-alarmboot,
#                     docs/alarm-power-on.md): the N9's alarm screens after
#                     such a start, then off again; with librtc-realtime
#                     for alarmd (tools/pro1x-rtc) and what alarmd needs as
#                     the session's user (arch/alarm: its D-Bus policy, its
#                     queue's directory, rtc0)
#
# hildon-session starts the first two; both show only while the MeeGo look
# is on (meego.service). After 40-hildon.sh and 41-hildon-apps.sh, which
# build again what has the MeeGo look's patches or the calendar fix
# (libhildon, hildon-status-menu, hildon-desktop; calendar-backend) and,
# with ONLY, what the Events page needs (rtcom-eventlogger). Then the
# session's files and the clock's unit (pro1x-rtc waits for rtc0).
#
# As root: in Leste with the image on $MNT, on the PC with a chroot of
# Arch Linux ARM (arch/pc-chroot.sh), or in the running Arch (MNT=/):
#
#   bash arch/42-meego-look.sh             build and install
#   OUT=/dir bash arch/42-meego-look.sh    also pacman packages of them and
#                                          of the parts above, into /dir
#   PKGS=/dir bash arch/42-meego-look.sh   install such packages (the
#                                          release's) instead of building
#   ONLY="pro1x-alarmboot" ...             build (and package) only these
#                                          of this step's own tools
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
MNT=${MNT:-/mnt/arch}
OUT=${OUT:-}
PKGS=${PKGS:-}
TOOLS="pro1x-events pro1x-volume-osd pro1x-fm pro1x-alarmboot"
ONLY=${ONLY:-}
# the repository's commit the packages name (REPO may be a git archive)
COMMIT=${COMMIT:-$(git -C "$REPO" rev-parse --short HEAD 2> /dev/null || echo unknown)}
B=/usr/src/hildon
# the versions of this repository's tools (Leste's packages have the same:
# scripts/mkdeb-tools.sh)
declare -A VERSIONS=([pro1x-fm]=1.0)
eval "$(sed -n 's/^declare -A VERSIONS=(\(.*\))$/VERSIONS+=(\1)/p' "$REPO/scripts/mkdeb-tools.sh")"
# The parts above packaged with OUT: built by 40 (in $B/build), by 41 (in
# $B/root); the +pro1xN of Leste's packages of them (scripts/49-hidpi.sh)
PARTS40="libhildon hildon-status-menu hildon-desktop"
PARTS41="calendar-backend rtcom-eventlogger rtcom-eventlogger-plugins"
declare -A PRO1X=()
eval "$(sed -n 's/^ *"\([a-z0-9-]*\)|[0-9a-f]*|\([0-9]*\)".*/PRO1X[\1]=\2/p' "$REPO/scripts/49-hidpi.sh")"
log() { echo ">> $*"; }

# The image's root ($R): a chroot of it, or the running Arch itself
if [ "$MNT" = / ]; then
	R=
	in_root() { /bin/bash "$@"; }
else
	R=$MNT
	mountpoint -q "$MNT" || { echo "$MNT is not mounted"; exit 1; }
	for m in proc sys dev dev/pts; do
		mountpoint -q "$MNT/$m" && continue
		mount --bind "/$m" "$MNT/$m"
		# a slave of the system's: unmounting dev/pts here would otherwise
		# take the system's /dev/pts along (a bind of a shared /dev is its peer)
		mount --make-rslave "$MNT/$m"
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
	in_root() { chroot "$MNT" /bin/bash "$@"; }
fi

if [ -n "$PKGS" ]; then
	# --- the release's packages -----------------------------------------------
	log "packages from $PKGS"
	rm -rf "$R/tmp/pro1x-pkgs"
	mkdir -p "$R/tmp/pro1x-pkgs"
	cp "$PKGS"/*.pkg.tar.* "$R/tmp/pro1x-pkgs/"
	# steps 40 and 41 installed these parts without pacman: their files are
	# nobody's, which pacman takes for a conflict
	in_root -euo pipefail <<'EOF'
pacman -U --noconfirm --needed --overwrite '*' /tmp/pro1x-pkgs/*.pkg.tar.*
gpgconf --homedir /etc/pacman.d/gnupg --kill all 2> /dev/null || true
rm -rf /tmp/pro1x-pkgs
EOF
else
	# --- build ----------------------------------------------------------------
	log "packages"
	in_root -euo pipefail <<'EOF'
# the Events page's JSON (Mastodon, the weather); amixer and pactl for the
# FM sound, Python for its test tools; the compiler where 40 and 41 did not
# run before (ONLY=pro1x-alarmboot on the release's packages), XInput
pacman -S --noconfirm --needed base-devel json-glib alsa-utils libpulse python libxi > /dev/null
gpgconf --homedir /etc/pacman.d/gnupg --kill all 2> /dev/null || true
EOF
	missing=
	pcs="hildon-1 hildon-control-panel gconf-2.0 libosso rtcom-eventlogger
	    calendar-backend libtime libpulse-mainloop-glib json-glib-1.0 libcurl"
	# pro1x-alarmboot alone: alarmd's library, GTK, PulseAudio, X
	[ "$ONLY" != pro1x-alarmboot ] ||
	    pcs="gtk+-2.0 gio-unix-2.0 alarm libpulse-simple x11 xi"
	for pc in $pcs; do
		in_root -c "PKG_CONFIG_PATH=/opt/libical3/lib/pkgconfig pkg-config --exists $pc" ||
		    missing="$missing $pc"
	done
	if [ -n "$missing" ]; then
		echo "missing:$missing; first: bash arch/40-hildon.sh, then"
		echo "  ONLY=\"osso-af-settings codelockui cityinfo clockd icd2-osso-ic-dev libconic statusbar-alarm-dbus-api libdsme osso-systemui-dbus-dev alarmd calendar-backend hildon-control-panel rtcom-eventlogger rtcom-eventlogger-plugins\" bash arch/41-hildon-apps.sh"
		exit 1
	fi
	S=$B/pro1x
	rm -rf "${R:?}$S"
	mkdir -p "$R$S/apps" "$R$S/tools" "$R$S/arch"
	for d in apps/pro1x-events apps/pro1x-fm tools/pro1x-events tools/pro1x-volume-osd tools/pro1x-fm \
	    tools/pro1x-alarmboot tools/pro1x-rtc arch/alarm; do
		cp -a "$REPO/$d" "$R$S/$d"
	done
	tools=${ONLY:-$TOOLS}
	log "$(for t in $tools; do printf '%s %s, ' "$t" "${VERSIONS[$t]}"; done)"
	in_root -euo pipefail -s "$S" "$tools" <<'EOF'
S=$1 tools=" $2 " B=/usr/src/hildon
# C as it was written (today's GCC has C23 by default); the calendar on
# libical 3 (/opt/libical3, 41-hildon-apps.sh) with calendar-backend's C++
export PKG_CONFIG_PATH=/opt/libical3/lib/pkgconfig CPATH=/opt/libical3/include
export CC="gcc -std=gnu17" CXX="g++ -std=gnu++17" CFLAGS="-O2 -pipe" CXXFLAGS="-O2 -pipe"
want() { case $tools in *" $1 "*) return 0 ;; esac; return 1; }
for name in $tools; do
	rm -rf "${B:?}/root/$name"
	mkdir -p "$B/root/$name"
done
if want pro1x-alarmboot; then
	R=$B/root/pro1x-alarmboot
	install -d $R/usr/bin $R/usr/lib/pro1x
	# shellcheck disable=SC2046
	$CC $CFLAGS -Wall -o $R/usr/bin/pro1x-alarmboot $S/tools/pro1x-alarmboot/pro1x-alarmboot.c \
		$(pkg-config --cflags --libs gtk+-2.0 gio-2.0 gio-unix-2.0 alarm libpulse-simple x11 xi) -lm
	$CC $CFLAGS -Wall -fPIC -shared -fvisibility=hidden -o $R/usr/lib/pro1x/librtc-realtime.so \
		$S/tools/pro1x-rtc/rtc-realtime.c -ldl
	install -D -m 644 $S/arch/alarm/pro1x-alarm.conf $R/usr/share/dbus-1/system.d/pro1x-alarm.conf
	install -D -m 644 $S/arch/alarm/pro1x-alarm.tmpfiles $R/usr/lib/tmpfiles.d/pro1x-alarm.conf
	install -D -m 644 $S/arch/alarm/84-pro1x-rtc-alarm.rules \
		$R/usr/lib/udev/rules.d/84-pro1x-rtc-alarm.rules
fi
if want pro1x-events; then
make -C $S/apps/pro1x-events PREFIX=/usr HAVE_CAL=1 \
	> $S/pro1x-events.log 2>&1 &&
make -C $S/apps/pro1x-events install PREFIX=/usr HAVE_CAL=1 \
	DESTDIR=$B/root/pro1x-events >> $S/pro1x-events.log 2>&1 ||
	{ grep -m20 -E "error|Error" $S/pro1x-events.log; exit 1; }
fi
if want pro1x-volume-osd; then
install -d $B/root/pro1x-volume-osd/usr/bin
# shellcheck disable=SC2046
$CC $CFLAGS -Wall -o $B/root/pro1x-volume-osd/usr/bin/pro1x-volume-osd \
	$S/tools/pro1x-volume-osd/pro1x-volume-osd.c \
	$(pkg-config --cflags --libs gtk+-2.0 gconf-2.0 libpulse-mainloop-glib) -lm
fi
if want pro1x-fm; then
make -C $S/apps/pro1x-fm install PREFIX=/usr DESTDIR=$B/root/pro1x-fm > $S/pro1x-fm.log 2>&1 ||
	{ grep -m20 -E "error|Error" $S/pro1x-fm.log; exit 1; }
install -m 755 $S/tools/pro1x-fm/pro1x-fm-test $S/tools/pro1x-fm/pro1x-fm-audio-test \
	$B/root/pro1x-fm/usr/bin/
fi
for name in $tools; do
	cp -a "$B/root/$name/." /
done
# alarmd's queue directory and rtc0 for the session's user, at once
systemd-tmpfiles --create /usr/lib/tmpfiles.d/pro1x-alarm.conf 2> /dev/null || true
EOF
fi

# --- pacman packages (OUT) -------------------------------------------------------
if [ -n "$OUT" ] && [ -z "$PKGS" ]; then
	log "pacman packages into $OUT"
	ver() {
		local v n=${PRO1X[$1]:-}
		v=$(sed -n '1s/^[^(]*(\([^)]*\)).*/\1/p' "$R$B/src/$1/debian/changelog")
		v=${v#*:}
		v=${v//-/_}
		echo "$v${n:++pro1x$n}"
	}
	list=
	# (with ONLY those tools alone)
	[ -z "$ONLY" ] || PARTS40= PARTS41=
	for name in $PARTS40 $PARTS41; do list="$list $name=$(ver "$name")"; done
	for name in ${ONLY:-$TOOLS}; do list="$list $name=${VERSIONS[$name]}"; done
	in_root -euo pipefail -s "$PARTS40" "$list" "$COMMIT" <<'EOF'
parts40=$1 list=$2 commit=$3 B=/usr/src/hildon
# 40 installed its parts into the root: once more into their own
for name in $parts40; do
	PR=$B/root/$name
	rm -rf "$PR"
	make -C "$B/build/$name" install DESTDIR="$PR" > "$B/build/$name.pkg.log" 2>&1 ||
	    { tail -20 "$B/build/$name.pkg.log"; exit 1; }
done
declare -A DESC=(
	[libhildon]="Hildon's widgets (Maemo Leste's), with the Pro1X's HiDPI and MeeGo look patches"
	[hildon-status-menu]="Hildon's status area and menu (Maemo Leste's), with the Pro1X's HiDPI and MeeGo look patches"
	[hildon-desktop]="Hildon's window manager and home (Maemo Leste's), with the Pro1X's HiDPI and MeeGo look patches"
	[calendar-backend]="Maemo's calendar library (Maemo Leste's), repeating appointments fixed on 64-bit"
	[rtcom-eventlogger]="Maemo's event log of calls and messages (Maemo Leste's)"
	[rtcom-eventlogger-plugins]="The event log's calls, SMS and chat (Maemo Leste's)"
	[pro1x-events]="The Nokia N9's Events page for Hildon's MeeGo look, with its settings applet"
	[pro1x-volume-osd]="The Nokia N9's volume bar for Hildon's MeeGo look"
	[pro1x-fm]="FM radio for Hildon: the F(x)tec Pro1X's WCN3950 receiver (/dev/radio0)"
	[pro1x-alarmboot]="Alarms that switch the F(x)tec Pro1X on: the N9's alarm screens in Hildon, then off again"
)
declare -A LICENSE=([libhildon]=LGPL-2.1-or-later [hildon-status-menu]=LGPL-2.1-or-later
	[hildon-desktop]=GPL-2.0-or-later [calendar-backend]=LGPL-2.1-or-later
	[rtcom-eventlogger]=LGPL-2.1-or-later [rtcom-eventlogger-plugins]=LGPL-2.1-or-later)
# what the files need besides the libraries they link (those go by the
# packages that have them, below)
declare -A EXTRA=([pro1x-fm]="libpulse alsa-utils python"
	[pro1x-alarmboot]="alarmd osso-systemui osso-systemui-alarm statusbar-alarm")
# left out: the event log's test suite (Leste packages it apart; it wants
# the check library)
declare -A DROP=([rtcom-eventlogger]="usr/bin/rtcom-eventlogger-testsuite usr/lib/tests")
for e in $list; do
	name=${e%%=*} v=${e#*=} PR=$B/root/$name
	case $name in
	pro1x-*) url=https://github.com/smatkovi/pro1x-maemo-leste
		lic=LicenseRef-pro1x-maemo-leste ;;
	*)	url=https://github.com/maemo-leste/$name
		lic=${LICENSE[$name]} ;;
	esac
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
	# the libraries its programs and libraries link, by the Arch packages
	# that have them (those built here, without pacman, are left out)
	for d in ${DROP[$name]:-}; do rm -rf "${PR:?}/$d"; done
	deps=" ${EXTRA[$name]:-} "
	ldconfig -p > "$B/ld.cache"
	while IFS= read -r f; do
		for lib in $(readelf -d "$f" 2> /dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
			path=$(awk -v l="$lib" '$1 == l && !p { print $NF; p = 1 }' "$B/ld.cache")
			[ -n "$path" ] || continue
			owner=$(pacman -Qqo "$path" 2> /dev/null) || continue
			# (its own, where an earlier package of it is installed)
			[ "$owner" != "$name" ] || continue
			case $deps in *" $owner "*) ;; *) deps="$deps$owner " ;; esac
		done
	done < <(find "$PR" -type f \( -perm -u+x -o -name '*.so*' \) -exec sh -c \
	    'head -c4 "$1" | grep -q ELF' sh {} \; -print)
	# GConf's schemas, registered as their packages do on Leste
	schemas=
	for d in etc/gconf/schemas usr/share/gconf/schemas; do
		[ ! -d "$PR/$d" ] ||
		    schemas="$schemas$(cd "$PR" && find "$d" -name '*.schemas' | sed 's|^|/|' | tr '\n' ' ')"
	done
	P=$B/pkg/$name
	rm -rf "$P"
	install -d -o alarm -g alarm "$P"
	install=
	if [ -n "$schemas" ]; then
		install=gconf.install
		cat > "$P/gconf.install" <<EOS
post_install() {
	GCONF_CONFIG_SOURCE=\$(gconftool-2 --get-default-source) \\
		gconftool-2 --makefile-install-rule $schemas > /dev/null
}
post_upgrade() {
	post_install
}
EOS
	fi
	cat > "$P/PKGBUILD" <<EOS
# Made by arch/42-meego-look.sh (pro1x-maemo-leste $commit) from what
# arch/40, 41 and 42 built and installed into $PR
pkgname=$name
pkgver=$v
pkgrel=1
pkgdesc="${DESC[$name]}"
arch=(aarch64)
url=$url
license=($lic)
depends=($deps)
options=(!debug)
${install:+install=$install}
package() {
	cp -a "$PR/." "\$pkgdir/"
}
EOS
	chown alarm:alarm "$P"/*
	su alarm -c "cd '$P' && makepkg -d -f --noconfirm > makepkg.log 2>&1" ||
	    { tail -20 "$P/makepkg.log"; exit 1; }
	echo "   $(cd "$P" && ls ./*.pkg.tar.* | sed 's|^\./||') (depends:$deps)"
done
EOF
	mkdir -p "$OUT"
	cp "$R$B"/pkg/*/*.pkg.tar.* "$OUT/"
fi

# --- the session and the clock ------------------------------------------------------
log "hildon-session, pro1x-rtc.service"
install -m 755 "$REPO/arch/hildon/hildon-session" "$R/usr/local/bin/hildon-session"
install -m 644 "$REPO/kali/rootfs-overlay/etc/systemd/system/pro1x-rtc.service" \
	"$R/etc/systemd/system/pro1x-rtc.service"
in_root -euo pipefail <<'EOF'
# PipeWire with its PulseAudio server for every user's session (the volume
# bar and the FM sound talk to it), where the packages have not enabled it
systemctl --global enable pipewire.socket pipewire-pulse.socket wireplumber.service \
	> /dev/null 2>&1 || true
# MeeCast's icons for the Events page's weather: Arch's MeeCast is in
# /usr/lib/meecast (41-hildon-apps.sh), the page looks in MeeCast's own place
if [ -d /usr/lib/meecast/share/iconsets ] && [ ! -e /opt/com.meecast.omweather ]; then
	mkdir -p /opt/com.meecast.omweather/share
	ln -s /usr/lib/meecast/share/iconsets /opt/com.meecast.omweather/share/iconsets
fi
EOF
[ -n "$R" ] || systemctl daemon-reload
log "done: the MeeGo look with pro1x-desktop meego (as root), FM Radio in the launcher"
