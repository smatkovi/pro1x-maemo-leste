#!/bin/bash
# Maemo Leste's Hildon desktop for the Arch Linux ARM image, built natively
# in the image from Leste's sources at the revisions freebsd/hildon/build.sh
# pins (PARTS, REV, URL), with Debian's patches and the patches Leste
# gets too (patches/hidpi, patches/meego, patches/fixes); Leste's theme,
# icons and sounds from the running Leste itself. An X11 session
# (hildon-session, hildon.service) beside Phosh and MeeGo (pro1x-desktop
# hildon).
#
# Runs as root on the phone, in Maemo Leste, with the image mounted on $MNT
# (Leste has the network the builds need), or on the PC with a chroot of
# Arch Linux ARM there (arch/pc-chroot.sh; no Leste theme then):
#
#   bash arch/40-hildon.sh
#   REBUILD="libhildon hildon-desktop" bash arch/40-hildon.sh
#
# A part is built again when its revision or this repository's patches for
# it change. Parts built before the patches counted (their mark has the
# revision only) count as built, but for those whose patches changed since
# (STALE); REBUILD names parts to build again anyway.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
MNT=${MNT:-/mnt/arch}
JOBS=${JOBS:-$(nproc)}
REBUILD=${REBUILD:-}
# the parts whose patches changed after the marks with the revision only
# (2026-10-28: the MeeGo look's steps 2 to 4)
STALE="libhildon hildon-status-menu hildon-desktop"
B=/usr/src/hildon
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

# The parts and their revisions, as the FreeBSD build has them (only the
# tables, the build itself is FreeBSD's)
F=$REPO/freebsd/hildon/build.sh
SYSROOT=
eval "$(sed -n '/^PARTS=(/,/^)/p' "$F")"
eval "$(sed -n '/^declare -A URL REV=(/,/^)/p' "$F")"
eval "$(grep '^URL\[[a-z0-9.-]*\]=' "$F")"

log "packages"
chroot "$MNT" /bin/bash -euo pipefail <<'EOF'
pacman -Sy --noconfirm --needed base-devel git autoconf automake libtool \
	gtk-doc intltool gettext glib2 glib2-devel pango cairo atk gdk-pixbuf2 \
	shared-mime-info libxcursor libxinerama libxrandr libxi libxcomposite \
	libxdamage libxfixes libxtst libxss libsm dbus-glib libcanberra libpulse \
	mesa libpng libjpeg-turbo libexif sqlite libxml2 gstreamer \
	gst-plugins-base startup-notification xorg-server xorg-xinit \
	xorg-xrdb xorg-xset xorg-xmodmap gnome-menus gdk-pixbuf-xlib > /dev/null
gpgconf --homedir /etc/pacman.d/gnupg --kill all 2> /dev/null || true
EOF

# GConf, from the AUR, where step 30 has not built it (as there: built by
# the user alarm, installed by root)
if ! chroot "$MNT" pacman -Q gconf > /dev/null 2>&1; then
	log "GConf (AUR)"
	chroot "$MNT" /bin/bash -euo pipefail <<'EOF'
rm -rf /home/alarm/aur/gconf
install -d -o alarm -g alarm /home/alarm/aur
su alarm -c 'cd /home/alarm/aur && git clone -q https://aur.archlinux.org/gconf.git'
deps=$(cd /home/alarm/aur/gconf && bash -c '. ./PKGBUILD; echo "${depends[@]} ${makedepends[@]}"' |
	sed 's/[<>=][^ ]*//g')
pacman -S --noconfirm --needed $deps > /dev/null
su alarm -c 'cd /home/alarm/aur/gconf && makepkg --nocheck --noconfirm > makepkg.log 2>&1' ||
	{ tail -30 /home/alarm/aur/gconf/makepkg.log; exit 1; }
pacman -U --noconfirm /home/alarm/aur/gconf/gconf-*.pkg.tar.* > /dev/null
gpgconf --homedir /etc/pacman.d/gnupg --kill all 2> /dev/null || true
EOF
fi

# --- sources (on the host, Leste has git and the network either way) ----------
log "sources"
mkdir -p "$MNT$B/src"
for p in "${PARTS[@]}"; do
	name=${p%%|*}
	url=${URL[$name]:-https://github.com/maemo-leste/$name.git}
	d=$MNT$B/src/$name
	[ -d "$d/.git" ] || git clone -q "$url" "$d"
	git -C "$d" cat-file -e "${REV[$name]}^{commit}" 2> /dev/null || git -C "$d" fetch -q origin
	git -C "$d" checkout -q -f "${REV[$name]}"
done
for d in hidpi meego fixes; do
	rm -rf "${MNT:?}$B/patches/$d"
	mkdir -p "$MNT$B/patches/$d"
	cp -a "$REPO/patches/$d/." "$MNT$B/patches/$d/"
done

# --- build ------------------------------------------------------------------------
log "build"
printf '%s\n' "${PARTS[@]}" > "$MNT$B/parts"
chroot "$MNT" /bin/bash -euo pipefail -s "$JOBS" "$REBUILD" "$STALE" <<'EOF'
jobs=$1 rebuild=" $2 " stale=" $3 "
B=/usr/src/hildon
# Today's GCC refuses what Leste's let pass (C23 by default, implicit
# declarations and pointer types as errors): the code as it was written
export CFLAGS="-O2 -pipe -std=gnu17 -Wno-error=incompatible-pointer-types \
-Wno-error=implicit-function-declaration -Wno-error=int-conversion \
-Wno-error=implicit-int -Wno-error=return-mismatch -fpermissive"
export CXXFLAGS="-O2 -pipe -fpermissive"
while IFS= read -r p; do
	name=${p%%|*} rest=${p#*|}
	args=${rest%%|*}
	extra=; case $rest in *"|"*) extra=${rest#*|} ;; esac
	rev=$(git -C $B/src/$name rev-parse HEAD)
	# this repository's patches for it (HiDPI, the MeeGo look, fixes)
	ours=
	for d in hidpi meego fixes; do
		[ ! -f "$B/patches/$d/$name.patch" ] || ours="$ours $B/patches/$d/$name.patch"
	done
	mark=$rev
	# shellcheck disable=SC2086
	[ -z "$ours" ] || mark="$rev $(cat $ours | sha256sum | cut -c1-16)"
	was=$(cat $B/$name.done 2> /dev/null || true)
	case $rebuild in *" $name "*) was= ;; esac
	case $stale in *" $name "*) [ "$was" != "$rev" ] || was= ;; esac
	if [ "$was" = "$mark" ] || [ "$was" = "$rev" ]; then
		echo "   $name: built"; continue
	fi
	echo "   $name"
	rm -rf "$B/build/$name"; mkdir -p $B/build
	cp -a "$B/src/$name" "$B/build/$name"
	cd "$B/build/$name"
	if [ -f debian/patches/series ]; then
		grep -v '^#' debian/patches/series | while read -r q _; do
			[ -n "$q" ] && patch -s -p1 < "debian/patches/$q"
		done
	fi
	# HiDPI, then the MeeGo look (pages, edge gestures, status bar), then
	# bug fixes, as Leste (scripts/49-hidpi.sh) and FreeBSD
	for q in $ours; do
		patch -s -p1 < "$q"
	done
	if [ "$args" = "!" ]; then
		case $name in
		mce-dev)
			# mce's D-Bus names only (the repository is mce itself)
			v=$(sed -n '1s/^[^(]*(\([^)]*\)).*/\1/p' debian/changelog)
			install -d /usr/include/mce /usr/lib/pkgconfig
			install -m 644 src/include/mce/*.h /usr/include/mce/
			printf 'prefix=/usr\nincludedir=${prefix}/include\n\nName: mce\nDescription: Mode Control Entity\nVersion: %s\nCflags: -I${includedir}\n' \
				"$v" > /usr/lib/pkgconfig/mce.pc ;;
		# its Makefile installs below ROOT, /tmp/test-profiled unless told
		profiled) make -j"$jobs" > make.log 2>&1 && make install ROOT=/ > install.log 2>&1 ;;
		osso-bookmark-engine) make -j"$jobs" > make.log 2>&1 && make install > install.log 2>&1 ;;
		esac
	else
		# the FreeBSD build's static matchbox library is /usr/lib here
		args=$(echo "$args" | sed 's|MB2_STATIC_LIB=[^ ]*|MB2_STATIC_LIB=/usr/lib/libmatchbox2-0.1.a|')
		for f in NEWS README AUTHORS ChangeLog; do [ -e "$f" ] || : > "$f"; done
		autoreconf -fi > autoreconf.log 2>&1 || { tail -15 autoreconf.log; exit 1; }
		# shellcheck disable=SC2086
		CPPFLAGS="$extra" ./configure --prefix=/usr --sysconfdir=/etc \
			--localstatedir=/var --disable-gtk-doc $args > configure.log 2>&1 ||
			{ tail -25 configure.log; exit 1; }
		find . -name Makefile -exec sed -i 's/ -Werror\([=a-z-]*\)\?//g' {} +
		# today's Perl refuses defined(@array) (GTK 2's demo generator)
		[ ! -f demos/gtk-demo/geninclude.pl ] ||
		    sed -i -e 's/defined *(\(@[a-z_]*\))/\1/' -e 's/defined *\(@[a-z_]*\)/\1/' demos/gtk-demo/geninclude.pl
		make -j"$jobs" > make.log 2>&1 || { grep -m20 -E "error:|Error [0-9]|\*\*\*" make.log; exit 1; }
		make install > install.log 2>&1 || { tail -20 install.log; exit 1; }
	fi
	ldconfig
	echo "$mark" > "$B/$name.done"
done < $B/parts
EOF

# --- Leste's theme, icons and sounds, from Leste itself ---------------------------
if command -v dpkg > /dev/null; then
	log "theme, icons and sounds from Leste"
	for pkg in hildon-theme-alpha osso-icons-default osso-sounds-ui; do
		{ dpkg -L "$pkg" 2> /dev/null || true; } | while read -r f; do
			[ -f "$f" ] || [ -L "$f" ] || continue
			mkdir -p "$MNT$(dirname "$f")"
			cp -a "$f" "$MNT$f"
		done
	done
else
	log "(not in Leste: no theme, icons and sounds from it)"
fi

# --- at the N900's size on the full resolution (tools/pro1x-hidpi, as Leste) ----
log "pro1x-hidpi"
install -m 755 "$REPO/tools/pro1x-hidpi/pro1x-hidpi" "$MNT/usr/local/bin/"
cp "$REPO/tools/pro1x-hidpi/pro1x-theme-scale.c" "$MNT$B/"
chroot "$MNT" /bin/bash -euo pipefail <<'EOF'
gcc -O2 -Wall -o /usr/local/bin/pro1x-theme-scale /usr/src/hildon/pro1x-theme-scale.c \
	$(pkg-config --cflags --libs gdk-pixbuf-2.0) -lm
pro1x-hidpi theme > /dev/null || echo "   (no theme to scale yet: pro1x-hidpi theme once it is there)"
EOF

# --- the session ------------------------------------------------------------------
log "hildon-session"
install -m 755 "$REPO/arch/hildon/hildon-session" "$MNT/usr/local/bin/hildon-session"
install -m 644 "$REPO/arch/hildon/hildon.service" "$REPO/arch/hildon/meego.service" \
	"$MNT/etc/systemd/system/"
install -m 755 "$REPO/arch/pro1x-desktop" "$MNT/usr/local/bin/pro1x-desktop"
# the N9's own UI (step 30) is "harmattan", "meego" the MeeGo look on Hildon
if [ -x "$MNT/usr/bin/meegotouchhome" ]; then
	install -m 755 "$REPO/arch/harmattan/harmattan-session" "$MNT/usr/local/bin/"
	install -m 644 "$REPO/arch/harmattan/harmattan.service" "$MNT/etc/systemd/system/"
	rm -f "$MNT/usr/local/bin/meego-session"
fi
log "done: pro1x-desktop hildon or meego (as root) makes it the session"
