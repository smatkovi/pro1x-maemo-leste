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
	"cityinfo|"
	"clockd|"
	"alarmd|!"
	"clock|"
	"hildon-time-zone-chooser|"
	"calendar-backend|"
	"icd2-osso-ic-dev|"
	"libconic|"
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
	"hildon-input-method-plugins|--disable-dependency-tracking"
	# Leste's extras
	"maepad|"
	"mstardict||-D_LIBCPP_ENABLE_CXX17_REMOVED_AUTO_PTR -Wno-reserved-user-defined-literal"
	"osso-mahjong|"
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
	# QMI over QRTR, the sensor client library, the step counter
	"hexagonrpc|!"
	"libqrtr-glib|!"
	"libqmi|!"
	"libssc|!"
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
	[hildon-input-method-plugins]=04c1d47222d0
	[maepad]=b50c9caadd86 [mstardict]=48f7924faed8
	[osso-mahjong]=fbe179709a15 [leafpad]=9ae1e0314158
	[desktop-cmd-exec]=e3ac73e7b031 [live-wallpaper]=d950d1e44e66
	[cal-home-widget]=52ef149cdca0 [maefat]=11bc84b6fccb [braek]=77ceda0d3d92
	[qtstyleplugins]=c371b3232025 [libgq-gconf]=8617c495b6ac
	[osso-calculator]=6fff52bb301f [clock-ui]=7c832feb877b [qalendar]=04ccfca3ca17
	[countdowntimer]=ddc1f7027af9 [easylist]=d5c5bcc6c677 [flappybird]=21f81151207a
	[qshot]=f10335de9c66 [mihphoto]=bab0cb18bc0e
	[hamsterfiler]=19ac58143e2e
	[ffmpeg8-libs]=464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c
	[qt5-webengine]=c5de7df005189a2ef727a40b13408ff3a14a0bbedce07af986aac59722d4cf09
	[qt5-webkit]=9ca126da9273664dd23a3ccd0c9bebceb7bb534bddd743db31caf6a5a6d4a9e6
	[jib]=5dd4a340d20d [dorian]=da92eda1d5d9
	[hexagonrpc]=40e50412812a
	[libqrtr-glib]=69791f8f2a09 [libqmi]=3f07d6e5b467 [libssc]=0cf77b93b557
	[headphoned]=d476cb4a8bf9 [pedometerhomewidget]=06d5246191fa
	[wifi-assistant]=604bbe98d8b5 [gpodder-leste]=2759445563b5
)
URL+=(
	[maepad]=$GM_EXTRAS/maepad.git
	[mstardict]=$GH_EXTRAS/mstardict.git [osso-mahjong]=$GH_EXTRAS/osso-mahjong.git
	[leafpad]=$GH_EXTRAS/leafpad.git [desktop-cmd-exec]=$GH_EXTRAS/desktop-cmd-exec.git
	[live-wallpaper]=$GH_EXTRAS/live-wallpaper.git
	[cal-home-widget]=$GH_EXTRAS/cal-home-widget.git [maefat]=$GH_EXTRAS/maefat.git
	[braek]=$GH_EXTRAS/braek.git
	[countdowntimer]=$GM_EXTRAS/countdowntimer.git [easylist]=$GH_EXTRAS/easylist.git
	[flappybird]=$GM_EXTRAS/flappybird.git [qshot]=$GH_EXTRAS/qshot.git
	[mihphoto]=$GH_EXTRAS/mihphoto.git
	[hamsterfiler]=$GH_EXTRAS/hamsterfiler.git
	[ffmpeg8-libs]=https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz
	[qt5-webengine]=pkg:release_1
	[qt5-webkit]=https://github.com/qtwebkit/qtwebkit/releases/download/qtwebkit-5.212.0-alpha4/qtwebkit-5.212.0-alpha4.tar.xz
	[jib]=$GH_EXTRAS/jib.git [dorian]=$GH_EXTRAS/dorian.git
	[hexagonrpc]=https://github.com/linux-msm/hexagonrpc.git
	[libqrtr-glib]=https://gitlab.freedesktop.org/mobile-broadband/libqrtr-glib.git
	[libqmi]=https://gitlab.freedesktop.org/mobile-broadband/libqmi.git
	[libssc]=https://codeberg.org/DylanVanAssche/libssc.git
	[headphoned]=https://github.com/smatkovi/headphoned.git
	[pedometerhomewidget]=https://github.com/smatkovi/pedometerhomewidget.git
	[wifi-assistant]=https://github.com/smatkovi/wifi-assistant.git
	[gpodder-leste]=https://github.com/smatkovi/gpodder-leste.git
)

# A plain Makefile: built with the cross compiler, installed below /usr,
# then moved below /usr/local (pkg-config files to libdata/pkgconfig)
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

# alarmd: only libalarm, the clients' library (the daemon wants dsme, mce,
# the system UI's interfaces and Linux's RTC alarm)
custom_alarmd() {
	local m=(CC="$CC" AR="$AR" PREFIX=/usr/local ROOT="$PR"
	    PKGCFGDIR=/usr/local/libdata/pkgconfig
	    PKG_NAMES="glib-2.0 dbus-glib-1 dbus-1" USE_LIBTIME=n)
	# LDFLAGS on the command line replace the Makefile's, which name the
	# soname: the users would need libalarm.so, the development link
	make -j1 "${m[@]}" CFLAGS="$CPPFLAGS $CFLAGS -std=gnu99 -fPIC" \
	    LDFLAGS="$LDFLAGS -Wl,-soname,libalarm.so.2" libalarm.a libalarm.so.2 > make.out 2>&1 ||
	    { grep -m20 -E "error:|Error [0-9]" make.out; exit 1; }
	make "${m[@]}" install-libalarm install-libalarm-dev > install.out 2>&1 ||
	    { tail -20 install.out; exit 1; }
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

custom_jib() { qt_cmake -DMAEMO=On; }
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
