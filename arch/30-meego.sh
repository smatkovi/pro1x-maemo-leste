#!/bin/bash
# The MeeGo look of the Nokia N9 for the Arch Linux ARM image: the open
# parts of its user interface ported to Qt 5 (github.com/smatkovi/
# harmattan-qt5: libmeegotouch, mcompositor, the home screen, the system
# UI, the settings, meegofeedbackd), built natively in the image and
# installed below /usr, plus an X11 session for them (harmattan-session).
#
# Runs as root on the phone, in Maemo Leste, with the image from steps 10
# and 20 mounted on $MNT (Leste has the network the builds need):
#
#   bash arch/30-meego.sh                 # HQ5_REV=main
#
# The N9's theme (blanco), its fonts and sounds are Nokia's and are not in
# the public repositories: N9_ROOT, a mounted N9 or N950 root file system
# (default: /host/harmattan.img, the phone's own copy, mounted read-only
# here), gives them to the image, which stays on the phone.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
MNT=${MNT:-/mnt/arch}
HQ5_URL=${HQ5_URL:-https://github.com/smatkovi/harmattan-qt5.git}
HQ5_REV=${HQ5_REV:-main}
N9_IMG=${N9_IMG:-/host/harmattan.img}
N9_ROOT=${N9_ROOT:-}
JOBS=${JOBS:-$(nproc)}

mountpoint -q "$MNT" || { echo "$MNT is not mounted"; exit 1; }
log() { echo ">> $*"; }

for m in proc sys dev dev/pts; do
	mountpoint -q "$MNT/$m" || mount --bind "/$m" "$MNT/$m"
done
n9mnt=
cleanup() {
	# the gpg-agent pacman starts to check signatures keeps /dev busy
	chroot "$MNT" gpgconf --homedir /etc/pacman.d/gnupg --kill all 2> /dev/null || true
	[ -z "$n9mnt" ] || umount "$n9mnt" 2> /dev/null || true
	for m in dev/pts dev sys proc; do
		! mountpoint -q "$MNT/$m" || umount "$MNT/$m" || umount -l "$MNT/$m"
	done
}
trap cleanup EXIT
rm -f "$MNT/etc/resolv.conf"
cp /etc/resolv.conf "$MNT/etc/resolv.conf"

# --- build dependencies --------------------------------------------------------
log "packages"
chroot "$MNT" /bin/bash -euo pipefail <<'EOF'
pacman -Sy --noconfirm --needed base-devel git \
	qt5-base qt5-x11extras qt5-declarative qt5-svg qt5-tools icu \
	dbus-glib libxml2 polkit intltool gtk-doc gobject-introspection \
	tinycdb libxdamage libxcomposite libxrandr libxtst libpulse libsndfile \
	xorg-server xorg-xinit xf86-input-libinput xorg-xinput xorg-xrandr \
	xorg-xset xorg-xprop xdotool doxygen > /dev/null
gpgconf --homedir /etc/pacman.d/gnupg --kill all 2> /dev/null || true
EOF

# GConf (libmeegotouch's settings, meegofeedbackd), from the AUR: built by
# the user alarm, installed by root
if ! chroot "$MNT" pacman -Q gconf > /dev/null 2>&1; then
	log "GConf (AUR)"
	chroot "$MNT" /bin/bash -euo pipefail <<'EOF'
rm -rf /home/alarm/aur/gconf
install -d -o alarm -g alarm /home/alarm/aur
su alarm -c 'cd /home/alarm/aur && git clone -q https://aur.archlinux.org/gconf.git'
# its dependencies installed by root first (makepkg runs without root)
deps=$(cd /home/alarm/aur/gconf && bash -c '. ./PKGBUILD; echo "${depends[@]} ${makedepends[@]}"' |
	sed 's/[<>=][^ ]*//g')
pacman -S --noconfirm --needed $deps > /dev/null
su alarm -c 'cd /home/alarm/aur/gconf && makepkg --nocheck --noconfirm > makepkg.log 2>&1' ||
	{ tail -30 /home/alarm/aur/gconf/makepkg.log; exit 1; }
pacman -U --noconfirm /home/alarm/aur/gconf/gconf-*.pkg.tar.* > /dev/null
EOF
fi

# --- harmattan-qt5 -------------------------------------------------------------
log "harmattan-qt5 $HQ5_REV"
chroot "$MNT" /bin/bash -euo pipefail -s "$HQ5_URL" "$HQ5_REV" "$JOBS" <<'EOF'
url=$1 rev=$2 jobs=$3
S=/usr/src/harmattan-qt5
[ -d $S/.git ] || git clone -q "$url" $S
git -C $S fetch -q origin
git -C $S checkout -q -f "$rev"
git -C $S reset -q --hard "origin/$rev" 2> /dev/null || true
B=/usr/src/harmattan-qt5-build
mkdir -p $B

# A part built in a copy of its source (several run generators there),
# installed into / ; a part already built at this revision is skipped
part() {
	local name=$1 kind=$2; shift 2
	local stamp=$B/$name.done want
	want=$(git -C $S log -1 --format=%H -- "$name")
	if [ "$(cat $stamp 2> /dev/null)" = "$want" ]; then
		echo "   $name: built"
		return
	fi
	echo "   $name"
	rm -rf "$B/$name"
	cp -a "$S/$name" "$B/$name"
	cd "$B/$name"
	case $kind in
	configure)
		./configure "$@" > configure.log 2>&1 || { tail -20 configure.log; exit 1; } ;;
	qmake)
		# the headers' own directories, as the stage builds on the PC have them
		qmake -r "$@" CONFIG+=NODOCS CONFIG+=NOTESTS INCLUDEPATH+=/usr/include/meegotouch \
		    INCLUDEPATH+=/usr/include/duicontrolpanel > qmake.log 2>&1 ||
		    { tail -20 qmake.log; exit 1; } ;;
	autotools)
		autoreconf -i -f > autoreconf.log 2>&1 || { tail -20 autoreconf.log; exit 1; }
		./configure --prefix=/usr --sysconfdir=/etc "$@" > configure.log 2>&1 ||
		    { tail -20 configure.log; exit 1; } ;;
	make)
		;;
	esac
	# -k: parts not ported yet (some status menu plugins) do not stop the
	# rest; what must exist is checked at the end
	[ $kind = make ] || set --
	make -k -j"$jobs" "$@" > make.log 2>&1 ||
	    { echo "   $name: not all of it builds:"; grep -m5 -e "error:" -e "\*\*\*" make.log | sed 's/^/     /' || true; }
	make -k install "$@" > install.log 2>&1 || true
	echo "$want" > "$stamp"
}

# The sound daemon for events (Nemo's ngfd, the open successor of the N9's)
# and its client library, from their upstream tags; Harmattan's mce-dev
# lacks a signal name newer mce has
upstream() {
	local name=$1 tag=$2; shift 2
	local stamp=$B/$name.done
	if [ "$(cat $stamp 2> /dev/null)" = "$tag" ]; then
		echo "   $name: built"
		return
	fi
	echo "   $name $tag"
	rm -rf "$B/$name"
	git clone -q --branch "$tag" "https://github.com/sailfishos/$name.git" "$B/$name"
	cd "$B/$name"
	git submodule update -q --init
	./autogen.sh > autogen.log 2>&1 || autoreconf -i -f > autogen.log 2>&1
	./configure --prefix=/usr --sysconfdir=/etc "$@" > configure.log 2>&1 ||
	    { tail -20 configure.log; exit 1; }
	make -j"$jobs" > make.log 2>&1 || { grep -m20 "error" make.log; exit 1; }
	make install > install.log 2>&1 || { tail -20 install.log; exit 1; }
	echo "$tag" > "$stamp"
}

part libmlocale configure -prefix /usr -release -nomake demos -nomake tests -nomake benchmarks
part libmeegotouch configure -prefix /usr -release -no-werror -nomake demos -nomake tests -nomake benchmarks
part contextkit autotools --disable-doc --disable-coverage
part libcontentaction autotools --disable-doc --disable-coverage
part mcompositor qmake mcompositor.pro
part libdsme make
part mce-dev make
part qmsystem2 qmake libqmsystem2.pro
pacman -S --noconfirm --needed check gstreamer gst-plugins-base gst-plugins-good \
	libcanberra > /dev/null
upstream libngf 0.29
CPPFLAGS='-DMCE_LED_PATTERN_DEACTIVATED_SIG="led_pattern_deactivated_ind"' \
	upstream ngfd 1.4.13 --disable-systemd
# system-ui plays its notifications' sounds through ngfd (libngf)
part system-ui qmake project.pro DEFINES+=HAVE_LIBNGF
part meegotouchhome qmake projects.pro
part duicontrolpanel qmake projects.pro
part meegotouch-controlpanelapplets qmake meegotouch-controlpanelapplets.pro
part meegofeedbackd qmake meegofeedbackd.pro
ldconfig
missing=
for f in /usr/lib/libmeegotouchcore.so /usr/bin/mcompositor /usr/bin/sysuid \
	/usr/bin/meegotouchhome /usr/bin/meegofeedbackd /usr/bin/ngfd; do
	[ -e $f ] || missing="$missing $f"
done
[ -z "$missing" ] || { echo "missing:$missing"; exit 1; }
EOF

# --- the N9's theme, fonts and sounds (Nokia's, from the phone's own copy) ---
if [ -z "$N9_ROOT" ] && [ -f "$N9_IMG" ]; then
	n9mnt=$(mktemp -d)
	mount -o loop,ro "$N9_IMG" "$n9mnt"
	N9_ROOT=$n9mnt
fi
if [ -n "$N9_ROOT" ] && [ -d "$N9_ROOT/usr/share/themes/blanco" ]; then
	log "the N9's theme, fonts and sounds from $N9_ROOT"
	mkdir -p "$MNT/usr/share/themes" "$MNT/usr/share/fonts/nokia" "$MNT/usr/share/sounds"
	cp -a "$N9_ROOT/usr/share/themes/base" "$N9_ROOT/usr/share/themes/blanco" \
		"$MNT/usr/share/themes/"
	cp -a "$N9_ROOT"/usr/share/fonts/*/NokiaPure* "$MNT/usr/share/fonts/nokia/" 2> /dev/null ||
		cp -a "$N9_ROOT"/usr/share/fonts/nokia/. "$MNT/usr/share/fonts/nokia/" 2> /dev/null || true
	for d in ui-tones ring-tones vibra; do
		cp -a "$N9_ROOT/usr/share/sounds/$d" "$MNT/usr/share/sounds/"
	done
	# ngfd's events: the N9's own, in place of Nemo's
	rm -rf "$MNT/usr/share/ngfd/events.d"
	cp -a "$N9_ROOT/usr/share/ngfd/events.d" "$MNT/usr/share/ngfd/events.d"
	chroot "$MNT" fc-cache -f > /dev/null 2>&1 || true
else
	log "no N9 root file system: the MeeGo look draws without its theme"
fi

# ngfd with the plugins that run here (harmattan-qt5/ngfd-meego)
install -m 644 "$MNT/usr/src/harmattan-qt5/ngfd-meego/ngfd.ini" "$MNT/usr/share/ngfd/ngfd.ini"

# --- the session ------------------------------------------------------------------
log "harmattan-session"
install -m 755 "$REPO/arch/harmattan/harmattan-session" "$MNT/usr/local/bin/harmattan-session"
install -m 644 "$REPO/arch/harmattan/harmattan.service" "$MNT/etc/systemd/system/harmattan.service"
install -m 755 "$REPO/arch/pro1x-desktop" "$MNT/usr/local/bin/pro1x-desktop"
log "done: pro1x-desktop harmattan (as root) makes it the session"
