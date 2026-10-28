# The Nokia N9's user interface (MeeGo 1.2 Harmattan) ported to Qt 5,
# github.com/smatkovi/harmattan-qt5, for FreeBSD: the parts of that one
# repository (its subdirectories), at one pinned revision, built as the
# apps are (apps.sh: FreeBSD's Qt 5 in the sysroot, the PC's Qt tools).
# Sourced by build.sh; "harmattan" builds them all, in this order,
# harmattan-<directory> one.
#
# Each part is built from the repository's directory with /usr/ in its
# build files made /usr/local/ (FreeBSD's place for packages; the paths
# the parts build in, themes, plugin and data directories, follow them).
HQ5_URL=https://github.com/smatkovi/harmattan-qt5.git
HQ5_REV=d18933a3d165a451cfdac724382671f3538aaefb

HARMATTAN=(
	"libmlocale|!"
	"libmeegotouch|!"
	"contextkit|--disable-doc --disable-coverage MOC=$H/qt5host/bin/moc UIC=$H/qt5host/bin/uic RCC=$H/qt5host/bin/rcc"
	"libcontentaction|--disable-doc --disable-coverage MOC=$H/qt5host/bin/moc UIC=$H/qt5host/bin/uic RCC=$H/qt5host/bin/rcc"
	"mcompositor|!"
	"libdsme|!"
	"mce-dev|!"
	"qmsystem2|!"
	"duicontrolpanel|!"
	"system-ui|!"
	"statusmenu-plugins|!"
	"meegotouchhome|!"
	"meegotouch-controlpanelapplets|!"
	"meegofeedbackd|!"
	"meegofeedback-reactionmaps|!"
	"meego-im-framework|!"
	"libmeegoimengine|!"
	"meego-keyboard|!"
	"meego-im-settings-applet|!"
	"meegotouchtheme|!"
)
# Each part is "harmattan-<directory>" here (its source, root and package;
# some directories have the names of Leste's parts, mce-dev): the revision
# and the source its package names
for _p in "${HARMATTAN[@]}"; do
	REV[harmattan-${_p%%|*}]=$HQ5_REV
	URL[harmattan-${_p%%|*}]=$HQ5_URL
done
unset _p

# qmake for the parts' own configure scripts (they run "qmake" from PATH):
# FreeBSD's Qt through qt_host's qt.conf, the cross compiler, and a spec
# without Leste's Q_WS_MAEMO_5 (the Maemo spec of the apps)
hq5_qmake() {
	local q=$H/qt5host
	qt_host
	mkdir -p "$q/mkspecs/freebsd-harmattan" "$q/hq5bin"
	cat > "$q/mkspecs/freebsd-harmattan/qmake.conf" <<-SPEC
	include($SYSROOT/usr/local/lib/qt5/mkspecs/freebsd-clang/qmake.conf)
	SPEC
	echo "#include \"$SYSROOT/usr/local/lib/qt5/mkspecs/freebsd-clang/qplatformdefs.h\"" \
	    > "$q/mkspecs/freebsd-harmattan/qplatformdefs.h"
	cat > "$q/hq5bin/qmake" <<-QMAKE
	#!/bin/sh
	# libmeegotouch's features (meegotouch.prf, ...) are below share/qt5;
	# narrowing in initializer lists (qreal to int), which gcc only warns
	# about, is no error either
	export QMAKEFEATURES="$SYSROOT/usr/local/share/qt5/mkspecs/features\${QMAKEFEATURES:+:\$QMAKEFEATURES}"
	case "\$1" in
	-query|-v|--version) exec /usr/bin/qmake-qt5 -qtconf "$q/qt.conf" "\$@" ;;
	esac
	exec /usr/bin/qmake-qt5 -qtconf "$q/qt.conf" -spec "$q/mkspecs/freebsd-harmattan" \\
	    QMAKE_CC="$CC" QMAKE_CXX="$CXX" QMAKE_LINK="$CXX" QMAKE_LINK_SHLIB="$CXX" \\
	    QMAKE_LINK_C="$CC" QMAKE_LINK_C_SHLIB="$CC" QMAKE_AR="llvm-ar cqs" \\
	    QMAKE_STRIP=llvm-strip QMAKE_OBJCOPY=llvm-objcopy \\
	    QMAKE_CFLAGS+="$CPPFLAGS $CFLAGS -idirafter $REPO/freebsd/hildon/compat-harmattan/include" \\
	    QMAKE_CXXFLAGS+="$CPPFLAGS $CXXFLAGS -idirafter $REPO/freebsd/hildon/compat-harmattan/include -Wno-c++11-narrowing" \\
	    QMAKE_LFLAGS+="$LDFLAGS" QMAKE_INCDIR= QMAKE_LIBDIR= QMAKE_INCDIR_X11= \\
	    QMAKE_LIBDIR_X11= QMAKE_INCDIR_OPENGL= QMAKE_LIBDIR_OPENGL= \\
	    QMAKE_INCDIR_EGL= QMAKE_LIBDIR_EGL= \\
	    QMAKE_LRELEASE="$q/bin/lrelease" \\
	    LIBS+="-Wl,--as-needed -lintl -Wl,--no-as-needed" "\$@"
	QMAKE
	chmod 755 "$q/hq5bin/qmake"
}

# A part's qmake features (meegotouch.prf, ...) name /usr/local/ paths: in
# the sysroot's copy they name the sysroot's, for the parts built on them
# (what such a part installs there, unsysroot takes back out)
hq5_sysroot_features() {
	local pr=$1 f
	# (and the .pri files a feature includes, duicontrolpanel's dcpconfig.pri;
	# their prefixes stay, the headers below one are the sysroot's)
	find "$pr" \( -name '*.prf' -o -path '*/mkspecs/*.pri' \) -printf '%P\n' | while read -r f; do
		sed -i -E -e "s#(^|[^A-Za-z0-9_/.]|-[LI])/usr/local/#\\1$SYSROOT/usr/local/#g" \
		    -e "s#\\\$\\\$([A-Z_]*PREFIX)/include#$SYSROOT/usr/local/include#g" "$SYSROOT/$f"
	done
}

# /usr/ in the build files is /usr/local/ here
hq5_usr_local() {
	grep -rlZE '/usr(/|[[:space:]]*$)' . --include='*.pro' \
	    --include='*.pri' --include='*.prf' --include='*.prf.in' --include='*.pc.in' \
	    --include='*.pc' --include='*.in' 2> /dev/null |
	    xargs -0 -r sed -i -E -e 's#/usr/(include|lib|share|bin|sbin|libexec)#/usr/local/\1#g' \
	        -e 's#^([[:space:]]*[A-Z_]+[[:space:]]*[+*]?=[[:space:]]*)/usr[[:space:]]*$#\1/usr/local#' \
	        -e 's#^(prefix=)/usr[[:space:]]*$#\1/usr/local#'
}

# The parts with a configure script of their own that runs qmake: configure
# (hq5_configure_only), then build and install into $PR (hq5_make_install)
hq5_configure_only() {
	hq5_qmake
	hq5_usr_local
	PATH=$H/qt5host/hq5bin:$H/qt5host/bin:$PATH ./configure "$@" > configure.out 2>&1 ||
	    { tail -25 configure.out; exit 1; }
}

hq5_make_install() {
	PATH=$H/qt5host/hq5bin:$H/qt5host/bin:$PATH make -j"$JOBS" > make.out 2>&1 ||
	    { grep -m25 -E "error:|Error [0-9]" make.out; exit 1; }
	PATH=$H/qt5host/hq5bin:$H/qt5host/bin:$PATH make install INSTALL_ROOT="$PR" > install.out 2>&1 ||
	    { tail -20 install.out; exit 1; }
	unsysroot "$PR"
}

hq5_configure() {
	hq5_configure_only "$@"
	hq5_make_install
}

# A qmake part: qmake -r PROJECT [arguments], build, install into $PR
hq5_qmake_part() {
	local pro=$1
	shift
	hq5_qmake
	hq5_usr_local
	PATH=$H/qt5host/hq5bin:$H/qt5host/bin:$PATH qmake -r "$pro" CONFIG+=NODOCS \
	    CONFIG+=NOTESTS CONFIG+=notests CONFIG+=nodoc "$@" > qmake.out 2>&1 ||
	    { tail -25 qmake.out; exit 1; }
	hq5_make_install
}

custom_harmattan_libmlocale() {
	hq5_configure -prefix /usr/local -release -nomake demos -nomake tests -nomake benchmarks
}

# mgen and mmoc run during the build (and that of every part on it): built
# with the PC's Qt once configure has written the build tree's cache, left
# out of the cross build, and in hq5_qmake's PATH for the parts after it
custom_harmattan_libmeegotouch() {
	local t
	sed -i -e '/^[[:space:]]*mmoc \\$/d' -e '/^[[:space:]]*mgen \\$/d' projects.pro
	hq5_configure_only -prefix /usr/local -release -no-werror -nomake demos -nomake tests \
	    -nomake benchmarks
	for t in mgen mmoc; do
		# the PC's compiler and flags (Arch's qmake takes CFLAGS and LDFLAGS
		# from the environment, which are the cross build's)
		(cd "$t" && env -u CC -u CXX -u CPP -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS \
		    -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_SYSROOT_DIR -u PKG_CONFIG_PATH \
		    sh -c '/usr/bin/qmake-qt5 "$1.pro" > qmake-host.out 2>&1 &&
		    make -j"$2" > make-host.out 2>&1' sh "$t" "$JOBS") ||
		    { tail -n 20 "$t/qmake-host.out"; tail -n 20 "$t/make-host.out"; exit 1; }
		install -m 755 "$t/$t" "$H/qt5host/hq5bin/$t"
	done
	# m-servicefwgen too (duicontrolpanel's qmake runs it), from a copy of
	# its directory beside it (its .pro includes ../../mkspecs), into the
	# copy: the cross build packages its own
	rm -rf tools/host-m-servicefwgen
	cp -a tools/m-servicefwgen.d tools/host-m-servicefwgen
	find tools/host-m-servicefwgen -name '*.o' -delete
	(cd tools/host-m-servicefwgen && rm -f Makefile &&
	    env -u CC -u CXX -u CPP -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS \
	    -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_SYSROOT_DIR -u PKG_CONFIG_PATH \
	    sh -c '/usr/bin/qmake-qt5 m-servicefwgen.d.pro -after DESTDIR=. > qmake-host.out 2>&1 &&
	    make -j"$1" > make-host.out 2>&1' sh "$JOBS") ||
	    { tail -n 20 tools/host-m-servicefwgen/make-host.out; exit 1; }
	install -m 755 tools/host-m-servicefwgen/m-servicefwgen "$H/qt5host/hq5bin/m-servicefwgen"
	hq5_make_install
}

custom_harmattan_mcompositor() { hq5_qmake_part mcompositor.pro; }
# the key daemon's inotify is libinotify's here
# without qmkeyd, the key daemon on Linux's evdev and SO_PEERCRED
custom_harmattan_qmsystem2() {
	sed -i 's/^SUBDIRS  = system keyd$/SUBDIRS  = system/' libqmsystem2.pro
	hq5_qmake_part libqmsystem2.pro "LIBS+=-Wl,--as-needed -linotify -Wl,--no-as-needed"
}
# (backtrace(3) is in libexecinfo on FreeBSD)
custom_harmattan_duicontrolpanel() {
	hq5_qmake_part projects.pro "LIBS+=-lexecinfo"
	# its tests and their scripts
	rm -rf "$PR/usr/local/lib/duicontrolpanel-tests" "$PR/usr/local/share/duicontrolpanel-tests"
}
custom_harmattan_system_ui() { hq5_qmake_part project.pro; }
custom_harmattan_statusmenu_plugins() { hq5_qmake_part statusmenu-plugins.pro; }
# (it calls Xlib and XDamage itself)
custom_harmattan_meegotouchhome() { hq5_qmake_part projects.pro "LIBS+=-lX11 -lXdamage"; }
custom_harmattan_meegotouch_controlpanelapplets() { hq5_qmake_part meegotouch-controlpanelapplets.pro; }
custom_harmattan_meegofeedbackd() { hq5_qmake_part meegofeedbackd.pro; }
custom_harmattan_meegofeedback_reactionmaps() {
	hq5_qmake_part meegofeedback-reactionmaps.pro
	# the XRecord source only: meegofeedbackd plays every source it loads,
	# and the other reads Linux's input events
	rm -f "$PR/usr/local/lib/meegofeedbackd/sources/libmeegofeedback-reactionmap.so"
}
custom_harmattan_meego_im_framework() {
	hq5_qmake_part maliit-framework.pro CONFIG+=enable-legacy CONFIG+=enable-contextkit
}
custom_harmattan_libmeegoimengine() { hq5_qmake_part meegoimengine.pro; }
custom_harmattan_meego_keyboard() { hq5_qmake_part m-keyboard.pro; }
custom_harmattan_meego_im_settings_applet() { hq5_qmake_part mim-settings-applet.pro; }
custom_harmattan_meegotouchtheme() { hq5_qmake_part theme.pro; }

# libdsme: a Makefile of its own; mce-dev: mce's D-Bus names (headers)
custom_harmattan_libdsme() {
	hq5_usr_local
	# (its -s is a linker flag on every compile, which clang with -Werror
	# refuses; its "ar rf", which llvm-ar does not take)
	make -j"$JOBS" CC="$CC" AR=ar C_OPTFLAGS=-O2 > make.out 2>&1 ||
	    { grep -m20 -E "error:|Error [0-9]" make.out; exit 1; }
	make install DESTDIR="$PR" PREFIX=/usr/local > install.out 2>&1 || { tail -20 install.out; exit 1; }
	usr_to_local "$PR"
}
custom_harmattan_mce_dev() {
	local v
	v=$(sed -n '1s/^[^(]*(\([^)]*\)).*/\1/p' debian/changelog)
	install -d "$PR/usr/local/include/mce" "$PR/usr/local/libdata/pkgconfig"
	install -m 644 include/mce/*.h "$PR/usr/local/include/mce/"
	printf 'prefix=/usr/local\nincludedir=${prefix}/include\n\nName: mce\nDescription: Mode Control Entity\nVersion: %s\nCflags: -I${includedir}\n' \
	    "$v" > "$PR/usr/local/libdata/pkgconfig/mce.pc"
}

