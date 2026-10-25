# gPodder for Maemo Leste

The podcast client of Maemo 5, brought up to date: the current gPodder 4
core (gpodder-core 4.17.2 with podcastparser and minidb, as pinned by
gPodder for Sailfish OS 4.18.1, and its Python bridge) with the touch QML
UI of gpodder-ui-qml, on PyOtherSide and Qt 5.

Maemo 5's gPodder 2.x had a PyGTK/Hildon UI on Python 2; Leste (Debian 12)
has neither, and GTK 2/Hildon have no Python 3 bindings. gPodder 4 is the
maintained line, and its touch UI is made for phone screens.

    bash apps/gpodder/install.sh      # as root on Leste; scripts/49-apps.sh runs it

Installs to /opt/gpodder; the menu entry starts `gpodder-leste`. Settings
and downloads live in ~/.config/gpodder, ~/.local/share/gpodder and
~/.cache/gpodder.

Licenses: gpodder-core GPL-3.0/ISC, gpodder-ui-qml and gpodder-sailfish's
bridge ISC, podcastparser ISC, minidb ISC; all by Thomas Perl and the
gPodder contributors.
