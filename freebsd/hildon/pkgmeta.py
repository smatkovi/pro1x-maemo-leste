#!/usr/bin/env python3
"""FreeBSD package metadata for the parts build.sh installed into
$WORK/hildon/pkgroot/<part>: per package a directory with +MANIFEST (the
fields pkg-create(8) takes with -M), plist and root/ (the files), for
package.sh to turn into packages with FreeBSD's own pkg create.

    pkgmeta.py HILDON_DIR PACKAGESITE_YAML OUT_DIR

Name: the part's, unless FreeBSD has one of that name (then <name>-maemo,
conflicting with it); gtk becomes FreeBSD's gtk2 with Leste's libraries
over FreeBSD's package, at a higher version, since everything built here
needs Leste's GTK. Version, description and licence come from the
part's debian/ directory (or the Leste package's control file for the
data parts). Dependencies come from the ELF files' DT_NEEDED: a library
of another part, of a FreeBSD package (packagesite.yaml) or of the base
system (no dependency).
"""
import fnmatch
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile

H, SITE, OUT = sys.argv[1:4]
ROOTS = os.path.join(H, 'pkgroot')
SRC = os.path.join(H, 'src')
SYSROOT = os.path.join(H, 'sysroot')
CACHE = os.path.join(os.path.dirname(os.path.dirname(SITE)), 'cache')
RENAME = {'gtk': 'gtk2'}
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOCAL = {
    'pro1x-hidpi': ('1.0', "Hildon at the N900's size on high-resolution screens",
                    'pro1x-theme-scale scales a Hildon theme (images, gtkrc lengths, '
                    'matchbox decorations) by one factor; pro1x-hidpi makes the '
                    "theme for the configured factor and the session's environment "
                    '(tools/pro1x-hidpi).', ['UNKNOWN']),
    'pro1x-look': ('1.0', "The control panel's choice of Hildon's style",
                   "A control panel applet: Hildon in Maemo's style or in the Nokia "
                   "N9's (pages and edge swipes, patches/meego), or Xfce in its "
                   'place (apps/pro1x-look).', ['UNKNOWN']),
    'pro1x-camera': ('1.0', "The Pro1X's cameras for Hildon",
                     'pro1x-camera shows the viewfinder, focuses on a tap and takes '
                     'pictures into MyDocs/DCIM through /dev/camss0 of qcom_camss.ko '
                     '(freebsd/camera).', ['GPLv2']),
    'pro1x-steps': ('1.0', "The Pro1X's hardware step counter on D-Bus",
                    "pro1x-steps reads the ADSP's pedometer through libssc and "
                    'serves the count as org.pro1x.Steps on the system bus '
                    '(tools/pro1x-steps), with its rc.d script.', ['UNKNOWN']),
    # 1.1: -d, the modem from a QMUX file (freebsd/modem's tests)
    'pro1x-gnss': ('1.1', "The Pro1X modem's GNSS as NMEA for gpsd",
                   "pro1x-gnss starts the modem's location service over QMI and "
                   'writes its NMEA sentences for gpsd (tools/pro1x-gnss).',
                   ['UNKNOWN']),
    # 1.1: message paths unique across restarts (telepathy-ring's tokens),
    # sphone for calls only (the SMS are Conversations', as on Leste);
    # 1.2: no signal poll of the modem while nobody looks at the display,
    # the SMS parts' timer only while parts wait (wake-ups)
    'pro1x-modem': ('1.2', "The Pro1X modem's calls and SMS as oFono's D-Bus API",
                    "pro1x-modemd talks QMI over QRTR to the modem (DMS, UIM, NAS, WMS, "
                    "VOICE) and offers the calls, SMS, SIM and network registration as "
                    "oFono's D-Bus API, for sphone and the other apps written for oFono; "
                    "pro1x-callaudio sets up the voice path of a call on the ADSP "
                    '(freebsd/modem).', ['GPLv2']),
    'pro1x-proximity': ('1.0', "The Pro1X's proximity sensor on D-Bus, while watched",
                        "pro1x-proximity reads the ADSP's proximity sensor through "
                        'libssc while a client watches it and serves it as '
                        'org.pro1x.Proximity on the system bus (tools/pro1x-proximity), '
                        'with its rc.d script.', ['UNKNOWN']),
    'pro1x-light': ('1.0', "One reading of the Pro1X's ambient light sensor",
                    "pro1x-light switches the ADSP's ambient light sensor on through "
                    'libssc, prints one reading in lux and switches it off again '
                    '(tools/pro1x-light); the photo light meter asks it.', ['UNKNOWN']),
    'pro1x-standby': ('1.0', "The Nokia N9's stand-by screen as the X screen saver",
                      'pro1x-standby shows the time, unread notifications and the '
                      "charging state in the style of the N9's low power mode when "
                      'X blanks the screen, the rest black for the AMOLED panel '
                      "(tools/pro1x-standby); Nokia's icons and font are not "
                      'included.', ['UNKNOWN']),
    # 1.1: the weather (Open-Meteo) and the calendar's appointments
    # (calendar-backend), each section on or off; 1.2: the posts' pictures
    # as the N9's feeds showed theirs
    'pro1x-events': ('1.2', "The Nokia N9's Events page for Hildon's MeeGo-like look",
                     'pro1x-events shows on the first home view the date, the weather '
                     "of a place (Open-Meteo, MeeCast's icons where MeeCast is installed), "
                     'the missed calls, messages and chats (rtcom-eventlogger), unread '
                     "e-mail (modest), the next days' appointments of Hildon's calendar "
                     "(calendar-backend) and a Mastodon account's home timeline with its posts' "
                     'pictures, pulled '
                     'down to update, while the MeeGo-like look is on; its control panel '
                     'applet turns the sections on and off and takes the place and the '
                     'account (tools/pro1x-events, apps/pro1x-events).',
                     ['UNKNOWN']),
    'pro1x-volume-keys': ('1.0', "The Pro1X's volume keys on PulseAudio's default sink",
                          'pro1x-volume-keys takes the volume keys (XF86AudioRaiseVolume, '
                          'LowerVolume, Mute) in the Hildon session and steps the volume '
                          "of PulseAudio's default sink by 5 % up to 100 % "
                          '(freebsd/audio/tools/pro1x-volume-keys).',
                          ['BSD2CLAUSE']),
    'pro1x-volume-osd': ('1.0', "The Nokia N9's volume bar for Hildon's MeeGo-like look",
                         'pro1x-volume-osd shows the volume of PulseAudio\'s default sink '
                         "as the N9's system UI does when it changes, while Hildon's "
                         'MeeGo-like look is on; a tap or a drag sets it '
                         "(tools/pro1x-volume-osd). Nokia's graphics are not included.",
                         ['UNKNOWN']),
    'pro1x-alarmboot': ('1.0', 'After the RTC alarm switched the Pro1X on: rings, offers to switch off',
                        'When the boot menu started FreeBSD for the RTC alarm that FreeBSD '
                        'had set (kenv pro1x.alarmboot), rc.d/pro1x_alarmboot starts the '
                        'Hildon session; pro1x-alarmboot there rings by itself should no '
                        "alarm dialog of alarmd's come (its queue lost with a memory /var), "
                        'and once the alarm is stopped asks whether to switch the phone off '
                        'again, as the N900 asked whether to switch it on '
                        '(tools/pro1x-alarmboot, docs/alarm-power-on.md).',
                        ['GPLv2+']),
}
# The N9's applications of harmattan-qt5 (apps/; harmattan.sh), new ones
# in QML with com.nokia.meego: what their .desktop files say of them
MEEGO_APPS = ('browser', 'calculator', 'calendar', 'camera', 'contacts', 'gallery', 'mail', 'maps', 'messages', 'music', 'notes', 'phone', 'video')

# description, licences, home page
TARBALL = {
    'ffmpeg8-libs': ('8.1.2', "FFmpeg 8.1's libraries, for FreeBSD 15.1's QtWebEngine",
                     "libavcodec, libavformat, libavutil and libswresample of FFmpeg "
                     "8.1 (LGPL parts only), which the qt5-webengine package of FreeBSD "
                     "15.1 (release_1) links; beside FFmpeg 9 of FreeBSD's quarterly "
                     "packages, without headers.", ['LGPL21+'], 'https://ffmpeg.org/'),
    'qt5-webkit': ('5.212.0.a4', 'QtWebKit with a more modern WebKit code base',
                   "QtWebKit 5.212.0-alpha4, the last release of the Qt port of WebKit, "
                   "with the patches of FreeBSD's port and Arch's: WebKit 1 (QWebView, "
                   "QtWebKitWidgets) for the Maemo apps; without WebKit 2, video, "
                   "geolocation and XSLT.", ['LGPL21'], 'https://github.com/qtwebkit/qtwebkit'),
}
# Parts from git without a debian/ directory (the DSPs' userland from
# Linux): version (git describe), comment, description, licences, and the
# licence logic if more than one
UPSTREAM = {
    'hexagonrpc': ('0.5.0.24', "FastRPC file server for the Qualcomm DSPs",
                   "hexagonrpcd serves a Hexagon DSP the files it asks for over "
                   "FastRPC: the ADSP its sensor configuration and registry. With "
                   "the sensor registry interface the SM6115's ADSP needs.",
                   ['GPLv3+']),
    'libqrtr-glib': ('1.4.0', 'QRTR sockets for GLib',
                     "libqrtr-glib, the Qualcomm IPC router (QRTR) for GLib "
                     "programs: libqmi's transport to the DSPs' QMI services.",
                     ['LGPL21+']),
    'libqmi': ('1.34.0', 'QMI protocol library and qmicli',
               "libqmi-glib and qmicli as Leste builds them (1.34), over QRTR; "
               "without udev, RMNET links, QMI over MBIM and the firmware updater.",
               ['LGPL21+', 'GPLv2+'], 'multi'),
    'libssc': ('0.4.4', 'Qualcomm Sensor Core client library',
               "libssc reads the sensors of a Qualcomm DSP (accelerometer, light, "
               "proximity, the step counter) over QMI, with Leste's two patches.",
               ['GPLv3+']),
    'qrtr': ('1.2.18', 'Qualcomm IPC router library and lookup tool',
             "libqrtr, QRTR sockets and QMI encoding for C programs, and "
             "qrtr-lookup, which lists the services of the name service.",
             ['BSD3CLAUSE']),
    'rmtfs': ('1.3', "Qualcomm modem's remote file system service",
              "rmtfs gives the modem its file systems (modemst1, modemst2, fsg, "
              "fsc) from the GPT partitions through the shared memory of "
              "qcom_rmtfs, read-only as on Leste.", ['BSD3CLAUSE']),
    'tqftpserv': ('1.3.2', 'TFTP over QRTR for the Qualcomm DSPs',
                  "tqftpserv serves the DSPs files over QRTR's TFTP: the modem "
                  "its configuration and the Wi-Fi's firmware (wlanmdsp.mbn).",
                  ['BSD3CLAUSE']),
    'qt-components': ('1.4.9', "MeeGo Harmattan's Qt Quick components for Qt 5",
                      "com.nokia.meego, the Qt Quick components of the Nokia N9's "
                      "MeeGo Harmattan, in Nemo Mobile's port to Qt 5 (import "
                      "com.nokia.meego 2.0), for MeeGo apps, with the fixes Qt 5.15 "
                      "needs; their look comes from a meegotouch theme in "
                      "/usr/local/share/themes (the N9's blanco from one's own "
                      "firmware), which is not included.", ['BSD3CLAUSE']),
    'meego-clock': ('0.1', "The Nokia N9's clock for Hildon",
                    "meego-clock: the analog clock and the alarms, and the alarm "
                    "editor with the N9's two-circle time picker, in QML with "
                    "com.nokia.meego, on alarmd; it draws with the N9's blanco "
                    "theme, which is not included.", ['BSD3CLAUSE']),
    'pd-mapper': ('1.1', 'Qualcomm protection domain mapper',
                  "pd-mapper tells the DSPs which of them serves which protection "
                  "domain, from the firmware's .jsn files.", ['BSD3CLAUSE']),
    'maplibre-native-qt': ('3.0.0', 'MapLibre Native for Qt, the map renderer of Pure Maps',
                           "MapLibre Native Qt draws vector tile maps with OpenGL (ES) "
                           "for Qt programs: Pure Maps' renderer, without its widgets "
                           "and QtLocation plugin.", ['BSD2CLAUSE']),
    'nemo-qml-plugin-dbus': ('2.1.27', 'Nemo.DBus: D-Bus for QML',
                             "Nemo.DBus, Sailfish OS's D-Bus interfaces and adaptors for "
                             "QML (and its libnemodbus), which Pure Maps' pages use.",
                             ['LGPL21']),
    'mapbox-gl-qml': ('3.0.0.79', "Pure Maps' QML map item on MapLibre",
                      "mapbox-gl-qml, the MapboxMap QML item on MapLibre Native Qt, "
                      "with its gestures.", ['LGPL3']),
    'pure-maps': ('3.5.1', 'Maps and navigation on OpenStreetMap data',
                  "Pure Maps, the Kirigami version: vector maps, search, routing and "
                  "turn-by-turn navigation with voice, online or with OSM Scout "
                  "Server; menu entries for MeeGo's home and Hildon, with Kirigami's "
                  "touch layout (pure-maps-mobile).", ['GPLv3+']),
    'osmscout-server': ('3.1.7', 'Offline maps, search and routing for Pure Maps',
                        "OSM Scout Server, the Kirigami version: downloads OpenStreetMap "
                        "data per country and serves Pure Maps vector tiles and search "
                        "(geocoder-nlp on libpostal) and offline routing (Valhalla 3.4.0, "
                        "static, on protobuf-lite 3.21) on 127.0.0.1:8553, started through "
                        "D-Bus; without Mapnik raster tiles.",
                        ['GPLv3+']),
}
for _a in MEEGO_APPS:
    try:
        with open(os.path.join(SRC, 'harmattan-meego-' + _a, 'data', 'meego-%s.desktop' % _a)) as _f:
            _c = re.search(r'^Comment=(.*)$', _f.read(), re.M).group(1)
    except (OSError, AttributeError):
        _c = "The Nokia N9's %s" % _a
    UPSTREAM['harmattan-meego-' + _a] = (
        '0.1', _c,
        '%s: in QML with com.nokia.meego, for Hildon and the N9\'s user interface '
        '(harmattan-qt5 apps/meego-%s); it draws with the N9\'s blanco theme, '
        'which is not included.' % (_c.replace(', as on the Nokia N9', ''), _a),
        ['BSD3CLAUSE'])

MAINTAINER = 'sebastian.matkovich@gmail.com'
# What a part needs besides the libraries it links: interpreters, the
# modules it loads, the session around it
RUNTIME = {
    'hildon-desktop': ['hildon-home', 'hildon-status-menu', 'sapwood',
                       'hildon-theme-alpha', 'osso-icons-default', 'osso-sounds-ui'],
    'gpodder-leste': ['python312', 'pyotherside-qt5', 'qt5-declarative',
                      'qt5-quickcontrols', 'qt5-quickcontrols2', 'qt5-graphicaleffects'],
    'pro1x-hidpi': ['xrdb'],
    'pro1x-look': ['hildon-control-panel', 'hildon-desktop'],
    'wifi-assistant': ['curl'],
    'pro1x-steps': ['hexagonrpc', 'dbus'],
    'pro1x-proximity': ['hexagonrpc', 'dbus'],
    'pro1x-light': ['hexagonrpc'],
    # the light meter asks the Pro1X's light sensor (pro1x-light)
    'photolightmeter': ['pro1x-light'],
    'pro1x-gnss': ['gpsd'],
    # as Leste's liblocation0: the daemon, the dialogs and the settings
    'liblocation': ['location-daemon', 'location-ui', 'location-control'],
    # the modem started by pro1x_modem (rmtfs, tqftpserv, pd-mapper), the bus;
    # rc.d/pro1x_gps runs gpsd on pro1x-gnss
    'pro1x-modem': ['rmtfs', 'dbus', 'pro1x-gnss', 'gpsd'],
    # the page is a home widget of hildon-desktop's MeeGo-like look; the
    # certificates for Mastodon's https
    'pro1x-events': ['hildon-desktop', 'hildon-control-panel', 'rtcom-eventlogger-plugins',
                     'ca_root_nss'],
    # the volume it shows is PulseAudio's, which hildon-session starts
    'pro1x-volume-osd': ['pulseaudio', 'hildon-desktop'],
    # its tone through PulseAudio, alarmd asked for the alarm, the session
    # whose alarm dialog it watches
    'pro1x-alarmboot': ['pulseaudio', 'alarmd', 'osso-systemui-alarm', 'hildon-desktop'],
    # pactl is PulseAudio's
    'pro1x-volume-keys': ['pulseaudio'],
    # the modem asks for its files and domains before its file systems
    'rmtfs': ['tqftpserv', 'pd-mapper'],
    # QtSql's SQLite driver, a package of its own on FreeBSD
    'jib': ['qt5-sqldrivers-sqlite3'],
    'dorian': ['qt5-sqldrivers-sqlite3'],
    # the chess engine the game runs
    'osso-chess-ui': ['gnuchess', 'osso-sounds-game-chess'],
    # the click sounds, through libcanberra's PulseAudio driver (FreeBSD's
    # libcanberra-pulse)
    'maemo-input-sounds': ['libcanberra', 'libcanberra-pulse', 'pulseaudio', 'osso-sounds-ui'],
    # alarmd asks clockd for the time; the alarm dialog is a system UI
    # plugin, it rings through the notification plugin
    'alarmd': ['clockd'],
    'osso-systemui-alarm': ['osso-systemui', 'alarmd', 'osso-sounds-ui'],
    # the decoder service makes WAV files of the AAC and MP3 tones (alarms,
    # ringtones) with GStreamer: decodebin, qtdemux and wavenc (good), faad,
    # Ogg Vorbis, mpg123
    'hildon-plugins-notify-sv': ['gstreamer1-plugins-good', 'gstreamer1-plugins-faad',
                                 'gstreamer1-plugins-ogg', 'gstreamer1-plugins-vorbis',
                                 'gstreamer1-plugins-mpg123'],
    'statusbar-alarm': ['alarmd'],
    # Leste's Qt 6 apps ask for its platform plugin (QT_QPA_PLATFORM=maemo)
    # and style themselves; Maemo Translate comes with the German models,
    # as on Leste
    'maemo-translate': ['qt6-qpa-maemo', 'qt6-style-maemo', 'maemo-translate-data-de'],
    # the SMS through telepathy-ring (its Mission Control plugin gives the
    # cellular account, as on Leste), the accounts Mission Control's
    'conversations': ['qt6-qpa-maemo', 'qt6-style-maemo', 'telepathy-ring',
                      'telepathy-mission-control'],
    # its account plugin is Mission Control's (which starts it)
    'telepathy-ring': ['telepathy-mission-control'],
    'nomweather': ['qt6-qpa-maemo', 'qt6-style-maemo'],
    # QML on the MeeGo components; its alarms are alarmd's
    'meego-clock': ['qt-components', 'alarmd', 'clockd'],
    # the N9's applications: QML on the MeeGo components; SQLite (calendar,
    # messages), GStreamer's decoders and encoders (music, video, camera),
    # a web engine (browser), positions (maps). Phone and messages need
    # ModemManager, which FreeBSD does not have ("No modem").
    **{'harmattan-meego-' + a: ['qt-components'] for a in MEEGO_APPS},
    'harmattan-meego-calendar': ['qt-components', 'qt5-sqldrivers-sqlite3'],
    'harmattan-meego-messages': ['qt-components', 'qt5-sqldrivers-sqlite3'],
    'harmattan-meego-music': ['qt-components', 'gstreamer1-plugins-good'],
    'harmattan-meego-video': ['qt-components', 'gstreamer1-plugins-good'],
    'harmattan-meego-camera': ['qt-components', 'gstreamer1-plugins-good',
                               'gstreamer1-plugins-jpeg'],
    'harmattan-meego-browser': ['qt-components', 'qt5-webengine'],
    'harmattan-meego-maps': ['qt-components', 'qt5-location'],
    # QML on QtQuick Controls 2, its stations in SQLite
    'meecast': ['qt5-quickcontrols2', 'qt5-graphicaleffects', 'qt5-sqldrivers-sqlite3'],
    # its tile cache is an SQLite database (QtSql)
    'maplibre-native-qt': ['qt5-sqldrivers-sqlite3'],
    # Python through PyOtherSide; the QML modules its pages import; the
    # voice (eSpeak NG)
    'pure-maps': ['python312', 'pyotherside-qt5', 'kf5-kirigami2', 'mapbox-gl-qml',
                  'nemo-qml-plugin-dbus', 'qt5-quickcontrols2', 'qt5-quickcontrols',
                  'qt5-graphicaleffects', 'qt5-location', 'qt5-multimedia', 'qt5-sensors',
                  'espeak-ng'],
    'osmscout-server': ['kf5-kirigami2', 'qt5-quickcontrols2', 'qt5-quickcontrols',
                        'qt5-location', 'qt5-sqldrivers-sqlite3'],
}
# Where debian/copyright says only "GPL" (any version) or nothing
LICENSES = {
    'hildon-im-vkbrenderer3': ['GPLv2+'],
    'hildon-input-method-plugins': ['GPLv2+'],
    'osso-mahjong': ['GPLv2+'],
    'osso-chess-ui': ['GPLv2+'],
    'osso-lmarbles': ['GPLv2+'],
    'hildon-games-wrapper': ['LGPL21'],
    'statusbar-alarm-dbus-api': ['BSD3CLAUSE'],
    'osso-systemui-dbus-dev': ['LGPL21'],
    'libplayback': ['LGPL21'],
    'osso-systemui-alarm': ['LGPL21'],
    # Maemo Translate and its libraries (their LICENSE files): Mozilla's
    # Bergamot models (firefox-translations-models) and kotki (Bergamot's
    # translator) MPL 2.0, Marian MIT, ruy and sentencepiece Apache 2.0
    'maemo-translate-data-de': ['MPL20'],
    'maemo-translate': ['MPL20'],
    'kotki': ['MPL20'],
    'marian-lite': ['MIT'],
    'ruy': ['APACHE20'],
    'sentencepiece-browsermt': ['APACHE20'],
    # Harmattan's (harmattan-qt5): LGPL 2.1 in LICENSE.LGPL or the sources'
    # headers, not in debian/copyright; statusmenu-plugins as the rest
    **{'harmattan-' + p: ['LGPL21'] for p in (
        'duicontrolpanel', 'libcontentaction', 'libmeegotouch', 'libmlocale',
        'mcompositor', 'meegotouch-controlpanelapplets', 'meegotouchhome',
        'meegotouchtheme', 'qmsystem2', 'statusmenu-plugins', 'system-ui')},
}
# Where debian/control describes a part badly
DESCRIBE = {
    'libdsme': ("dsme's D-Bus names (libdsme_dbus_if)",
                "libdsme_dbus_if, the names of dsme's D-Bus interface, which alarmd "
                "uses; FreeBSD runs no dsme, so libdsme's socket protocol is left out."),
    'hildon-games-wrapper': ('Wrapper that starts and pauses full screen games',
                             "hildon-games-wrapper (libhgw and its wrapper) runs a "
                             "game such as Marbles from osso-games-startup and passes "
                             "it D-Bus and GConf messages: pause, quit, its settings."),
}
# Files FreeBSD's packages own already (and what the part then needs)
EXCLUDE = {
    'osso-icons-default': ([], ['hicolor-icon-theme']),
    # FreeBSD has gtk-update-icon-cache in a package of its own; the API
    # documentation stays out; FreeBSD's libraries (2.24.33) under Leste's
    # (2.24.25 with the Hildon changes), which the links now point to
    # libhildon's English texts lack the 12-hour times and am/pm the
    # clock and the alarm dialog show: Leste's (leste-l10n-en) instead
    'libhildon': (['usr/local/share/locale/en_GB/LC_MESSAGES/hildon-libs.mo'], ['leste-l10n-en']),
    # Modest's English texts are Leste's too (leste-l10n-en); the client
    # library installs Modest's NEWS as well
    'modest': (['usr/local/share/locale/en_GB/LC_MESSAGES/modest.mo'], ['leste-l10n-en']),
    'libmodest-dbus-client': (['usr/local/share/modest/data/NEWS'], []),
    # Harmattan's libdsme (libdsme.so.0.2.0, which qmsystem2 links) beside
    # Leste's (libdsme_dbus_if.so.0, alarmd's): without the development
    # files both have, so that both install
    'harmattan-libdsme': (['usr/local/include/dsme/dsme_dbus_if.h', 'usr/local/lib/libdsme_dbus_if.so',
                           'usr/local/libdata/pkgconfig/dsme_dbus_if.pc'], []),
    'gtk': (['usr/local/bin/gtk-update-icon-cache', 'usr/local/share/man/man1/gtk-update-icon-cache.1*',
             'usr/local/share/gtk-doc/*', 'usr/local/lib/libgtk-x11-2.0.so.0.2400.33',
             'usr/local/lib/libgdk-x11-2.0.so.0.2400.33'], ['gtk-update-icon-cache']),
    # Harmattan's test suites (some need stub libraries of their build) and
    # the N9's restore-factory-settings script stay out
    'harmattan-contextkit': (['usr/local/lib/libcontext*-tests', 'usr/local/share/libcontext*-tests'], []),
    'harmattan-duicontrolpanel': (['share'], []),
    'harmattan-libcontentaction': (['usr/local/share/libcontentaction-tests'], []),
    'harmattan-libmeegoimengine': (['usr/local/lib/libmeegoimengine-tests',
                                    'usr/local/share/libmeegoimengine-tests'], []),
    'harmattan-meego-im-framework': (['usr/local/lib/meego-im-framework-tests'], []),
    'harmattan-meego-im-settings-applet': (['usr/local/lib/meego-im-settings-applet-tests',
                                            'usr/local/share/meego-im-settings-applet-tests'], []),
    'harmattan-meegotouch-controlpanelapplets': (['mcpapplets-rfs.sh'], []),
    # as Leste's package (debian/install): the connection manager, its
    # D-Bus service, manager file and Mission Control plugin; not the test
    # programs, the modem library's static archive and headers
    'telepathy-ring': (['opt', 'usr/local/lib/libmodem-glib.a', 'usr/local/include/modem-glib',
                        'usr/local/share/man/man8/telepathy-ring.8'], []),
}
# Packages of this repository without a part: the Hildon session for any
# FreeBSD (freebsd/hildon/rootfs) and one to install everything
SESSION_DEPS = ['hildon-desktop', 'pro1x-hidpi', 'gconf2', 'dbus', 'xrdb', 'xset']

site = {}
provider = {}
for line in open(SITE):
    d = json.loads(line)
    site[d['name']] = d
    for so in d.get('shlibs_provided', []):
        provider.setdefault(so, []).append(d['name'])
# What the sysroot was made of: the preferred provider of a library
in_sysroot = set()
wanted = os.path.join(H, 'sysroot.list')
if os.path.exists(wanted):
    for line in open(wanted):
        m = re.match(r'(.*)-[^-]+~\d+\$\w+\.pkg$', os.path.basename(line.strip()))
        if m:
            in_sysroot.add(m.group(1))
base_libs = set()
for d in ('lib', 'usr/lib'):
    p = os.path.join(SYSROOT, d)
    if os.path.isdir(p):
        base_libs.update(f for f in os.listdir(p) if '.so' in f)


def readelf(path):
    try:
        out = subprocess.run(['llvm-readelf', '-d', path], capture_output=True,
                             text=True).stdout
    except OSError:
        return None, []
    soname = re.findall(r'\(SONAME\)\s+Library soname: \[(.*)\]', out)
    needed = re.findall(r'\(NEEDED\)\s+Shared library: \[(.*)\]', out)
    return (soname[0] if soname else None), needed


def is_elf(path):
    try:
        with open(path, 'rb') as f:
            return f.read(4) == b'\x7fELF'
    except OSError:
        return False


# FreeBSD-side changes to a part at the same upstream version (patches,
# build.sh), like a port's PORTREVISION: version_N, so that pkg upgrade
# takes the new package. Raise a part's number with each such change.
REVISION = {
    # 2026-10-27: clock-ui ran no more than its start (time zone, paths,
    # missing alarmd), apps' data moved from /usr/share to /usr/local/share
    'cityinfo': 1, 'clockd': 1, 'clock-ui': 1,
    'easylist': 1, 'flappybird': 1, 'hamsterfiler': 1, 'maepad': 1,
    'mihphoto': 1, 'osso-mahjong': 1, 'qshot': 1,
    # 2026-10-27: the launcher's menu below /usr/local (it was empty), the
    # MeeGo-like pages (patches/meego, off by default); 2: the edge
    # gestures, no more crash when a thumbnail zooms in (isnanf, 0003);
    # 4: no dependency on Nokia's sounds (PRIVATE); 5: the MeeGo-like look's
    # status bar over the launcher and the task navigator, the Events page;
    # 6: portrait as on the N9 (pages side by side, the N9's launcher rows,
    # the open applications in two columns)
    'hildon-desktop': 6,
    # 2026-10-27: the daemon too (it was libalarm only); 2: the wake-up
    # alarm through qcom_rtc's dev.qcom_rtc.0.wakealarm, the next wake-up
    # in /var/run/alarmd-wakeup for pro1x-suspend (0002, 0006)
    'alarmd': 2,
    # 2026-10-27: Leste's hildon-libs texts, not libhildon's (EXCLUDE); 2:
    # the MeeGo-like look's list menu and wheel pickers (patches/meego)
    'libhildon': 2, 'leste-l10n-en': 1,
    # 2026-10-29: the MeeGo-like look's thin status bar (patches/meego)
    'hildon-status-menu': 1,
    # 2026-10-30: repeating appointments on 64-bit (0003, also Leste's
    # patches/fixes), SQLite's double-quoted strings allowed again (0004):
    # no repeating appointment was expanded, the default calendars missing
    'calendar-backend': 1,
    # 2026-10-27: Qt 5.15 fixes (fonts, theme images, window size, styles,
    # dialogs and banners that took taps)
    'qt-components': 1,
    # 2026-10-27: the profiles from /usr/local/etc/profiled (profiled found
    # none and answered every value empty), profilesx's custom profiles and
    # helper there too (2)
    'profiled': 1, 'profilesx': 2,
    # 2026-10-27: sounds: no Linux-only time stamps (it aborted), the decoded
    # tones in the user's directory, the decoder's caps as GTypes (it crashed)
    'hildon-plugins-notify-sv': 1,
    # 2026-10-28: offline routing (Valhalla), no Mapnik data downloads
    'osmscout-server': 1,
    # 2026-10-29: Qt 6's plugins below /usr/local/lib/qt6 (qmake6 -query
    # answered with the PC's /usr/lib/qt6), sphone's modules below
    # /usr/local/lib, system-ui's D-Bus policy below /usr/local/etc and
    # its dead notification sinks dropped (harmattan-qt5 278a4b8); sphone
    # 3 (2026-10-30): DTMF through oFono's SendTones (0001), the Speaker
    # button to pro1x-callaudio (route-pro1x, 0002)
    'qt6-qpa-maemo': 1, 'qt6-style-maemo': 1, 'sphone': 3, 'harmattan-system-ui': 1,
    # 2026-10-29: libmeegotouch's style sheet cache read every attribute 4
    # bytes early in clang builds (nothing drew, harmattan-qt5 0bbefb4),
    # the Pro1X's 960x480 target; the pro1x theme's black wallpaper
    'harmattan-libmeegotouch': 1, 'harmattan-meegotouchtheme': 1,
    # 2026-10-29: files that other packages have too, left out (EXCLUDE)
    'modest': 1, 'libmodest-dbus-client': 1, 'harmattan-libdsme': 1,
    # 2026-10-29: Yappari's program itself (its qmake had failed unseen:
    # libqrencode), sphone's configuration below /usr/local/share
    'yappari-leste': 2,
    # 2026-10-29: the light meter reads the Pro1X's light sensor (pro1x-light)
    'photolightmeter': 1,
    # 2026-10-29: its headers below include/harmattan-mce, beside Leste's
    'harmattan-mce-dev': 1,
    # 2026-10-29: the gettext texts below /usr/local/share/locale (the
    # sources named /usr/share/locale: the address book showed text IDs)
    'hildon-home': 1, 'hildon-control-panel-personalisation': 1, 'osso-calculator': 1, 'osso-abook': 1, 'osso-addressbook': 1,
    # 2026-10-29: no dependency on Nokia's sounds (PRIVATE)
    'osso-chess-ui': 1, 'maemo-input-sounds': 1, 'osso-systemui-alarm': 1,
    # 2026-10-30: events were never logged on FreeBSD (0001: SQLite's printf
    # has no j modifier, FreeBSD's PRIiMAX)
    'rtcom-eventlogger': 1,
    # 2026-10-29: its texts below /usr/local/share/locale (as above); 2
    # (2026-10-30): SMS through telepathy-ring and Mission Control
    # (RUNTIME), started in the background by hildon-session; sent SMS no
    # longer pending for ever (0001); the launcher's child ends with it
    # (0002)
    'conversations': 2,
    # 2026-10-30: the lock in /var/run (0003), location-ui's dialogs asked
    # when it runs, without MCE (0004), the dialogs' answers (0005, 0006)
    # 2026-10-30: the Uncertainty match rule's quote (0007); creating a
    # control no longer starts positioning (0008); 3 (2026-10-30): start
    # after stop (0009), the lock held only while started (0010)
    'liblocation': 3,
    # 2026-10-30: asks for the headphones (their cable is the antenna) when
    # pro1x-audiod reports none plugged in
    'pro1x-fm': 1,
}

# Maemo 5's sounds as Leste's archive has them: Nokia's (debian/copyright:
# all rights reserved) and the ringtones, which name no licence. They are
# not made into packages for the public releases, and nothing depends on
# them (the click sounds, alarms and chess are silent without them); the
# phone gets them from the private assets repository (maemo5-sounds/), and
# the root image from the stage as before. PRO1X_PRIVATE=1 makes their
# packages too, for that repository.
PRIVATE = {'osso-sounds-ui', 'osso-sounds-rtc', 'osso-sounds-game-chess', 'maemo-ringtones-mr0'}
WITH_PRIVATE = os.environ.get('PRO1X_PRIVATE') == '1'


def version_of(text):
    """Debian version -> FreeBSD's: no epoch (nothing to be newer than),
    no - + ~ : (package file names stay plain, for download links)"""
    if ':' in text:
        text = text.split(':', 1)[1]
    v = re.sub(r'[^0-9A-Za-z.]', '.', text)
    return re.sub(r'\.+', '.', v).strip('.') or '0'


def stanzas(text):
    out = []
    for block in re.split(r'\n\s*\n', text):
        d, key = {}, None
        for line in block.split('\n'):
            m = re.match(r'([A-Za-z-]+):\s?(.*)', line)
            if m and not line.startswith(' '):
                key = m.group(1)
                d[key] = m.group(2)
            elif key and line.startswith(' '):
                d[key] += '\n' + line[1:]
        if d:
            out.append(d)
    return out


def describe(control, part):
    """comment and desc of the part's main binary package"""
    st = stanzas(control)
    bins = [s for s in st if 'Package' in s]
    main = None
    for s in bins:
        if s['Package'] == part:
            main = s
    if main is None:
        for s in bins:
            if not re.search(r'-(dev|dbg|doc|dbgsym|l10n.*)$', s['Package']):
                main = s
                break
    if main is None and bins:
        main = bins[0]
    desc = (main or {}).get('Description', part)
    first, _, rest = desc.partition('\n')
    rest = '\n'.join('' if l.strip() == '.' else l.strip()
                     for l in rest.split('\n')).strip()
    return first.strip() or part, rest or first.strip() or part


SPDX = {
    'GPL-2': 'GPLv2', 'GPL-2+': 'GPLv2+', 'GPL-2.0': 'GPLv2', 'GPL-2.0+': 'GPLv2+',
    'GPL-2.0-only': 'GPLv2', 'GPL-2.0-or-later': 'GPLv2+',
    'GPL-3': 'GPLv3', 'GPL-3+': 'GPLv3+', 'GPL-3.0': 'GPLv3', 'GPL-3.0+': 'GPLv3+',
    'GPL-3.0-only': 'GPLv3', 'GPL-3.0-or-later': 'GPLv3+',
    'LGPL-2': 'LGPL20', 'LGPL-2+': 'LGPL20+', 'LGPL-2.1': 'LGPL21', 'LGPL-2.1+': 'LGPL21+',
    'LGPL-2.1-only': 'LGPL21', 'LGPL-2.1-or-later': 'LGPL21+',
    'LGPL-3': 'LGPL3', 'LGPL-3+': 'LGPL3+', 'MIT': 'MIT', 'Expat': 'MIT',
    'BSD-2-clause': 'BSD2CLAUSE', 'BSD-3-clause': 'BSD3CLAUSE', 'BSD-2-Clause': 'BSD2CLAUSE',
    'BSD-3-Clause': 'BSD3CLAUSE', 'CC-BY-SA-3.0': 'CC-BY-SA-3.0',
}


def statement(t):
    """The licence a free-text notice grants: 'terms of the GNU ...'"""
    t = re.sub(r'\s+', ' ', t)
    m = re.search(r'(?:terms|conditions) of (?:the )?GNU (Lesser |Library )?General Public License', t, re.I)
    if m:
        tail = t[m.end():m.end() + 300]
        later = re.search(r'any later version', tail, re.I)
        v = re.search(r'version (\d(?:\.\d)?)', tail, re.I)
        v = v.group(1) if v else ('2.1' if m.group(1) and m.group(1).startswith('Lesser') else
                                  '2' if m.group(1) else '2')
        if m.group(1):
            name = {'2': 'LGPL20', '2.0': 'LGPL20', '2.1': 'LGPL21', '3': 'LGPL3'}.get(v, 'LGPL21')
        else:
            name = {'2': 'GPLv2', '3': 'GPLv3'}.get(v[0], 'GPLv2')
        return [name + ('+' if later else '')]
    if re.search(r'Permission is hereby granted, free of charge', t):
        return ['MIT']
    if re.search(r'Redistribution and use in source and binary forms', t):
        return ['BSD3CLAUSE' if re.search(r'neither the name', t, re.I) else 'BSD2CLAUSE']
    return []


def header(t):
    """The licence a full licence text (COPYING) is"""
    h = re.sub(r'\s+', ' ', t[:400])
    if re.search(r'LESSER GENERAL PUBLIC LICENSE Version 2\.1', h, re.I):
        return ['LGPL21']
    if re.search(r'LIBRARY GENERAL PUBLIC LICENSE Version 2', h, re.I):
        return ['LGPL20']
    if re.search(r'LESSER GENERAL PUBLIC LICENSE Version 3', h, re.I):
        return ['LGPL3']
    if re.search(r'GENERAL PUBLIC LICENSE Version 3', h, re.I):
        return ['GPLv3']
    if re.search(r'GENERAL PUBLIC LICENSE Version 2', h, re.I):
        return ['GPLv2']
    return statement(t)


def licences(copyright, copying):
    """debian/copyright's machine-readable Files: * licence, else its
    notice, else the licence text the source carries"""
    for st in stanzas(copyright):
        if st.get('Files', '').strip() == '*' and 'License' in st and \
                not st['License'].startswith('<'):    # dh_make's template
            first = st['License'].split('\n')[0].strip()
            names = [SPDX.get(x.strip(), x.strip()) for x in re.split(r'\s+or\s+|\s+and\s+', first) if x.strip()]
            if names:
                return names
    return statement(copyright) or next((header(c) for c in copying if header(c)), []) or ['UNKNOWN']


def read(path):
    try:
        return open(path, errors='replace').read()
    except OSError:
        return ''


# --- the parts -------------------------------------------------------------
parts = sorted(p for p in os.listdir(ROOTS)
               if os.path.isdir(os.path.join(ROOTS, p)))
meta = {}
for part in parts:
    if part in PRIVATE and not WITH_PRIVATE:
        continue
    root = os.path.join(ROOTS, part)
    meta_extra = {}
    name = RENAME.get(part, part)
    conflicts = []
    if name in site and part not in RENAME:
        conflicts = [name + '-*']
        name = name + '-maemo'
    src = os.path.join(SRC, part)
    url = read(root + '.url').strip()
    rev = read(root + '.rev').strip()
    if os.path.isdir(os.path.join(src, 'debian')):
        control = read(os.path.join(src, 'debian/control'))
        cl = read(os.path.join(src, 'debian/changelog'))
        m = re.match(r'\S+ \(([^)]+)\)', cl)
        version = version_of(m.group(1) if m else '0')
        comment, desc = DESCRIBE.get(part, describe(control, part))
        lic = licences(read(os.path.join(src, 'debian/copyright')),
                       [read(os.path.join(src, f)) for f in
                        ('COPYING', 'LICENSE', 'COPYING.LIB', 'LICENSE.txt')])
        www = re.sub(r'\.git$', '', url) or 'https://github.com/maemo-leste'
    elif part.startswith('leste-l10n-'):
        # Leste's *-l10n-<language> packages of one index, together
        lang = {'en': 'English', 'de': 'German'}.get(part.rsplit('-', 1)[1], part)
        rel = read(os.path.join(H, 'leste', 'Release'))
        m_ = re.search(r'^Date: \w+, (\d+) (\w+) (\d+)', rel, re.M)
        months = 'Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec'.split()
        version = ('%s.%02d.%02d' % (m_.group(3), months.index(m_.group(2)) + 1,
                                     int(m_.group(1)))) if m_ else '0'
        n = len([l for l in read(root + '.rev').splitlines() if l.strip()])
        comment = "Maemo Leste's user interface texts (%s)" % lang
        desc = ('The %s translations of the Hildon desktop and its apps: %d of '
                "Leste's *-l10n packages, as in its archive on %s." % (lang, n, version))
        lic = ['UNKNOWN']
        www = 'https://maedevu.maemo.org/leste'
        url = www
    elif os.path.exists(root + '.manifest'):
        # FreeBSD's own package of another package set (build.sh: "pkg"),
        # as FreeBSD describes it
        fm = json.loads(read(root + '.manifest'))
        version, comment, desc = fm['version'], fm['comment'], fm['desc']
        lic, www = fm.get('licenses') or ['UNKNOWN'], fm.get('www', url)
        meta_extra = {'origin': fm['origin'], 'logic': fm.get('licenselogic', 'single'),
                      'scripts': fm.get('scripts')}
    elif part in TARBALL:
        version, comment, desc, lic, www = TARBALL[part]
    elif part in UPSTREAM:
        version, comment, desc, lic = UPSTREAM[part][:4]
        if len(UPSTREAM[part]) > 4:
            meta_extra = {'logic': UPSTREAM[part][4]}
        www = re.sub(r'\.git$', '', url)
    elif part in LOCAL:
        # this repository's own (tools/)
        version, comment, desc, lic = LOCAL[part]
        www = 'https://github.com/smatkovi/pro1x-maemo-leste'
        url = www
        rev = subprocess.run(['git', '-C', REPO, 'rev-parse', 'HEAD'],
                             capture_output=True, text=True).stdout.strip()
    else:
        # a data package from Leste's archive (build.sh leste_data)
        ctl = stanzas(read(os.path.join(H, 'leste', part + '.control')))
        c = ctl[0] if ctl else {}
        version = version_of(c.get('Version', '0'))
        comment, desc = describe(read(os.path.join(H, 'leste', part + '.control')), part)
        deb = rev.split()[1] if rev.startswith('deb ') else ''
        cr = ''
        if deb:
            try:
                out = subprocess.run(
                    'bsdtar -xOf %s "data.tar.*" | bsdtar -xOf - "./usr/share/doc/%s/copyright"'
                    % (os.path.join(H, 'leste', deb), part), shell=True,
                    capture_output=True, text=True).stdout
                cr = out
            except OSError:
                pass
        lic = licences(cr, [])
        www = 'https://maedevu.maemo.org/leste'
    lic = LICENSES.get(part, lic)
    if part in REVISION:
        version += '_%d' % REVISION[part]
    meta[part] = dict(name=name, version=version, comment=comment, desc=desc,
                      licenses=lic, www=www, url=url, rev=rev, root=root,
                      conflicts=conflicts)
    meta[part].update(meta_extra)

# Leste's GTK as FreeBSD's gtk2, one step above FreeBSD's version
if 'gtk' in meta:
    g = site['gtk2']
    m = meta['gtk']
    m['version'] = g['version'].replace('_', '.1_', 1) if '_' in g['version'] \
        else g['version'] + '.1'
    m['origin'] = g['origin']
    m['comment'] = g['comment'] + " (Maemo Leste's Hildon GTK)"
    m['licenses'] = ['LGPL20']

# Libraries each part provides
ours = {}
for part, m in meta.items():
    for dp, _, fs in os.walk(m['root']):
        for f in fs:
            p = os.path.join(dp, f)
            if '.so' in f and not os.path.islink(p) and is_elf(p):
                so, _ = readelf(p)
                ours.setdefault(so or f, part)
            if '.so' in f:
                ours.setdefault(f, part)

if os.path.isdir(OUT):
    shutil.rmtree(OUT)
os.makedirs(OUT)
unresolved = {}
for part, m in meta.items():
    d = os.path.join(OUT, m['name'])
    r = os.path.join(d, 'root')
    os.makedirs(d)
    deps = {}
    extra_files = []
    if part == 'gtk':
        # FreeBSD's gtk2 under Leste's libraries
        g = site['gtk2']
        pk = [f for f in os.listdir(CACHE) if f.startswith('gtk2-%s~' % g['version'])]
        os.makedirs(r)
        with tarfile.open(os.path.join(CACHE, pk[0])) as t:
            for ti in t.getmembers():
                if ti.name.startswith('+'):
                    continue
                ti.name = ti.name.lstrip('/')
                t.extract(ti, r, filter='tar')
        for k, v in g.get('deps', {}).items():
            deps[k] = {'origin': v['origin'], 'version': v['version']}
    os.makedirs(r, exist_ok=True)
    subprocess.run(['cp', '-a', '--remove-destination', m['root'] + '/.', r + '/'],
                   check=True)
    if part.startswith('leste-l10n-'):
        # texts a part ships itself stay with it, unless EXCLUDE takes
        # them from it
        for dp, _, fs in os.walk(r):
            for f in fs:
                rel = os.path.relpath(os.path.join(dp, f), r)
                if any(os.path.lexists(os.path.join(o['root'], rel)) and
                       not any(fnmatch.fnmatch(rel, x) for x in EXCLUDE.get(q, ([], []))[0])
                       for q, o in meta.items() if q != part and not q.startswith('_')):
                    os.remove(os.path.join(dp, f))
    for x in EXCLUDE.get(part, ([], []))[0]:
        for hit in glob.glob(os.path.join(r, x)):
            if os.path.isdir(hit) and not os.path.islink(hit):
                shutil.rmtree(hit)
            else:
                os.remove(hit)
    RUNTIME.setdefault(part, []).extend(EXCLUDE.get(part, ([], []))[1])
    plist = []
    needed = set()
    provided = set()
    for dp, dns, fs in os.walk(r):
        for f in sorted(fs):
            p = os.path.join(dp, f)
            rel = '/' + os.path.relpath(p, r)
            plist.append(rel[len('/usr/local/'):] if rel.startswith('/usr/local/') else rel)
            if not os.path.islink(p) and is_elf(p):
                so, nd = readelf(p)
                needed.update(nd)
                if so:
                    provided.add(so)
    for so in sorted(needed - provided):
        if so in ours and ours[so] != part:
            o = meta[ours[so]]
            deps[o['name']] = {'origin': o.get('origin', 'maemo-leste/' + ours[so]),
                               'version': o['version']}
        elif so in ours or so in base_libs:
            # the part's own, or the base system's (libgcc_s, libc++, ...,
            # which some packages also carry: gnat, gcc)
            continue
        elif so in provider:
            cands = provider[so]
            pick = next((c for c in cands if c in in_sysroot), cands[0])
            if pick == 'gtk2' and 'gtk' in meta:
                o = meta['gtk']
                deps['gtk2'] = {'origin': o['origin'], 'version': o['version']}
            else:
                deps[pick] = {'origin': site[pick]['origin'],
                              'version': site[pick]['version']}
        else:
            unresolved.setdefault(m['name'], []).append(so)
    for r_ in RUNTIME.get(part, []):
        if r_ in PRIVATE and not WITH_PRIVATE:
            continue
        o = next((x for x in meta.values() if x['name'] == r_), None)
        if o is not None:
            deps[r_] = {'origin': o.get('origin', 'maemo-leste/' + r_), 'version': o['version']}
        elif r_ in site:
            deps[r_] = {'origin': site[r_]['origin'], 'version': site[r_]['version']}
        else:
            unresolved.setdefault(m['name'], []).append('runtime ' + r_)
    deps.pop(m['name'], None)
    manifest = {
        'name': m['name'],
        'origin': m.get('origin', 'maemo-leste/' + part),
        'version': m['version'],
        'comment': m['comment'][:70],
        'desc': m['desc'] + '\n\nMaemo Leste for FreeBSD (F(x)tec Pro1X port): '
                + (m['url'] or m['www']) + (' at ' + m['rev'][:12] if m['rev']
                                            and not m['rev'].startswith('deb') else ''),
        'maintainer': MAINTAINER,
        'www': m['www'],
        'prefix': '/usr/local',
        'licenselogic': 'single' if len(m['licenses']) == 1 else m.get('logic', 'dual'),
        'licenses': m['licenses'],
        'categories': ['x11'],
        'deps': deps,
        'annotations': {'source': m['url'] or m['www'], 'revision': m['rev']},
    }
    if m.get('scripts'):
        # FreeBSD's own (ldconfig, Qt's qconfig-modules.h)
        manifest['scripts'] = m['scripts']
    with open(os.path.join(d, '+MANIFEST'), 'w') as f:
        json.dump(manifest, f, indent=1)
    with open(os.path.join(d, 'plist'), 'w') as f:
        f.write('\n'.join(plist) + '\n')
    print('%-40s %-22s %-14s %d files, %d deps' % (
        m['name'], m['version'], ','.join(m['licenses']), len(plist), len(deps)))


# --- hildon-session and hildon-meta ---------------------------------------
def dep_on(n):
    o = next((x for x in meta.values() if x['name'] == n), None)
    if o is not None:
        return {'origin': o.get('origin', 'maemo-leste/' + n), 'version': o['version']}
    return {'origin': site[n]['origin'], 'version': site[n]['version']}


rev = subprocess.run(['git', '-C', REPO, 'rev-parse', 'HEAD'],
                     capture_output=True, text=True).stdout.strip()
repo_www = 'https://github.com/smatkovi/pro1x-maemo-leste'
# a step up with every commit of this repository, so that pkg upgrade
# brings hildon-meta's new apps
own_version = '1.0.' + (subprocess.run(['git', '-C', REPO, 'rev-list', '--count', 'HEAD'],
                                       capture_output=True, text=True).stdout.strip() or '0')
# what shows in a menu, the home view, the status area or the control panel
apps = sorted(x['name'] for x in meta.values()
              if any(os.path.isdir(os.path.join(x['root'], d, 'share/applications'))
                     for d in ('usr/local', 'usr')))
# and what works without an entry: the on-screen keyboard, daemons, the
# alarm dialog (with alarmd and the system UI), the click sounds, the
# ringtones and call sounds the profiles and alarms choose from
apps = sorted(set(apps) | {x['name'] for x in meta.values() if x['name'] in (
    'hildon-input-method-plugins', 'headphoned', 'wifi-assistant', 'hildon-status-menu',
    'osso-systemui-alarm', 'maemo-input-sounds', 'maemo-ringtones-mr0', 'osso-sounds-rtc',
    'pro1x-volume-osd', 'pro1x-volume-keys', 'pro1x-alarmboot')})
# the N9's own user interface (harmattan.sh), where it is built: all of
# it but the headers
harmattan = sorted(x['name'] for x in meta.values()
                   if x['name'].startswith('harmattan-') and x['name'] != 'harmattan-mce-dev')
sessions = [
        ('hildon-session', "Maemo Leste's Hildon session for FreeBSD",
         'hildon-session (startx hildon-session, or "startx hildon" on the Pro1X) '
         'starts hildon-desktop with the status menu and home, as Leste\'s X session '
         'does; rc.d/hildon_setup makes the GConf defaults and the theme for the '
         'HiDPI factor at boot; links for the paths Hildon has built in.',
         os.path.join(REPO, 'freebsd/hildon/rootfs'), SESSION_DEPS),
        ('hildon-meta', "Maemo Leste's Hildon desktop with all its apps on FreeBSD",
         'Installs the Hildon session and every app and plugin built for FreeBSD '
         'from Maemo Leste (and its extras), see freebsd/hildon/README.md.',
         None, ['hildon-session'] + apps)]
if harmattan:
    sessions.append(
        ('harmattan-session', "The Nokia N9's user interface as a session for FreeBSD",
         'harmattan-session (startx harmattan-session, or "startx harmattan" on the Pro1X) '
         "starts the N9's compositor, system UI, keyboard and home screen "
         '(github.com/smatkovi/harmattan-qt5), as on Arch; MeeGo in Hildon\'s and '
         "Xfce's menus switches to it, Hildon among its applications back.",
         os.path.join(REPO, 'freebsd/hildon/harmattan-rootfs'),
         harmattan + ['gconf2', 'dbus', 'xset']))
for name, comment, desc, root, deps in sessions:
    d = os.path.join(OUT, name)
    r = os.path.join(d, 'root')
    os.makedirs(r)
    plist = []
    if root:
        subprocess.run(['cp', '-a', root + '/.', r + '/'], check=True)
        for dp, _, fs in os.walk(r):
            for f in sorted(fs):
                rel = '/' + os.path.relpath(os.path.join(dp, f), r)
                plist.append(rel[len('/usr/local/'):] if rel.startswith('/usr/local/') else rel)
    manifest = {
        'name': name, 'origin': 'pro1x/' + name, 'version': own_version,
        'comment': comment, 'desc': desc, 'maintainer': MAINTAINER,
        'www': repo_www, 'prefix': '/usr/local', 'licenselogic': 'single',
        'licenses': ['UNKNOWN'], 'categories': ['x11'],
        'deps': {n: dep_on(n) for n in deps},
        'annotations': {'source': repo_www, 'revision': rev},
    }
    if name == 'harmattan-session':
        manifest['messages'] = [{'message':
            "The N9's user interface needs the system D-Bus: sysrc dbus_enable=YES &&\n"
            'service dbus start. Then: startx /usr/local/bin/harmattan-session'}]
    if name == 'hildon-session':
        manifest['messages'] = [{'message':
            'Hildon needs the system D-Bus: sysrc dbus_enable=YES && service dbus start.\n'
            'Its GConf defaults and theme are made at boot (rc.d/hildon_setup), the first\n'
            'time with: service hildon_setup start. Then: startx /usr/local/bin/hildon-session\n'
            "Leste's GTK (with the Hildon changes) is this repository's gtk2: pkg install -r pro1x gtk2"}]
    with open(os.path.join(d, '+MANIFEST'), 'w') as f:
        json.dump(manifest, f, indent=1)
    with open(os.path.join(d, 'plist'), 'w') as f:
        f.write('\n'.join(plist) + ('\n' if plist else ''))
    meta['_' + name] = {'name': name, 'version': own_version, 'origin': 'pro1x/' + name,
                        'root': r}
    print('%-40s %-22s %-14s %d files, %d deps' % (name, own_version, 'UNKNOWN', len(plist), len(deps)))

for n, sos in sorted(unresolved.items()):
    print('unresolved in %s: %s' % (n, ' '.join(sorted(set(sos)))))
