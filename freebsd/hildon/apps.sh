# Maemo Leste's apps for FreeBSD (sourced by build.sh): the libraries
# they need first, then the apps; each "name|configure arguments[|extra
# CPPFLAGS]" as PARTS, "name|!" for a custom_<name> build and "name|make"
# for a plain Makefile that installs below /usr (moved to /usr/local).
# The revisions are what build.sh checks out; URL where a source is not
# github.com/maemo-leste/<name>.
GH_EXTRAS=https://github.com/maemo-leste-extras
GM_EXTRAS=https://git.maemo.org/leste-extras

APPS=(
	# libraries and data the apps build on
	"osso-af-settings|"
	"maemo-launcher|!"
	"codelockui|make"
	"libconbtui|"
	"libcomapp|"
	"hildon-input-method-framework|--disable-gtk-doc"
	"hildon-input-method|--disable-gtk-doc"
	"libimlayouts|make"
	"libimengines|make"
	"hildon-im-vkbrenderer3|"
	"wpeditor|"
	"osso-games-startup|"
	"hildon-games-wrapper|!"
	"cityinfo|"
	"clockd|"
	# the alarm daemon and what it talks to: the connectivity library, the
	# status area clock's, the system UI's and dsme's D-Bus names
	"icd2-osso-ic-dev|"
	"libconic|"
	"statusbar-alarm-dbus-api|"
	"libdsme|!"
	"osso-systemui-dbus-dev|!"
	"alarmd|!"
	"clock|"
	"hildon-time-zone-chooser|"
	"calendar-backend|"
	# the alarm dialog: the system UI and its plugin, with the sound and
	# vibra notifications (no sound on FreeBSD yet), and the status area
	# clock, which shows the next alarm
	"libplayback|!"
	"hildon-plugins-notify-sv|"
	"osso-systemui|"
	"osso-systemui-alarm|!"
	"statusbar-alarm|"
	# preinstalled on Leste
	"osso-xterm|"
	"osso-pdf-viewer|"
	"hildon-control-panel|"
	"hildon-control-panel-personalisation|"
	"osso-applet-display|make"
	"applet-datetime|"
	"osso-applet-languageregional|"
	"osso-applet-textinput|"
	"osso-applet-notificationlight|make"
	"status-area-applet-battery|"
	"profilesx|"
	"status-menu-applet-profiles|"
	# the Maemo 5 key and touch screen clicks (libcanberra, the profile's
	# key sound level)
	"maemo-input-sounds|"
	"hildon-input-method-plugins|--disable-dependency-tracking"
	# Leste's extras
	"maepad|"
	"mstardict||-D_LIBCPP_ENABLE_CXX17_REMOVED_AUTO_PTR -Wno-reserved-user-defined-literal"
	"osso-mahjong|"
	# the N900's games that are free software (smatkovi's ports); chess
	# compiles its .c files as C++, so no C-only -std= in its CFLAGS
	"osso-chess-ui|--with-gnuchess=/usr/local/bin/gnuchess CFLAGS=-O2"
	"osso-lmarbles|"
	"leafpad|"
	"desktop-cmd-exec|"
	"live-wallpaper|"
	"cal-home-widget|!"
	"maefat|"
	"braek|!"
	# Qt 5: the Hildon style with the Maemo 5 module (QtMaemo5), then the
	# Qt apps
	"qtstyleplugins|!"
	"libgq-gconf|"
	"osso-calculator|!"
	"clock-ui|!"
	"qalendar|!"
	"countdowntimer|!"
	"easylist|!"
	"flappybird|!"
	"qshot|!"
	"mihphoto|!"
	"hamsterfiler|!"
	# The MeeGo Harmattan (Nokia N9) Qt Quick components, com.nokia.meego,
	# in their Qt 5 port (QtQuick 2, import com.nokia.meego 2.0), for
	# MeeGo apps; their theme is a meegotouch theme in
	# /usr/local/share/themes
	"qt-components|!"
	# the Nokia N9's clock in QML on them (github.com/smatkovi)
	"meego-clock|!"
	# MeeCast, the weather forecast of Maemo 5 and the N9 (Qt 5, QtQuick
	# Controls 2), with its weather sources
	"meecast|!"
	# The web engines, which FreeBSD's quarterly packages no longer have for
	# arm64: QtWebEngine as FreeBSD 15.1 built it (its release_1 set), with
	# the FFmpeg 8.1 libraries it links; QtWebKit from its last release.
	# Then the apps that need them.
	"ffmpeg8-libs|!"
	"qt5-webengine|pkg"
	"qt5-webkit|!"
	"jib|!"
	"dorian|!"
	# The DSPs' userland (freebsd/dsp/README.md): the sensors' file server,
	# QMI over QRTR, the sensor client library; for the modem libqrtr, its
	# remote file system, its TFTP server and the protection domain mapper
	"hexagonrpc|!"
	"libqrtr-glib|!"
	"libqmi|!"
	"libssc|!"
	"qrtr|!"
	"rmtfs|!"
	"tqftpserv|!"
	"pd-mapper|!"
	# Maemo 5 apps ported to Leste (github.com/smatkovi), at their releases
	"headphoned|!"
	"pedometerhomewidget|!"
	"wifi-assistant|!"
	"gpodder-leste|!"
)

REV+=(
	[osso-af-settings]=3e35b3b2ca7f [codelockui]=07c6654ae546
	[maemo-launcher]=2cb10ec66a22
	[libconbtui]=91e6bd445be8 [libcomapp]=84317d5c6614
	[hildon-input-method-framework]=99afab2e2e78
	[hildon-input-method]=0977df548b0b [libimlayouts]=d7a4960189b2
	[libimengines]=5541d57ab84f [hildon-im-vkbrenderer3]=b8b4dbe0993b
	[wpeditor]=84a17efe3392 [osso-games-startup]=1740aa8daaf4
	[cityinfo]=2228f3ae85ac [clockd]=9130890fe1ae [alarmd]=6a248a323bcc
	[statusbar-alarm-dbus-api]=acf2954 [osso-systemui-dbus-dev]=dc5c9b5
	[libdsme]=e4f8b85a6743
	[libplayback]=ed18405 [hildon-plugins-notify-sv]=ddfe375457f7
	[osso-systemui]=d1b65f09fe6d [osso-systemui-alarm]=f9350639d4ba
	[statusbar-alarm]=55045c7780b2
	[clock]=8abc98d2854f [hildon-time-zone-chooser]=123ee9c1606f
	[calendar-backend]=ce6ae29ea5d4 [icd2-osso-ic-dev]=acf440d9d3f2
	[libconic]=cd9db218dacd
	[osso-xterm]=6c912f73983e [osso-pdf-viewer]=a5c944ebff05
	[hildon-control-panel]=359e43a8a899
	[hildon-control-panel-personalisation]=4f6086c7e69b
	[osso-applet-display]=66309efec84a [applet-datetime]=39054c07ac94
	[osso-applet-languageregional]=0f82237b8f5e
	[osso-applet-textinput]=0b32f803ace8
	[osso-applet-notificationlight]=e364719aa592
	[status-area-applet-battery]=0e4fcf07b39b [profilesx]=4c89c9701fdc
	[status-menu-applet-profiles]=f655a3c400fd
	[maemo-input-sounds]=9e2b7e460a8a
	[hildon-input-method-plugins]=04c1d47222d0
	[maepad]=b50c9caadd86 [mstardict]=48f7924faed8
	[osso-mahjong]=fbe179709a15 [leafpad]=9ae1e0314158
	[osso-chess-ui]=4674e8e [osso-lmarbles]=97dcd65
	[hildon-games-wrapper]=2b36c80
	[desktop-cmd-exec]=e3ac73e7b031 [live-wallpaper]=d950d1e44e66
	[cal-home-widget]=52ef149cdca0 [maefat]=11bc84b6fccb [braek]=77ceda0d3d92
	[qtstyleplugins]=c371b3232025 [libgq-gconf]=8617c495b6ac
	[osso-calculator]=6fff52bb301f [clock-ui]=7c832feb877b [qalendar]=04ccfca3ca17
	[countdowntimer]=ddc1f7027af9 [easylist]=d5c5bcc6c677 [flappybird]=21f81151207a
	[qshot]=f10335de9c66 [mihphoto]=bab0cb18bc0e
	[hamsterfiler]=19ac58143e2e [qt-components]=c967fbd79e75
	[meego-clock]=8c9e205 [meecast]=f8f26737cb74
	[ffmpeg8-libs]=464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c
	[qt5-webengine]=c5de7df005189a2ef727a40b13408ff3a14a0bbedce07af986aac59722d4cf09
	[qt5-webkit]=9ca126da9273664dd23a3ccd0c9bebceb7bb534bddd743db31caf6a5a6d4a9e6
	[jib]=5dd4a340d20d [dorian]=da92eda1d5d9
	[hexagonrpc]=40e50412812a
	[libqrtr-glib]=69791f8f2a09 [libqmi]=3f07d6e5b467 [libssc]=0cf77b93b557
	[qrtr]=29e36ae16438 [rmtfs]=b30a3eb38f9a [tqftpserv]=0bf5b98c2422
	[pd-mapper]=5ecd2fe926ac
	[headphoned]=d476cb4a8bf9 [pedometerhomewidget]=06d5246191fa
	[wifi-assistant]=604bbe98d8b5 [gpodder-leste]=2759445563b5
)
URL+=(
	[maepad]=$GM_EXTRAS/maepad.git
	[mstardict]=$GH_EXTRAS/mstardict.git [osso-mahjong]=$GH_EXTRAS/osso-mahjong.git
	[osso-chess-ui]=https://github.com/smatkovi/osso-chess-ui.git
	[osso-lmarbles]=https://github.com/smatkovi/osso-lmarbles.git
	[leafpad]=$GH_EXTRAS/leafpad.git [desktop-cmd-exec]=$GH_EXTRAS/desktop-cmd-exec.git
	[live-wallpaper]=$GH_EXTRAS/live-wallpaper.git
	[cal-home-widget]=$GH_EXTRAS/cal-home-widget.git [maefat]=$GH_EXTRAS/maefat.git
	[braek]=$GH_EXTRAS/braek.git
	[countdowntimer]=$GM_EXTRAS/countdowntimer.git [easylist]=$GH_EXTRAS/easylist.git
	[flappybird]=$GM_EXTRAS/flappybird.git [qshot]=$GH_EXTRAS/qshot.git
	[mihphoto]=$GH_EXTRAS/mihphoto.git
	[hamsterfiler]=$GH_EXTRAS/hamsterfiler.git
	[qt-components]=https://github.com/sailfishos/qt-components.git
	[meego-clock]=https://github.com/smatkovi/meego-clock.git
	[meecast]=https://github.com/Meecast/meecast.git
	[ffmpeg8-libs]=https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz
	[qt5-webengine]=pkg:release_1
	[qt5-webkit]=https://github.com/qtwebkit/qtwebkit/releases/download/qtwebkit-5.212.0-alpha4/qtwebkit-5.212.0-alpha4.tar.xz
	[jib]=$GH_EXTRAS/jib.git [dorian]=$GH_EXTRAS/dorian.git
	[hexagonrpc]=https://github.com/linux-msm/hexagonrpc.git
	[libqrtr-glib]=https://gitlab.freedesktop.org/mobile-broadband/libqrtr-glib.git
	[libqmi]=https://gitlab.freedesktop.org/mobile-broadband/libqmi.git
	[libssc]=https://codeberg.org/DylanVanAssche/libssc.git
	[qrtr]=https://github.com/linux-msm/qrtr.git
	[rmtfs]=https://github.com/linux-msm/rmtfs.git
	[tqftpserv]=https://github.com/linux-msm/tqftpserv.git
	[pd-mapper]=https://github.com/linux-msm/pd-mapper.git
	[headphoned]=https://github.com/smatkovi/headphoned.git
	[pedometerhomewidget]=https://github.com/smatkovi/pedometerhomewidget.git
	[wifi-assistant]=https://github.com/smatkovi/wifi-assistant.git
	[gpodder-leste]=https://github.com/smatkovi/gpodder-leste.git
)

# A plain Makefile: built with the cross compiler, installed below /usr,
# then moved below /usr/local (pkg-config files to libdata/pkgconfig)
custom_hildon_games_wrapper() {
	# plain Makefiles that take their flags from pkg-config with CFLAGS+=,
	# which a CFLAGS on the command line would replace
	make -C libhgw CC="$CC" CFLAGS="$CPPFLAGS $CFLAGS $(pkg-config --cflags dbus-1 gconf-2.0) -I. -fPIC" \
	    LDFLAGS="$LDFLAGS -shared $(pkg-config --libs dbus-1)" > make.out 2>&1 &&
	make -C wrapper2 CC="$CC" CFLAGS="$CPPFLAGS $CFLAGS" LDFLAGS="$LDFLAGS" >> make.out 2>&1 ||
	    { grep -m20 -E "error:|Error [0-9]" make.out; tail -20 make.out; exit 1; }
	make -C libhgw install DESTDIR="$PR" PREFIX=/usr/local > install.out 2>&1 &&
	make -C wrapper2 install DESTDIR="$PR" PREFIX=/usr/local >> install.out 2>&1 ||
	    { tail -20 install.out; exit 1; }
	# its links name /usr/local/lib/libhgw.so.N: relative, for the sysroot
	local l
	for l in "$PR"/usr/local/lib/lib*.so*; do
		[ -L "$l" ] && ln -sf "$(basename "$(readlink "$l")")" "$l"
	done
	return 0
}

custom_make() {
	# one job: such Makefiles often miss dependencies between targets
	make -j1 CC="$CC" CXX="$CXX" AR="$AR" \
	    CFLAGS="$CPPFLAGS $CFLAGS $LDFLAGS" CXXFLAGS="$CPPFLAGS $CXXFLAGS $LDFLAGS" \
	    LDFLAGS="$LDFLAGS" PREFIX=/usr "${HOSTTOOLS[@]}" > make.out 2>&1 ||
	    { grep -m20 -E "error:|Error [0-9]" make.out; tail -20 make.out; exit 1; }
	make install DESTDIR="$PR" PREFIX=/usr "${HOSTTOOLS[@]}" > install.out 2>&1 ||
	    { tail -20 install.out; exit 1; }
	usr_to_local "$PR"
}

usr_to_local() {
	local d=$1 x
	[ -d "$d/usr" ] || return 0
	mkdir -p "$d/usr/local"
	for x in "$d"/usr/*; do
		[ "$x" = "$d/usr/local" ] && continue
		cp -a "$x" "$d/usr/local/"
		rm -rf "$x"
	done
	if [ -d "$d/usr/local/lib/pkgconfig" ]; then
		mkdir -p "$d/usr/local/libdata/pkgconfig"
		mv "$d"/usr/local/lib/pkgconfig/* "$d/usr/local/libdata/pkgconfig/"
		rmdir "$d/usr/local/lib/pkgconfig"
	fi
	find "$d/usr/local/libdata/pkgconfig" -name '*.pc' -exec sed -i \
	    -e 's|^prefix=/usr$|prefix=/usr/local|' \
	    -e 's|=/usr/lib|=/usr/local/lib|' -e 's|=/usr/include|=/usr/local/include|' \
	    {} + 2> /dev/null || true
}

# alarmd: libalarm, the clients' library, and the daemon, which follows
# clockd's time changes (libtime.h from the sysroot). FreeBSD has no RTC
# wake-up alarm (patches/alarmd/0002): an alarm rings while the system
# runs. dsme (power up for an alarm) it asks over D-Bus; FreeBSD has none.
# Started at boot by rc.d/alarmd (extra/alarmd), as root like the session.
custom_alarmd() {
	local m=(CC="$CC" AR="$AR" PREFIX=/usr/local ROOT="$PR"
	    PKGCFGDIR=/usr/local/libdata/pkgconfig
	    DBUS_DIR=/usr/local/etc/dbus-1/system.d
	    LIBTIME_H="$SYSROOT/usr/local/include/clockd/libtime.h" USE_LIBTIME=n)
	local pk="glib-2.0 dbus-glib-1 dbus-1"
	# LDFLAGS on the command line replace the Makefile's, which name the
	# soname: the users would need libalarm.so, the development link
	make -j1 "${m[@]}" PKG_NAMES="$pk" CFLAGS="$CPPFLAGS $CFLAGS -std=gnu99 -fPIC" \
	    LDFLAGS="$LDFLAGS -Wl,-soname,libalarm.so.2" libalarm.a libalarm.so.2 > make.out 2>&1 ||
	    { grep -m20 -E "error:|Error [0-9]" make.out; exit 1; }
	make -j1 "${m[@]}" PKG_NAMES="$pk conic osso-systemui-dbus mce dsme_dbus_if statusbar-alarm" \
	    CFLAGS="$CPPFLAGS $CFLAGS -std=gnu99" LDFLAGS="$LDFLAGS" alarmd >> make.out 2>&1 ||
	    { grep -m20 -E "error:|Error [0-9]" make.out; tail -5 make.out; exit 1; }
	make "${m[@]}" install-libalarm install-libalarm-dev install-alarmd-dbus-system \
	    > install.out 2>&1 || { tail -20 install.out; exit 1; }
	install -d "$PR/usr/local/sbin"
	install -m 755 alarmd "$PR/usr/local/sbin/alarmd"
}

# dsme's D-Bus names only (libdsme_dbus_if), for alarmd: FreeBSD runs no
# dsme, libdsme's socket protocol is of no use there
custom_libdsme() {
	$CC $CPPFLAGS $CFLAGS -fPIC -shared -o libdsme_dbus_if.so.0 \
	    -Wl,-soname,libdsme_dbus_if.so.0 modules/dsme_dbus_if.c $LDFLAGS > make.out 2>&1 ||
	    { grep -m20 "error" make.out; exit 1; }
	install -d "$PR/usr/local/lib" "$PR/usr/local/include/dsme" \
	    "$PR/usr/local/libdata/pkgconfig"
	install -m 755 libdsme_dbus_if.so.0 "$PR/usr/local/lib/"
	ln -s libdsme_dbus_if.so.0 "$PR/usr/local/lib/libdsme_dbus_if.so"
	install -m 644 modules/dsme_dbus_if.h "$PR/usr/local/include/dsme/"
	sed 's|^prefix=/usr$|prefix=/usr/local|' dsme_dbus_if.pc \
	    > "$PR/usr/local/libdata/pkgconfig/dsme_dbus_if.pc"
}

# The system UI's D-Bus names: headers only (its install wants doxygen)
custom_osso_systemui_dbus_dev() {
	install -d "$PR/usr/local/include/systemui" "$PR/usr/local/libdata/pkgconfig"
	install -m 644 include/systemui/*.h "$PR/usr/local/include/systemui/"
	sed 's|^prefix=/usr$|prefix=/usr/local|' osso-systemui-dbus.pc \
	    > "$PR/usr/local/libdata/pkgconfig/osso-systemui-dbus.pc"
}

# libplayback: its Makefile links with the host's libtool, for Linux
custom_libplayback() {
	local o=() f
	for f in src/*.c; do
		# shellcheck disable=SC2046
		$CC $CPPFLAGS $CFLAGS -fPIC -I include $(pkg-config --cflags dbus-1) \
		    -c "$f" -o "${f%.c}.o" >> make.out 2>&1 ||
		    { grep -m20 "error:" make.out; exit 1; }
		o+=("${f%.c}.o")
	done
	# shellcheck disable=SC2046
	$CC -shared -o libplayback-1.so.0 -Wl,-soname,libplayback-1.so.0 "${o[@]}" \
	    $LDFLAGS $(pkg-config --libs dbus-1) >> make.out 2>&1 ||
	    { grep -m20 "error" make.out; exit 1; }
	install -d "$PR/usr/local/lib" "$PR/usr/local/include/libplayback-1/libplayback" \
	    "$PR/usr/local/libdata/pkgconfig"
	install -m 755 libplayback-1.so.0 "$PR/usr/local/lib/"
	ln -s libplayback-1.so.0 "$PR/usr/local/lib/libplayback-1.so"
	install -m 644 include/libplayback/playback.h include/libplayback/playback-macros.h \
	    include/libplayback/playback-types.h "$PR/usr/local/include/libplayback-1/libplayback/"
	sed 's|^prefix=/usr$|prefix=/usr/local|' libplayback-1.pc \
	    > "$PR/usr/local/libdata/pkgconfig/libplayback-1.pc"
}

# The alarm dialog, a system UI plugin: the notification plugin it links
# is in hildon-desktop's library directory
custom_osso_systemui_alarm() {
	local LDFLAGS="$LDFLAGS -L$SYSROOT/usr/local/lib/hildon-desktop -Wl,-rpath,/usr/local/lib/hildon-desktop"
	custom_make
}

# The keyboard layouts are compiled at build time by libimlayouts' gen_vkb:
# one for the host
pre_hildon_input_method_plugins() {
	local l=$SRC/libimlayouts
	# shellcheck disable=SC2046
	env -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_SYSROOT_DIR /usr/bin/cc -O2 \
	    -o "$H/hostbin/gen_vkb" "$l/gen_vkb.c" "$l/imlayouts.c" -I"$l" \
	    $(env -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_SYSROOT_DIR \
	    pkg-config --cflags --libs glib-2.0 expat)
}

# cal-home-widget: C compiled as C++ (its Makefile sets CC=g++)
custom_cal_home_widget() {
	local CC=$CXX
	custom_make
}

# braek: one source file; its makefile installs into debian/
custom_braek() {
	# shellcheck disable=SC2046
	$CC $CPPFLAGS $CFLAGS $LDFLAGS -o braek braek.c \
	    $(pkg-config --cflags --libs cairo cairo-png gtk+-2.0) > make.out 2>&1 ||
	    { grep -m20 "error" make.out; exit 1; }
	install -D -m 755 braek "$PR/usr/local/bin/braek"
	install -D -m 644 braek.png "$PR/usr/local/share/icons/hicolor/scalable/apps/braek.png"
	install -D -m 644 braek.desktop "$PR/usr/local/share/applications/hildon/braek.desktop"
}

# --- the Maemo 5 ports ------------------------------------------------
# headphoned: the X session script and the udev rule are Leste's; FreeBSD
# gets the daemon (evdev; the Pro1X has no jack driver there yet)
custom_headphoned() {
	# CFLAGS from the environment: the Makefile adds pkg-config's to them
	CFLAGS="$CPPFLAGS $CFLAGS" make CC="$CC" LDFLAGS="$LDFLAGS" > make.out 2>&1 ||
	    { grep -m20 "error" make.out; exit 1; }
	install -D -m 755 headphoned "$PR/usr/local/bin/headphoned"
}

custom_pedometerhomewidget() {
	# CFLAGS from the environment: the Makefile adds pkg-config's to them
	CFLAGS="$CPPFLAGS $CFLAGS" make CC="$CC" LDFLAGS="$LDFLAGS" > make.out 2>&1 ||
	    { grep -m20 "error" make.out; exit 1; }
	make install DESTDIR="$PR" > install.out 2>&1 || { tail install.out; exit 1; }
}

custom_wifi_assistant() {
	make install DESTDIR="$PR" PREFIX=/usr/local > install.out 2>&1 ||
	    { tail install.out; exit 1; }
	mkdir -p "$PR/usr/local"
	mv "$PR/etc" "$PR/usr/local/etc"
}

# The upstream code is in submodules
pre_gpodder_leste() {
	git -C "$SRC/gpodder-leste" submodule update -q --init --recursive &&
	    cp -a "$SRC/gpodder-leste/upstream/." "$BUILD/gpodder-leste/upstream/"
}

custom_gpodder_leste() {
	make install DESTDIR="$PR" PREFIX=/usr/local > install.out 2>&1 ||
	    { tail install.out; exit 1; }
}

# --- Qt 5 ------------------------------------------------------------------
# FreeBSD's Qt 5 (packages in the sysroot) with the PC's moc, uic and rcc
# of the same version: qmake through a qt.conf that points the target's
# paths into the sysroot and its tools to the host; CMake through a
# toolchain file, with Qt5's tool targets set to the host's.
qt_host() {
	local q=$H/qt5host t c
	# FreeBSD's Qt packages make qconfig-modules.h at install (a trigger
	# the sysroot does not run): one include per module's qconfig-*.h,
	# rewritten only when that changes (builds running meanwhile read it)
	c=$SYSROOT/usr/local/include/qt5/QtCore
	for t in "$c"/qconfig-*.h; do
		case ${t##*/} in qconfig-modules.h|qconfig-bootstrapped.h) continue ;; esac
		echo "#include <QtCore/${t##*/}>"
	done > "$q.modules.h"
	if cmp -s "$q.modules.h" "$c/qconfig-modules.h"; then rm "$q.modules.h"
	else mv "$q.modules.h" "$c/qconfig-modules.h"; fi
	# Qt's CMake files name /usr/local absolutely: copies that name the
	# sysroot, in a root of their own searched before the sysroot
	# (toolchain.cmake). Only changed files are replaced: a CMake build
	# running meanwhile (QtWebKit's) would configure itself again.
	rm -rf "$q/cmakeroot.new"
	mkdir -p "$q/cmakeroot.new/usr/local/lib/cmake"
	for t in "$SYSROOT"/usr/local/lib/cmake/Qt5*; do
		cp -a "$t" "$q/cmakeroot.new/usr/local/lib/cmake/"
	done
	find "$q/cmakeroot.new" -name '*.cmake' -exec sed -i \
	    -e "s|_install_prefix \"/usr/local\"|_install_prefix \"$SYSROOT/usr/local\"|" \
	    -e "s|\"/usr/local/|\"$SYSROOT/usr/local/|g" {} +
	mkdir -p "$q/cmakeroot"
	rsync -rlc --delete "$q/cmakeroot.new/" "$q/cmakeroot/"
	rm -rf "$q/cmakeroot.new"
	# qmake -query with the target's paths (no sysroot), for CMake projects
	# that install where Qt's own files are (QtWebKit's KDEInstallDirs)
	mkdir -p "$q/bin"
	cat > "$q/bin/qmake-target" <<-QMAKE
	#!/bin/sh
	[ "\$1" = -query ] && [ -n "\$2" ] || exit 1
	case \$2 in QT_HOST_DATA) v=QT_INSTALL_ARCHDATA ;; *) v=\$2 ;; esac
	r=\$(/usr/bin/qmake-qt5 -query -qtconf "$q/qt.conf" "\$v/raw" 2> /dev/null) &&
	    { echo "\$r"; exit 0; }
	exec /usr/bin/qmake-qt5 -query -qtconf "$q/qt.conf" "\$2"
	QMAKE
	chmod 755 "$q/bin/qmake-target"
	[ -f "$q/qt.conf" ] && return 0
	mkdir -p "$q/bin"
	for t in moc uic rcc qdbusxml2cpp qdbuscpp2xml lrelease lupdate qmlcachegen; do
		ln -sf "/usr/bin/$t-qt5" "$q/bin/$t"
	done
	# Qt modules' header generator (qt_module.prf)
	ln -sf /usr/bin/syncqt.pl-qt5 "$q/bin/syncqt"
	# Leste's "maemo" spec (qtstyleplugins/spec) on FreeBSD's: the maemo5
	# scope of .pro files, Q_WS_MAEMO_5
	mkdir -p "$q/mkspecs/freebsd-maemo"
	cat > "$q/mkspecs/freebsd-maemo/qmake.conf" <<-SPEC
	include($SYSROOT/usr/local/lib/qt5/mkspecs/freebsd-clang/qmake.conf)
	QMAKE_PLATFORM += maemo5
	DEFINES += Q_WS_MAEMO_5
	SPEC
	echo "#include \"$SYSROOT/usr/local/lib/qt5/mkspecs/freebsd-clang/qplatformdefs.h\"" \
	    > "$q/mkspecs/freebsd-maemo/qplatformdefs.h"
	cat > "$q/qt.conf" <<-QTCONF
	[Paths]
	Prefix=/usr/local
	Headers=include/qt5
	Libraries=lib/qt5
	ArchData=lib/qt5
	Data=share/qt5
	Binaries=lib/qt5/bin
	LibraryExecutables=lib/qt5/libexec
	Plugins=lib/qt5/plugins
	Imports=lib/qt5/imports
	Qml2Imports=lib/qt5/qml
	Translations=share/qt5/translations
	Sysroot=$SYSROOT
	SysrootifyPrefix=true
	TargetSpec=freebsd-clang
	HostSpec=linux-g++
	HostPrefix=$q
	HostBinaries=bin
	HostLibraries=/usr/lib
	HostData=$SYSROOT/usr/local/lib/qt5
	QTCONF
	cat > "$q/toolchain.cmake" <<-CMAKE
	set(CMAKE_SYSTEM_NAME FreeBSD)
	set(CMAKE_SYSTEM_PROCESSOR aarch64)
	set(CMAKE_SYSROOT $SYSROOT)
	set(CMAKE_C_COMPILER clang)
	set(CMAKE_CXX_COMPILER clang++)
	set(CMAKE_C_COMPILER_TARGET $TARGET)
	set(CMAKE_CXX_COMPILER_TARGET $TARGET)
	set(CMAKE_LINKER_TYPE LLD)
	set(CMAKE_FIND_ROOT_PATH $q/cmakeroot $SYSROOT)
	set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
	set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
	set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
	set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
	set(CMAKE_PREFIX_PATH /usr/local /usr/local/lib/qt5)
	CMAKE
	cat > "$q/hosttools.cmake" <<-CMAKE
	# Qt5's tools from the host (Qt5CoreConfigExtras keeps targets that exist)
	foreach(t moc rcc)
	  if(NOT TARGET Qt5::\${t})
	    add_executable(Qt5::\${t} IMPORTED)
	    set_target_properties(Qt5::\${t} PROPERTIES IMPORTED_LOCATION $q/bin/\${t})
	  endif()
	endforeach()
	if(NOT TARGET Qt5::uic)
	  add_executable(Qt5::uic IMPORTED)
	  set_target_properties(Qt5::uic PROPERTIES IMPORTED_LOCATION $q/bin/uic)
	endif()
	foreach(t qdbusxml2cpp qdbuscpp2xml)
	  if(NOT TARGET Qt5::\${t})
	    add_executable(Qt5::\${t} IMPORTED)
	    set_target_properties(Qt5::\${t} PROPERTIES IMPORTED_LOCATION $q/bin/\${t})
	  endif()
	endforeach()
	foreach(t lrelease lupdate)
	  if(NOT TARGET Qt5::\${t})
	    add_executable(Qt5::\${t} IMPORTED)
	    set_target_properties(Qt5::\${t} PROPERTIES IMPORTED_LOCATION $q/bin/\${t})
	  endif()
	endforeach()
	CMAKE
}

# qmake [project] [qmake arguments...]: configure, build, install into $PR
qt_qmake() {
	local pro=${1:-}
	[ $# -gt 0 ] && shift
	qt_host
	/usr/bin/qmake-qt5 -qtconf "$H/qt5host/qt.conf" -spec "$H/qt5host/mkspecs/freebsd-maemo" \
	    QMAKE_CC="$CC" QMAKE_CXX="$CXX" QMAKE_LINK="$CXX" QMAKE_LINK_SHLIB="$CXX" \
	    QMAKE_LINK_C="$CC" QMAKE_LINK_C_SHLIB="$CC" QMAKE_AR="llvm-ar cqs" \
	    QMAKE_STRIP=llvm-strip QMAKE_OBJCOPY=llvm-objcopy \
	    QMAKE_CFLAGS+="$CPPFLAGS $CFLAGS" QMAKE_CXXFLAGS+="$CPPFLAGS $CXXFLAGS" \
	    QMAKE_LFLAGS+="$LDFLAGS" QMAKE_INCDIR= QMAKE_LIBDIR= QMAKE_INCDIR_X11= \
	    QMAKE_LIBDIR_X11= QMAKE_INCDIR_OPENGL= QMAKE_LIBDIR_OPENGL= \
	    QMAKE_INCDIR_EGL= QMAKE_LIBDIR_EGL= \
	    PKG_CONFIG_SYSROOT_DIR="$SYSROOT" PKG_CONFIG_LIBDIR="$PKG_CONFIG_LIBDIR" \
	    QMAKE_LRELEASE="$H/qt5host/bin/lrelease" \
	    LIBS+="-Wl,--as-needed -lintl -Wl,--no-as-needed" \
	    PREFIX=/usr/local "$@" $pro > qmake.out 2>&1 ||
	    { tail -20 qmake.out; exit 1; }
	make -j"$JOBS" > make.out 2>&1 ||
	    { grep -m20 -E "error:|Error [0-9]" make.out; tail -20 make.out; exit 1; }
	make install INSTALL_ROOT="$PR" > install.out 2>&1 ||
	    { tail -20 install.out; exit 1; }
	# qmake installs to the sysrootified target paths
	unsysroot "$PR"
}

# cmake [arguments...]: configure, build, install into $PR
qt_cmake() {
	qt_host
	cmake -S . -B _build -DCMAKE_TOOLCHAIN_FILE="$H/qt5host/toolchain.cmake" \
	    -DCMAKE_PROJECT_INCLUDE="$H/qt5host/hosttools.cmake" \
	    -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_BUILD_TYPE=Release \
	    -DCMAKE_C_FLAGS="$CPPFLAGS $CFLAGS" -DCMAKE_CXX_FLAGS="$CPPFLAGS $CXXFLAGS" \
	    -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS" -DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS" \
	    -DCMAKE_MODULE_LINKER_FLAGS="$LDFLAGS" "$@" > cmake.out 2>&1 ||
	    { tail -30 cmake.out; exit 1; }
	cmake --build _build -j"$JOBS" > make.out 2>&1 ||
	    { grep -m20 -E "error:|Error [0-9]" make.out; tail -20 make.out; exit 1; }
	DESTDIR="$PR" cmake --install _build > install.out 2>&1 ||
	    { tail -20 install.out; exit 1; }
}

# The styles, platform themes and QtMaemo5, a Qt module (qt_module.prf:
# headers and .pri into the Qt tree)
custom_qtstyleplugins() {
	qt_qmake qtstyleplugins.pro
}

# GConf for Qt; its moc must be the host's
pre_libgq_gconf() {
	export MOC=$H/qt5host/bin/moc
	qt_host
}

# The Qt apps: qmake with Leste's maemo spec, or CMake
custom_osso_calculator() { qt_qmake osso-calculator.pro; }
custom_clock_ui() { qt_qmake Alarm.pro; }
custom_qalendar() { qt_qmake qalendar.pro; }
custom_countdowntimer() { qt_qmake countdowntimer.pro; }
custom_easylist() { qt_qmake easylist.pro; }
custom_flappybird() { qt_qmake flappybird.pro; }
custom_qshot() { qt_qmake screenshot.pro; }
custom_mihphoto() { qt_qmake mihphoto.pro; }
custom_hamsterfiler() { qt_cmake; }

# --- the web engines -------------------------------------------------------
# FFmpeg 8.1's shared libraries (the release FreeBSD's port had last): what
# release_1's QtWebEngine links (libavcodec.so.62, ...), beside quarterly's
# FFmpeg 9. Only the versioned libraries: no headers, no unversioned links,
# so nothing else builds on them. LGPL (no GPL or non-free parts); its own
# decoders only, as Chromium uses them.
custom_ffmpeg8_libs() {
	# configure takes CPPFLAGS and the like from the environment: not
	# the Leste parts' (glibc-compat.h)
	local e=(env -u CPPFLAGS -u CFLAGS -u CXXFLAGS -u LDFLAGS)
	"${e[@]}" ./configure --prefix=/usr/local --enable-cross-compile --target-os=freebsd \
	    --arch=aarch64 --cc="clang --target=$TARGET" --cxx="clang++ --target=$TARGET" \
	    --ld="clang --target=$TARGET -fuse-ld=lld" --sysroot="$SYSROOT" \
	    --ar=llvm-ar --ranlib=llvm-ranlib --nm=llvm-nm --strip=llvm-strip \
	    --pkg-config=pkg-config --extra-cflags="-ffile-prefix-map=$BUILD/=/usr/src/" \
	    --enable-shared --disable-static --disable-programs --disable-doc \
	    --disable-autodetect --enable-zlib --disable-avdevice --disable-avfilter \
	    --disable-swscale > configure.out 2>&1 || { tail -30 configure.out; exit 1; }
	"${e[@]}" make -j"$JOBS" > make.out 2>&1 ||
	    { grep -m20 -E "error:|Error [0-9]" make.out; tail -20 make.out; exit 1; }
	"${e[@]}" make install DESTDIR="$PWD/_inst" > install.out 2>&1 ||
	    { tail -20 install.out; exit 1; }
	install -d "$PR/usr/local/lib"
	cp -a _inst/usr/local/lib/lib*.so.* "$PR/usr/local/lib/"
}

# QtWebKit's last release (2020), with the patches of FreeBSD's port (until
# 2023) and Arch's (ICU, Python, bison, compilers) in patches/qt5-webkit.
# WebKit 1, the widgets (QWebView), which the Maemo apps use; no WebKit 2
# (QML), video, geolocation or XSLT. Installed where FreeBSD's Qt has its
# files: KDEInstallDirs asks qmake (qmake-target: the target's paths).
custom_qt5_webkit() {
	local CPPFLAGS="-ffile-prefix-map=$SYSROOT= -ffile-prefix-map=$BUILD/=/usr/src/"
	local CXXFLAGS="-O2 -pipe -w -DNDEBUG" CFLAGS="-O2 -pipe -w"
	qt_cmake -G Ninja -DPORT=Qt -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
	    -DQMAKE_EXECUTABLE="$H/qt5host/bin/qmake-target" \
	    -DPYTHON_EXECUTABLE=/usr/bin/python3 -DRUBY_EXECUTABLE=/usr/bin/ruby \
	    -DPERL_EXECUTABLE=/usr/bin/perl -DBISON_EXECUTABLE=/usr/bin/bison \
	    -DGPERF_EXECUTABLE=/usr/bin/gperf \
	    -DENABLE_WEBKIT2=OFF -DENABLE_TOOLS=OFF -DENABLE_API_TESTS=OFF \
	    -DENABLE_TEST_SUPPORT=OFF -DENABLE_QT_WEBCHANNEL=OFF \
	    -DENABLE_GEOLOCATION=OFF -DENABLE_DEVICE_ORIENTATION=OFF \
	    -DENABLE_VIDEO=OFF -DENABLE_WEB_AUDIO=OFF -DUSE_GSTREAMER=OFF \
	    -DUSE_QT_MULTIMEDIA=OFF -DUSE_LIBHYPHEN=OFF -DUSE_WOFF2=OFF \
	    -DENABLE_XSLT=OFF -DUSE_LD_GOLD=OFF \
	    -DECM_PKGCONFIG_INSTALL_DIR=/usr/local/libdata/pkgconfig
}

# --- meson ---------------------------------------------------------------------
# meson [options...]: configure for the sysroot (a cross file from the
# environment build.sh sets up), build, install into $PR
meson_cross() {
	local cf=$PWD/cross.ini
	{
		echo "[binaries]"
		echo "c = ['clang', '--target=$TARGET', '--sysroot=$SYSROOT']"
		echo "cpp = ['clang++', '--target=$TARGET', '--sysroot=$SYSROOT']"
		echo "ar = 'llvm-ar'"
		echo "strip = 'llvm-strip'"
		echo "pkg-config = '$H/hostbin/pkg-config'"
		echo "[host_machine]"
		echo "system = 'freebsd'"
		echo "cpu_family = 'aarch64'"
		echo "cpu = 'aarch64'"
		echo "endian = 'little'"
		echo "[built-in options]"
		printf "c_args = [%s]\n" "$(meson_list $CPPFLAGS $CFLAGS $MESON_CFLAGS)"
		printf "cpp_args = [%s]\n" "$(meson_list $CPPFLAGS $CXXFLAGS $MESON_CFLAGS)"
		printf "c_link_args = [%s]\n" "$(meson_list $LDFLAGS)"
		printf "cpp_link_args = [%s]\n" "$(meson_list $LDFLAGS)"
	} > "$cf"
	meson setup _build --cross-file "$cf" --prefix=/usr/local --libdir=lib \
	    --buildtype=release -Dwerror=false "$@" > meson.out 2>&1 ||
	    { tail -30 meson.out; exit 1; }
	ninja -C _build -j"$JOBS" > make.out 2>&1 ||
	    { grep -m20 -E "error:|FAILED" make.out; tail -20 make.out; exit 1; }
	DESTDIR="$PR" meson install -C _build --no-rebuild > install.out 2>&1 ||
	    { tail -20 install.out; exit 1; }
}

# words as a meson list: 'a', 'b'
meson_list() {
	local w out=
	for w in "$@"; do
		out="$out${out:+, }'$w'"
	done
	printf '%s' "$out"
}

# Ported Linux programs find their kernel interfaces here (misc/fastrpc.h,
# linux/qrtr.h)
DSP_INCLUDE=$REPO/freebsd/dsp/include

# hexagonrpcd: serves the ADSP its sensor configuration and registry
custom_hexagonrpc() {
	local MESON_CFLAGS="-I$DSP_INCLUDE"
	meson_cross
	# systemd's units: FreeBSD has rc.d/hexagonrpcd (extra/hexagonrpc)
	rm -rf "$PR/usr/local/lib/systemd"
}

# QRTR sockets for GLib (libqmi's QRTR transport)
custom_libqrtr_glib() {
	local MESON_CFLAGS="-I$DSP_INCLUDE"
	meson_cross -Dintrospection=false -Dgtk_doc=false
}

# libqmi as Leste builds it (1.34), without what is Linux's own: udev,
# RMNET links (netlink), QMI over MBIM, the firmware updater
custom_libqmi() {
	local MESON_CFLAGS="-I$DSP_INCLUDE"
	meson_cross -Dqrtr=true -Dmbim_qmux=false -Drmnet=false -Dudev=false \
	    -Dfirmware_update=false -Dmm_runtime_check=false -Dintrospection=false \
	    -Dgtk_doc=false -Dman=false -Dbash_completion=false
}

# The ADSP's sensors over QMI (Leste: v0.4.4 with its two patches)
custom_libssc() {
	meson_cross
}

# libqrtr and qrtr-lookup (the name service is the kernel's)
custom_qrtr() {
	local MESON_CFLAGS="-I$DSP_INCLUDE"
	meson_cross
}

# linux-msm's plain Makefiles: the cross compiler, libqrtr and LIBS,
# installed below /usr/local without their systemd units and udev rules
# (FreeBSD has rc.d scripts: extra/<part>)
msm_make() {
	make -j"$JOBS" CC="$CC" CFLAGS="$CPPFLAGS $CFLAGS -I$DSP_INCLUDE -Wall" \
	    LDFLAGS="$LDFLAGS $1" prefix=/usr/local > make.out 2>&1 ||
	    { grep -m20 -E "error:|Error [0-9]" make.out; tail -20 make.out; exit 1; }
	make install DESTDIR="$PR" prefix=/usr/local > install.out 2>&1 ||
	    { tail -20 install.out; exit 1; }
	rm -rf "$PR/usr/local/lib/systemd" "$PR/usr/local/lib/udev"
	rmdir "$PR/usr/local/lib" 2> /dev/null || :
}

# The modem's file systems (EFS) from its partitions, as Leste runs it
# (-r -P: read from /dev/gpt, the modem's writes kept in memory). The QMI
# code is generated already: newer than its .qmi, no qmic needed.
custom_rmtfs() {
	touch qmi_rmtfs.c qmi_rmtfs.h
	msm_make "-lqrtr -lpthread"
}

# The modem's files over QRTR's TFTP (wlanmdsp.mbn for the Wi-Fi); the
# script that copies the persist partition's files is Linux's
custom_tqftpserv() {
	local MESON_CFLAGS="-I$DSP_INCLUDE"
	meson_cross
	rm -f "$PR/usr/local/bin/populate-readwrite.sh"
}

# Which DSP serves which protection domain (the firmware's .jsn files):
# Linux's kernel has it built in, FreeBSD runs it in userspace
custom_pd_mapper() {
	touch servreg_loc.c servreg_loc.h
	msm_make "-lqrtr -llzma"
}

custom_jib() { qt_cmake -DMAEMO=On; }
# qt-components: what its configure script writes (it runs the host's
# qmake), the MeeGo style only, no examples or tests
custom_qt_components() {
	printf 'Q_COMPONENTS_SOURCE_TREE = %s\nQ_COMPONENTS_BUILD_TREE = %s\n' "$PWD" "$PWD" \
	    > .qmake.cache
	mkdir -p features
	echo "CONFIG += meego" > features/qt-components-config.prf
	QMAKEFEATURES=$PWD/features qt_qmake qt-components.pro -r CONFIG+=meego
}

custom_meego_clock() { qt_qmake meego-clock.pro; }

# MeeCast: its libraries (sqlite3, libxml2 bundled), the weather sources
# and their station databases, the QML application (omweather-qml). The
# bundled libxml2 turns its thread support on by _REENTRANT or
# _POSIX_C_SOURCE, which glibc defines by default and FreeBSD does not. Its
# QMAKE_LRELEASE is gettext's msgfmt (it compiles .po files). Its own tree,
# /opt/com.meecast.omweather (Sailfish's place for it, built into code and
# QML too), goes to /usr/local/lib/meecast.
custom_meecast() {
	grep -rlZ /opt/com.meecast.omweather . --include='*.pro' --include='*.pri' \
	    --include='*.cpp' --include='*.h' --include='*.c' --include='*.qml' \
	    --include='*.js' --include='*.desktop' --include='*.xml' --include='*.in' |
	    xargs -0 -r sed -i 's|/opt/com\.meecast\.omweather|/usr/local/lib/meecast|g'
	qt_qmake meecast.pro QMAKE_CFLAGS+=-D_REENTRANT QMAKE_LRELEASE=msgfmt &&
	    usr_to_local "$PR" &&
	# started directly, not through Sailfish's invoker (extra/meecast has
	# Hildon's menu entry)
	sed -i 's|^Exec=.*omweather-qml.*|Exec=/usr/local/lib/meecast/bin/omweather-qml|' \
	    "$PR/usr/local/share/applications/meecast.desktop"
}

# Dorian installs its menu entry and icons below /usr/share
custom_dorian() { qt_qmake dorian.pro && usr_to_local "$PR"; }

# maemo-launcher starts apps built as shared objects from a preloaded
# process (Linux-specific). On FreeBSD apps are plain programs: only
# maemo-launcher-app.pc, without its -shared, for the apps that ask for it
custom_maemo_launcher() {
	local v
	v=$(sed -n '1s/^[^(]*(\([^)]*\)).*/\1/p' debian/changelog)
	install -d "$PR/usr/local/libdata/pkgconfig"
	printf 'Name: maemo-launcher-app\nDescription: maemo-launcher application startup accelerator (FreeBSD: plain programs)\nVersion: %s\nLibs:\nCflags:\n' \
	    "$v" > "$PR/usr/local/libdata/pkgconfig/maemo-launcher-app.pc"
}
