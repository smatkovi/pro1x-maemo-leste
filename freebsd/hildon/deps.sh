# The FreeBSD packages Hildon builds on and runs with (freebsd/hildon/build.sh
# for the sysroot, rootfs.sh for the root image)
HILDON_PKGS="gtk2 gconf2 dbus dbus-glib gnome-menus libcanberra libcanberra-gtk2
	gdk-pixbuf-xlib startup-notification mesa-libs mesa-dri libglvnd png
	libSM libXcomposite libXdamage libXfixes libXi libXt libXrandr libXft
	sqlite3 libxml2 pango glib libinotify xorg-server libexif xrdb"
# Not needed to build against
HILDON_PRUNE="llvm* spirv-llvm-translator* python3* py3*-* perl5* samba* qt6-*"
# What the apps (apps.sh) build on and run with (the second part: what
# QtWebEngine and QtWebKit link)
APPS_PKGS="gtk3 vte poppler-glib upower gstreamer1 gstreamer1-plugins pulseaudio
	libical espeak-ng libsigc++ libxkbfile libXtst expat sdl12-compat
	sdl_mixer libsidplay libsndfile curl
	qt5-qmake qt5-buildtools qt5-core qt5-gui qt5-widgets qt5-dbus qt5-x11extras qt5-network qt5-xml
	qt5-sql qt5-svg qt5-declarative qt5-quickcontrols qt5-quickcontrols2
	qt5-multimedia qt5-concurrent qt5-websockets qt5-charts qt5-graphicaleffects
	qt5-linguisttools phonon-qt5 evolution-data-server
	qt5-location qt5-webchannel qt5-printsupport qt5-opengl libXScrnSaver libevent
	libpci libvpx minizip nss nspr openh264 opus re2 snappy webp jsoncpp lcms2
	double-conversion alsa-lib libxslt jpeg-turbo icu qt5-sqldrivers-sqlite3
	json-c protobuf-c"
