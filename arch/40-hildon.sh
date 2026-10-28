#!/bin/bash
# Maemo Leste's Hildon desktop for the Arch Linux ARM image, built natively
# in the image from Leste's sources at the revisions freebsd/hildon/build.sh
# pins (PARTS, REV, URL), with Debian's patches and the Pro1X HiDPI
# patches Leste gets too (patches/hidpi); Leste's theme, icons and sounds
# from the running Leste itself. An X11 session (hildon-session,
# hildon.service) beside Phosh and MeeGo (pro1x-desktop hildon).
#
# Runs as root on the phone, in Maemo Leste, with the image mounted on $MNT
# (Leste has the network the builds need):
#
#   bash arch/40-hildon.sh
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
MNT=${MNT:-/mnt/arch}
JOBS=${JOBS:-$(nproc)}
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
	xorg-xrdb xorg-xset xorg-xmodmap > /dev/null
gpgconf --homedir /etc/pacman.d/gnupg --kill all 2> /dev/null || true
EOF

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
mkdir -p "$MNT$B/patches"
cp -a "$REPO/patches/hidpi/." "$MNT$B/patches/"

# --- build ------------------------------------------------------------------------
log "build"
printf '%s\n' "${PARTS[@]}" > "$MNT$B/parts"
chroot "$MNT" /bin/bash -euo pipefail -s "$JOBS" <<'EOF'
jobs=$1
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
	if [ "$(cat $B/$name.done 2> /dev/null)" = "$rev" ]; then
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
	[ ! -f "$B/patches/$name.patch" ] || patch -s -p1 < "$B/patches/$name.patch"
	if [ "$args" = "!" ]; then
		case $name in
		mce-dev)
			# mce's D-Bus names only (the repository is mce itself)
			v=$(sed -n '1s/^[^(]*(\([^)]*\)).*/\1/p' debian/changelog)
			install -d /usr/include/mce /usr/lib/pkgconfig
			install -m 644 src/include/mce/*.h /usr/include/mce/
			printf 'prefix=/usr\nincludedir=${prefix}/include\n\nName: mce\nDescription: Mode Control Entity\nVersion: %s\nCflags: -I${includedir}\n' \
				"$v" > /usr/lib/pkgconfig/mce.pc ;;
		profiled) make -j"$jobs" > make.log 2>&1 && make install > install.log 2>&1 ;;
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
		make -j"$jobs" > make.log 2>&1 || { grep -m20 "error" make.log; exit 1; }
		make install > install.log 2>&1 || { tail -20 install.log; exit 1; }
	fi
	ldconfig
	echo "$rev" > "$B/$name.done"
done < $B/parts
EOF

# --- Leste's theme, icons and sounds, from Leste itself ---------------------------
log "theme, icons and sounds from Leste"
for pkg in hildon-theme-alpha osso-icons-default osso-sounds-ui; do
	dpkg -L "$pkg" 2> /dev/null | while read -r f; do
		[ -f "$f" ] || [ -L "$f" ] || continue
		mkdir -p "$MNT$(dirname "$f")"
		cp -a "$f" "$MNT$f"
	done
done

# --- at the N900's size on the full resolution (tools/pro1x-hidpi, as Leste) ----
log "pro1x-hidpi"
install -m 755 "$REPO/tools/pro1x-hidpi/pro1x-hidpi" "$MNT/usr/local/bin/"
cp "$REPO/tools/pro1x-hidpi/pro1x-theme-scale.c" "$MNT$B/"
chroot "$MNT" /bin/bash -euo pipefail <<'EOF'
gcc -O2 -Wall -o /usr/local/bin/pro1x-theme-scale /usr/src/hildon/pro1x-theme-scale.c \
	$(pkg-config --cflags --libs gdk-pixbuf-2.0) -lm
pro1x-hidpi theme > /dev/null
EOF

# --- the session ------------------------------------------------------------------
log "hildon-session"
install -m 755 "$REPO/arch/hildon/hildon-session" "$MNT/usr/local/bin/hildon-session"
install -m 644 "$REPO/arch/hildon/hildon.service" "$MNT/etc/systemd/system/hildon.service"
install -m 755 "$REPO/arch/meego/pro1x-desktop" "$MNT/usr/local/bin/pro1x-desktop"
log "done: pro1x-desktop hildon (as root) makes it the session"
