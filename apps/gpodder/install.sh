#!/bin/bash
# gPodder for Maemo Leste: the current gPodder 4 core with the touch QML UI.
#
# Maemo 5's gPodder was gPodder 2.x with a PyGTK/Hildon UI on Python 2,
# neither of which Leste (Debian 12) has. This puts together gPodder 4:
#   - gpodder-core 4.17.2, podcastparser and minidb as pinned by gPodder for
#     Sailfish OS 4.18.1, with its up-to-date Python bridge (common/main.py)
#     and QML models (common/);
#   - the touch UI of gpodder-ui-qml (gPodder 4 QML UI reference
#     implementation, for phones);
# on PyOtherSide and Qt 5. Installs to /opt/gpodder, starts as gpodder-leste.
#
# Runs as root on Leste (the phone or the rootfs chroot). Needs network.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC:-/root/src}
DEST=/opt/gpodder
SAILFISH=https://github.com/gpodder/gpodder-sailfish.git
SAILFISH_COMMIT=356472d0a2055e5b4614580c1773bed8e5785d1f	# 4.18.1 + translations
UIQML=https://github.com/gpodder/gpodder-ui-qml.git
UIQML_COMMIT=ed036ab40776889f716d781ec08e7ce15604f404		# 4.11.1+
export DEBIAN_FRONTEND=noninteractive

apt-get install -y -q --no-install-recommends git qmlscene qml-module-io-thp-pyotherside \
	qml-module-qtquick2 qml-module-qtquick-window2 qml-module-qtmultimedia \
	qml-module-qtgraphicaleffects libqt5multimedia5-plugins gstreamer1.0-plugins-good \
	gstreamer1.0-pulseaudio python3-mygpoclient fontconfig

fetch() {	# fetch <dir> <url> <commit>
	[ -d "$SRC/$1/.git" ] || git clone -q "$2" "$SRC/$1"
	git -C "$SRC/$1" fetch -q origin
	git -C "$SRC/$1" checkout -q -f "$3"
	git -C "$SRC/$1" submodule update -q --init
}
mkdir -p "$SRC"
fetch gpodder-sailfish "$SAILFISH" "$SAILFISH_COMMIT"
fetch gpodder-ui-qml "$UIQML" "$UIQML_COMMIT"

S=$SRC/gpodder-sailfish
rm -rf "$DEST"
mkdir -p "$DEST/lib"
cp -r "$SRC/gpodder-ui-qml/touch" "$DEST/touch"
rm -rf "$DEST/touch/common"
cp -r "$S/common" "$DEST/touch/common"
rm -f "$DEST/touch/common/main.py" "$DEST/touch/common/README.md"
cp "$S/common/main.py" "$DEST/main.py"
cp -r "$S/gpodder-core/src/gpodder" "$DEST/lib/"
cp "$S/minidb/minidb.py" "$S/podcastparser/podcastparser.py" "$DEST/lib/"

# The UI asks for its fonts by name ("Iconic" for the symbols).
install -d /usr/local/share/fonts/gpodder
install -m 644 "$DEST/touch/icons/iconic_fill.ttf" "$DEST/touch/fonts/"*.ttf /usr/local/share/fonts/gpodder/
fc-cache -f /usr/local/share/fonts/gpodder

install -m 755 "$HERE/gpodder-leste" /usr/local/bin/gpodder-leste
install -m 644 "$HERE/gpodder-leste.desktop" /usr/share/applications/hildon/gpodder-leste.desktop
for s in 86 108 128 256; do
	install -D -m 644 "$S/icons/${s}x${s}/apps/harbour-org.gpodder.sailfish.png" \
		"/usr/share/icons/hicolor/${s}x${s}/apps/gpodder-leste.png"
done
gtk-update-icon-cache -f -q /usr/share/icons/hicolor 2>/dev/null || true
echo ">> gPodder installed: gpodder-leste"
