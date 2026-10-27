# Hildon on FreeBSD

Maemo Leste's Hildon desktop (hildon-desktop, hildon-home,
hildon-status-menu and the libraries under them), cross-compiled for
FreeBSD/arm64 on a Linux PC, for the root image of `freebsd/rootfs.sh`.

    WORK=/path/to/freebsd-build bash freebsd/hildon/build.sh [part...]

`build.sh` builds a sysroot from the release's base system and FreeBSD's
packages (`deps.sh`: gtk2, gconf2, dbus-glib, mesa, ...), then each part
from `github.com/maemo-leste/<part>` with clang for
`aarch64-unknown-freebsd15.0`, Debian's own patches (`debian/patches`),
the FreeBSD changes in `patches/<part>/` and the HiDPI changes Leste
gets too (`../../patches/hidpi/<part>.patch`, see
`tools/pro1x-hidpi/README.md`). The results go to
`$WORK/hildon/stage` and into the sysroot for the parts after them.
Leste's theme, icons and sounds come from Leste's archive (InRelease
checked with `keys/`, packages with the checksums it signs).
`rootfs.sh` puts the stage into the root image when it exists, with the
packages Hildon runs with and LLVM's library for Mesa's software renderer
(hildon-desktop composites with GLES; FreeBSD has no driver for the
Adreno GPU).

| Part | Note |
| --- | --- |
| clutter-0.8 | Leste's fork, EGL/GLES2 |
| mce-dev | headers |
| gtk | Leste's GTK 2.24 with the hildonize patches, replacing FreeBSD's gtk2; only the libraries and modules |
| sapwood | the theme engine; no abstract sockets (Linux only) |
| libmatchbox2 | also static: hildon-desktop links it in, as Leste does |
| libhildon | the first weekday without glibc's langinfo: Monday |
| libosso | |
| iphbd | only the client library libiphb |
| libhildondesktop, libhildonmime | |
| profiled | libprofile and the daemon; no nested functions (clang) |
| osso-bookmark-engine, hildon-thumbnail, libhildonfm | for hildon-home |
| hildon-status-menu, hildon-home, hildon-desktop | `G_DISABLE_CHECKS` as Leste builds them |

`compat/` has what Maemo's code takes from glibc (`TEMP_FAILURE_RETRY`,
`HOST_NAME_MAX`, `<features.h>`, `<sys/vfs.h>`); `rootfs/` the session
(`startx /usr/local/bin/hildon-session`: status menu and home beside
hildon-desktop, as Leste's Xsession.post starts them), links for the
paths Hildon has built in (`/etc/hildon/theme`, `/usr/share/themes`, ...)
and the GConf defaults, made from the schemas at boot. The session is
Xfce unless `pro1x.desktop=hildon` is in `freebsd/cmdline` or X was
started with `startx hildon`; "Xfce" in Hildon's menu and "Hildon" in
Xfce's switch over (`pro1x-desktop`), and Hildon's menu also has Xfce's
terminal, file manager and editor.

Hildon is scaled to the N900's size at the full resolution
(`tools/pro1x-hidpi`): `build.sh` also builds `pro1x-theme-scale` and
`pro1x-hidpi`, `rootfs.sh` makes the theme for the default factor into the
image (`/usr/local/share/pro1x/hidpi`), and at boot `pro1x_setup` points
`/etc/hildon/theme` at it, or at one made into `/var` for another factor
in `pro1x-display.conf`.

## The apps

`apps.sh` lists what is built after the desktop: Leste's libraries the
apps need (input methods, clock and time zones, calendar, alarms,
connectivity, the code lock, GConf for Qt, QtMaemo5), the apps Leste
installs (terminal, PDF viewer, control panel with its applets, battery
and profiles in the status area, on-screen keyboard), apps from Leste's
extras and the Maemo 5 apps ported to Leste (github.com/smatkovi), each
at a pinned revision, with FreeBSD changes in `patches/<part>/`. Most
changes are of three kinds: glibc's extras FreeBSD lacks (`compat/`, or
`#ifdef` where a feature has no counterpart, such as the locale names of
glibc's address category), Linux headers (`linux/input.h` is
`dev/evdev/input.h` on FreeBSD) and what clang refuses where GCC let it
pass (nested functions, C++ that drops `const`). `build.sh` itself takes
care of what applies to every part: menu entries and D-Bus services that
name `/usr` where the files went to `/usr/local`, Leste's process
supervisor (`dsmetool`) in them, and programs that export only `main`,
which on FreeBSD must export `environ` too (the C library and glib look
for it in the program). Calls of functions without a declaration are
listed after each part (a pointer such a call returns loses its upper
half on arm64).

`extra/<part>/` adds files a part needs on FreeBSD (D-Bus activation for
daemons Leste starts from its init scripts, links Debian's packaging
makes). The texts come from Leste's `*-l10n-*` packages (English and
German, `leste-l10n-en`, `leste-l10n-de`). The session also starts the
theme engine's image server (sapwood) and puts an index of hicolor that
includes the Hildon icon directories in front (FreeBSD's hicolor package
owns the plain one).

Alarms work as on Leste, from the Hildon session: `alarmd` keeps the
queue (`/var/cache/alarmd`, kept over a reboot with the writable ZFS root
only) and follows clockd's time changes, the system UI (`osso-systemui`)
shows its dialog through `osso-systemui-alarm`, and the status area clock
(`statusbar-alarm`) shows that an alarm is set. FreeBSD has no RTC wake-up
alarm (Linux's `RTC_WKALM_SET`): an alarm rings while the system runs, it
does not power the phone up. Both start with the session, since alarmd
needs its bus. `../vm/try-alarm.sh` sets an alarm as clock-ui does and
takes a screenshot of the dialog (`RESPOND=snooze` or `stop` taps a
button).

MeeGo apps: `qt-components` is com.nokia.meego, the Nokia N9's Qt Quick
components, in Nemo Mobile's port to Qt 5 (`import com.nokia.meego 2.0`),
with what Qt 5.15 needs (`patches/qt-components`): Harmattan's fonts
(Nokia Pure Text), the theme's `images/` directory, the window at its own
size below Hildon's title bar, and Qt Quick 2's ways that the port missed
(a subtype's style shadowed unseen, closed dialogs and hidden banners that
took taps). They draw with a meegotouch theme in
`/usr/local/share/themes`; the N9's own, blanco, and the Nokia Pure fonts
are Nokia's and come from one's own firmware (the image takes them from
the private `$WORK/private/harmattan`, `HARMATTAN=`). `meego-clock`
(github.com/smatkovi/meego-clock) is the N9's clock written anew on them:
the analog clock, the alarms, and the alarm editor with the two-circle time
picker, its alarms alarmd's as clock-ui makes them. `../vm/session.sh`
keeps a Hildon session open in the VM, to drive an app step by step with
xdotool and screenshots.

Qt apps build with the PC's `moc`, `uic` and `rcc` (the same Qt 5.15 as
FreeBSD's) against FreeBSD's Qt in the sysroot (`qt_qmake`, `qt_cmake` in
`apps.sh`), with Leste's "maemo" qmake spec on FreeBSD's.

### Web engines

FreeBSD's quarterly packages for arm64 have neither QtWebEngine nor
QtWebKit any more, which Jib (QtWebEngine) and Dorian (QtWebKit) need:

- `qt5-webengine` is FreeBSD's own package from FreeBSD 15.1's package set
  (`release_1`, origin `www/qt5-webengine`, built from the ports tree at
  `f1707ff8961d`), fetched from that set's signed index and checked
  against the pinned SHA-256 (`build.sh`: a part "`pkg`" with the URL
  `pkg:<set>`), repackaged with its dependencies on quarterly's packages.
  It is the same Qt 5.15 except for FFmpeg: it links FFmpeg 8.1
  (`libavcodec.so.62`), quarterly has FFmpeg 9.
- `ffmpeg8-libs` is FFmpeg 8.1's shared libraries (LGPL parts only, its
  own decoders, with the patch FreeBSD's port adds for Chromium,
  `av_stream_get_first_dts`), next to quarterly's FFmpeg: only the
  versioned libraries, no headers or unversioned links.
- `qt5-webkit` is QtWebKit 5.212.0-alpha4, the last release, with the
  patches of FreeBSD's port (until 2023) and Arch's in
  `patches/qt5-webkit/` (ICU 76, Python 3, bison, newer compilers, the
  JIT's cache flush on FreeBSD/arm64): the widgets (QWebView), JIT on;
  without WebKit 2 (QML), video, geolocation and XSLT. Its build tree
  takes about 1 GB (`BUILD=<directory>` puts the build trees elsewhere).

Release tarballs are parts too: a URL ending in `.tar.*` with its SHA-256
as the pinned revision. Together they add about 250 MB to `hildon-meta`.
Pages in scripts FreeBSD has no fonts for by default (CJK) need a font
package such as `noto` (all of Noto) or `noto-jp`, `noto-kr`, `noto-sc`.

## Packages

`package.sh` makes a FreeBSD package of every part and two more:
`hildon-session` (the session script, `rc.d/hildon_setup` for the GConf
defaults and the HiDPI theme at boot, the links of `rootfs/`) and
`hildon-meta` (everything). The metadata comes from each part's
`debian/` directory (`pkgmeta.py`: version, description, licence, and
the dependencies from the libraries the files link against, mapped to
the other parts, FreeBSD's packages or the base system); the packages
themselves are made with FreeBSD's own `pkg create` and `pkg repo` in a
FreeBSD/arm64 machine, the VM of `../vm/vm.sh`, which also tries them
(`../vm/try-hildon.sh`: Hildon at 2160x1080 on a virtual screen, the apps
started from their menu entries, a screenshot of each).

Leste's GTK replaces FreeBSD's `gtk2` (same name, one version step
higher, FreeBSD's package with Leste's libraries in it), so it has to be
installed from this repository explicitly once. On FreeBSD 15 (arm64), as
root, with the packages of a release in `/path/to/pkg`:

    cat > /usr/local/etc/pkg/repos/pro1x.conf <<EOF
    pro1x: { url: "file:///path/to/pkg", signature_type: "none", priority: 10 }
    EOF
    pkg update
    pkg install -r pro1x gtk2
    pkg install hildon-meta
    startx /usr/local/bin/hildon-session

Licences: as each package says; "UNKNOWN" where the source carries no
licence statement. The sources are the pinned revisions in the table and
`patches/`.

<!-- packages -->
| Package | Version | Licence | Source | Revision | State |
| --- | --- | --- | --- | --- | --- |
| alarmd | 1.1.23_1 | LGPL21 | [alarmd](https://github.com/maemo-leste/alarmd) | `6a248a323bcc` | runs (VM: alarms ring while Hildon runs; no RTC wake-up on FreeBSD) |
| applet-datetime | 0.6.2 | GPLv3 | [applet-datetime](https://github.com/maemo-leste/applet-datetime) | `39054c07ac94` | builds |
| braek | 1.3.1 | BSD3CLAUSE | [braek](https://github.com/maemo-leste-extras/braek) | `77ceda0d3d92` | runs (VM) |
| cal-home-widget | 1.7 | GPLv2+ | [cal-home-widget](https://github.com/maemo-leste-extras/cal-home-widget) | `52ef149cdca0` | builds |
| calendar-backend | 0.9.1 | LGPL21 | [calendar-backend](https://github.com/maemo-leste/calendar-backend) | `ce6ae29ea5d4` | builds |
| cityinfo | 0.5.19.1_1 | LGPL21 | [cityinfo](https://github.com/maemo-leste/cityinfo) | `2228f3ae85ac` | builds |
| clock | 0.5.17 | GPLv3 | [clock](https://github.com/maemo-leste/clock) | `8abc98d2854f` | builds |
| clock-ui | 0.8.3_1 | GPLv2+ | [clock-ui](https://github.com/maemo-leste/clock-ui) | `7c832feb877b` | runs (VM: world clock, the next alarm from alarmd) |
| clockd | 0.0.48_1 | GPLv2 | [clockd](https://github.com/maemo-leste/clockd) | `9130890fe1ae` | builds |
| clutter-0.8 | 0.8.2.79 | LGPL20+ | [clutter-0.8](https://github.com/maemo-leste/clutter-0.8) | `ead56341b457` | builds |
| codelockui | 1.6.20 | LGPL20+ | [codelockui](https://github.com/maemo-leste/codelockui) | `07c6654ae546` | builds |
| countdowntimer | 0.6.1.2 | GPLv2+ | [countdowntimer](https://git.maemo.org/leste-extras/countdowntimer) | `ddc1f7027af9` | builds |
| desktop-cmd-exec | 1.22 | GPLv2+ | [desktop-cmd-exec](https://github.com/maemo-leste-extras/desktop-cmd-exec) | `e3ac73e7b031` | builds |
| dorian | 0.5.8.1 | GPLv3 | [dorian](https://github.com/maemo-leste-extras/dorian) | `da92eda1d5d9` | runs (VM: opens an EPUB) |
| easylist | 0.3.12.1.leste1_1 | GPLv2+ | [easylist](https://github.com/maemo-leste-extras/easylist) | `d5c5bcc6c677` | runs (VM) |
| ffmpeg8-libs | 8.1.2 | LGPL21+ | [ffmpeg-8.1.2.tar.xz](https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz) | `sha256 464beb5e7bf0` | runs (VM, through QtWebEngine) |
| flappybird | 0.0.3.1_1 | GPLv2+ | [flappybird](https://git.maemo.org/leste-extras/flappybird) | `21f81151207a` | runs (VM, no sound: no WAV decoder) |
| gpodder-leste | 4.17.2.leste1 | ISC | [gpodder-leste](https://github.com/smatkovi/gpodder-leste) | `2759445563b5` | runs (VM) |
| gtk2 | 2.24.33.1_1 | LGPL20 | [gtk](https://github.com/maemo-leste/gtk) | `bf9a7dfb8f69` | runs (VM) |
| hamsterfiler | 0.8.1_1 | GPLv3+ | [hamsterfiler](https://github.com/maemo-leste-extras/hamsterfiler) | `19ac58143e2e` | runs (VM) |
| headphoned | 2.0.0 | GPLv2+ | [headphoned](https://github.com/smatkovi/headphoned) | `d476cb4a8bf9` | builds |
| hexagonrpc | 0.5.0.24 | GPLv3+ | [hexagonrpc](https://github.com/linux-msm/hexagonrpc) | `40e50412812a` | builds; needs the Pro1X's ADSP (untested on the phone) |
| hildon-control-panel | 2.2.32.2 | LGPL21 | [hildon-control-panel](https://github.com/maemo-leste/hildon-control-panel) | `359e43a8a899` | runs (VM) |
| hildon-control-panel-personalisation | 1.0.8 | GPLv3 | [hildon-control-panel-personalisation](https://github.com/maemo-leste/hildon-control-panel-personalisation) | `4f6086c7e69b` | builds |
| hildon-desktop | 2.2.197_2 | GPLv2 | [hildon-desktop](https://github.com/maemo-leste/hildon-desktop) | `7f8ce2104a48` | runs (VM) |
| hildon-games-wrapper | 1.9.5 | LGPL21 | [hildon-games-wrapper](https://github.com/maemo-leste/hildon-games-wrapper) | `2b36c80334d4` | runs (VM, through Marbles) |
| hildon-home | 0.3.80 | LGPL21 | [hildon-home](https://github.com/maemo-leste/hildon-home) | `8dd16c59f867` | runs (VM) |
| hildon-im-vkbrenderer3 | 3.3.27.1 | GPLv2+ | [hildon-im-vkbrenderer3](https://github.com/maemo-leste/hildon-im-vkbrenderer3) | `b8b4dbe0993b` | builds |
| hildon-input-method | 2.1.61.1 | LGPL21 | [hildon-input-method](https://github.com/maemo-leste/hildon-input-method) | `0977df548b0b` | builds |
| hildon-input-method-framework | 2.1.71.1 | LGPL21 | [hildon-input-method-framework](https://github.com/maemo-leste/hildon-input-method-framework) | `99afab2e2e78` | builds |
| hildon-input-method-plugins | 3.9.91 | GPLv2+ | [hildon-input-method-plugins](https://github.com/maemo-leste/hildon-input-method-plugins) | `04c1d47222d0` | builds |
| hildon-meta | 1.0.130 | UNKNOWN | [pro1x-maemo-leste](https://github.com/smatkovi/pro1x-maemo-leste) | `8b721ad64b43` | builds |
| hildon-plugins-notify-sv | 0.65 | GPLv3 | [hildon-plugins-notify-sv](https://github.com/maemo-leste/hildon-plugins-notify-sv) | `ddfe375457f7` | builds |
| hildon-session | 1.0.130 | UNKNOWN | [pro1x-maemo-leste](https://github.com/smatkovi/pro1x-maemo-leste) | `8b721ad64b43` | runs (VM) |
| hildon-status-menu | 0.3.43.1 | LGPL21 | [hildon-status-menu](https://github.com/maemo-leste/hildon-status-menu) | `796dbd7ca973` | runs (VM) |
| hildon-theme-alpha | 9.7 | GPLv3 | [maemo-leste](https://github.com/maemo-leste) |  | builds |
| hildon-thumbnail | 3.1.4 | LGPL21 | [hildon-thumbnail](https://github.com/maemo-leste/hildon-thumbnail) | `86bd10f3f620` | builds |
| hildon-time-zone-chooser | 0.6.0 | GPLv3 | [hildon-time-zone-chooser](https://github.com/maemo-leste/hildon-time-zone-chooser) | `123ee9c1606f` | builds |
| icd2-osso-ic-dev | 2.0.1 | LGPL21 | [icd2-osso-ic-dev](https://github.com/maemo-leste/icd2-osso-ic-dev) | `acf440d9d3f2` | builds |
| iphbd | 0.1.3 | GPLv2 | [iphbd](https://github.com/maemo-leste/iphbd) | `bad0de1ff36d` | builds |
| jib | 0.6.7 | UNKNOWN | [jib](https://github.com/maemo-leste-extras/jib) | `5dd4a340d20d` | runs (VM: renders https pages) |
| leafpad-maemo | 0.8.17le.leste1 | GPLv2 | [leafpad](https://github.com/maemo-leste-extras/leafpad) | `9ae1e0314158` | builds |
| leste-l10n-de | 2026.07.05 | UNKNOWN | [leste](https://maedevu.maemo.org/leste) |  | builds |
| leste-l10n-en | 2026.07.05_1 | UNKNOWN | [leste](https://maedevu.maemo.org/leste) |  | builds |
| libcomapp | 1.8.07 | LGPL21 | [libcomapp](https://github.com/maemo-leste/libcomapp) | `84317d5c6614` | builds |
| libconbtui | 1.4.4 | LGPL21+ | [libconbtui](https://github.com/maemo-leste/libconbtui) | `91e6bd445be8` | builds |
| libconic | 0.24.2 | LGPL21 | [libconic](https://github.com/maemo-leste/libconic) | `cd9db218dacd` | builds |
| libdsme | 0.60.33 | LGPL21 | [libdsme](https://github.com/maemo-leste/libdsme) | `e4f8b85a6743` | builds |
| libgq-gconf | 0.3 | LGPL21 | [libgq-gconf](https://github.com/maemo-leste/libgq-gconf) | `8617c495b6ac` | builds |
| libhildon | 2.2.30_1 | LGPL21 | [libhildon](https://github.com/maemo-leste/libhildon) | `78c268e51200` | builds |
| libhildondesktop | 2.2.3 | LGPL21 | [libhildondesktop](https://github.com/maemo-leste/libhildondesktop) | `d05587180c24` | builds |
| libhildonfm | 2.28.27 | LGPL21 | [libhildonfm](https://github.com/maemo-leste/libhildonfm) | `6df73d0c97f2` | builds |
| libhildonmime | 4.2.0 | LGPL20 | [libhildonmime](https://github.com/maemo-leste/libhildonmime) | `bfedaf83ff01` | builds |
| libimengines | 4.2.5 | LGPL20+ | [libimengines](https://github.com/maemo-leste/libimengines) | `5541d57ab84f` | builds |
| libimlayouts | 1.6.5 | LGPL20+ | [libimlayouts](https://github.com/maemo-leste/libimlayouts) | `d7a4960189b2` | builds |
| libmatchbox2 | 0.2.107 | LGPL21+ | [libmatchbox2](https://github.com/maemo-leste/libmatchbox2) | `208c59d7a3ba` | builds |
| libosso | 2.35.1 | LGPL21 | [libosso](https://github.com/maemo-leste/libosso) | `ebd2f1804c29` | builds |
| libplayback | 0.5.25 | LGPL21 | [libplayback](https://github.com/maemo-leste/libplayback) | `ed18405bf5dd` | builds |
| libqmi | 1.34.0 | LGPL21+, GPLv2+ | [libqmi](https://gitlab.freedesktop.org/mobile-broadband/libqmi) | `3f07d6e5b467` | builds |
| libqrtr-glib | 1.4.0 | LGPL21+ | [libqrtr-glib](https://gitlab.freedesktop.org/mobile-broadband/libqrtr-glib) | `69791f8f2a09` | builds |
| libssc | 0.4.4 | GPLv3+ | [libssc](https://codeberg.org/DylanVanAssche/libssc) | `0cf77b93b557` | builds |
| live-wallpaper | 1.0.3.1.leste1 | GPLv2+ | [live-wallpaper](https://github.com/maemo-leste-extras/live-wallpaper) | `d950d1e44e66` | builds |
| maefat | 2.1.1.1.leste2 | GPLv2 | [maefat](https://github.com/maemo-leste-extras/maefat) | `11bc84b6fccb` | runs (VM) |
| maemo-launcher | 0.39 | GPLv2 | [maemo-launcher](https://github.com/maemo-leste/maemo-launcher) | `2cb10ec66a22` | builds |
| maepad | 1.11_1 | LGPL21 | [maepad](https://git.maemo.org/leste-extras/maepad) | `b50c9caadd86` | runs (VM) |
| mce-dev | 1.10.19 | LGPL21 | [mce](https://github.com/maemo-leste/mce) | `d3260521eaf1` | builds |
| mihphoto | 1.0.16_1 | GPLv3 | [mihphoto](https://github.com/maemo-leste-extras/mihphoto) | `bab0cb18bc0e` | runs (VM) |
| mstardict | 0.6.5.leste1 | GPLv3+ | [mstardict](https://github.com/maemo-leste-extras/mstardict) | `48f7924faed8` | builds |
| osso-af-settings | 0.9.4.1 | GPLv2+ | [osso-af-settings](https://github.com/maemo-leste/osso-af-settings) | `3e35b3b2ca7f` | builds |
| osso-applet-display | 1.6.1 | GPLv2+ | [osso-applet-display](https://github.com/maemo-leste/osso-applet-display) | `66309efec84a` | builds |
| osso-applet-languageregional | 1.0.2 | GPLv3 | [osso-applet-languageregional](https://github.com/maemo-leste/osso-applet-languageregional) | `0f82237b8f5e` | builds |
| osso-applet-notificationlight | 0.0.10 | GPLv2+ | [osso-applet-notificationlight](https://github.com/maemo-leste/osso-applet-notificationlight) | `e364719aa592` | builds |
| osso-applet-textinput | 3.2.3 | UNKNOWN | [osso-applet-textinput](https://github.com/maemo-leste/osso-applet-textinput) | `0b32f803ace8` | builds |
| osso-bookmark-engine | 2.0.0 | GPLv3 | [osso-bookmark-engine](https://github.com/maemo-leste/osso-bookmark-engine) | `0a8ff94e3cfe` | builds |
| osso-calculator | 2.1 | GPLv3+ | [osso-calculator](https://github.com/maemo-leste/osso-calculator) | `6fff52bb301f` | runs (VM) |
| osso-chess-ui | 1.8.3.2.leste1 | GPLv2+ | [osso-chess-ui](https://github.com/smatkovi/osso-chess-ui) | `4674e8e999e7` | runs (VM: against GNU Chess 6.3, drag and tap moves, scaled to 2160x1080) |
| osso-games-startup | 1.7.53 | LGPL21 | [osso-games-startup](https://github.com/maemo-leste/osso-games-startup) | `1740aa8daaf4` | builds |
| osso-icons-default | 2.5.3.5m7 | UNKNOWN | [leste](https://maedevu.maemo.org/leste) |  | builds |
| osso-lmarbles | 1.6.38.2.leste1 | GPLv2+ | [osso-lmarbles](https://github.com/smatkovi/osso-lmarbles) | `97dcd6572031` | runs (VM: start screen, first level; red and blue swapped in the VM's picture, to check on a device) |
| osso-mahjong | 1.9.1_1 | GPLv2+ | [osso-mahjong](https://github.com/maemo-leste-extras/osso-mahjong) | `fbe179709a15` | runs (VM) |
| osso-pdf-viewer | 1.10.3 | GPLv2 | [osso-pdf-viewer](https://github.com/maemo-leste/osso-pdf-viewer) | `a5c944ebff05` | runs (VM) |
| osso-sounds-ui | 5.6.0m5 | UNKNOWN | [leste](https://maedevu.maemo.org/leste) |  | builds |
| osso-systemui | 0.2.1 | LGPL21 | [osso-systemui](https://github.com/maemo-leste/osso-systemui) | `d1b65f09fe6d` | runs (VM: shows the alarm dialog) |
| osso-systemui-alarm | 0.3.3.0cssu1 | LGPL21 | [osso-systemui-alarm](https://github.com/maemo-leste/osso-systemui-alarm) | `f9350639d4ba` | runs (VM: the alarm dialog, Snooze and Stop answered; no sound on FreeBSD yet) |
| osso-systemui-dbus-dev | 0.1.5 | LGPL21 | [osso-systemui-dbus-dev](https://github.com/maemo-leste/osso-systemui-dbus-dev) | `dc5c9b542cf6` | builds |
| osso-xterm | 0.17 | GPLv2 | [osso-xterm](https://github.com/maemo-leste/osso-xterm) | `6c912f73983e` | runs (VM) |
| pd-mapper | 1.1 | BSD3CLAUSE | [pd-mapper](https://github.com/linux-msm/pd-mapper) | `5ecd2fe926ac` | builds |
| pedometerhomewidget | 0.4.0 | GPLv3+ | [pedometerhomewidget](https://github.com/smatkovi/pedometerhomewidget) | `06d5246191fa` | builds |
| pro1x-gnss | 1.0 | UNKNOWN | [pro1x-maemo-leste](https://github.com/smatkovi/pro1x-maemo-leste) | `8b721ad64b43` | builds; needs the Pro1X's modem (untested on the phone) |
| pro1x-hidpi | 1.0 | UNKNOWN | [pro1x-maemo-leste](https://github.com/smatkovi/pro1x-maemo-leste) | `8b721ad64b43` | builds |
| pro1x-proximity | 1.0 | UNKNOWN | [pro1x-maemo-leste](https://github.com/smatkovi/pro1x-maemo-leste) | `8b721ad64b43` | builds; needs the Pro1X's ADSP (untested on the phone) |
| pro1x-standby | 1.0 | UNKNOWN | [pro1x-maemo-leste](https://github.com/smatkovi/pro1x-maemo-leste) | `8b721ad64b43` | runs (VM: time-out, dimming, double tap, power key, proximity signals; no system calls while in use) |
| pro1x-steps | 1.0 | UNKNOWN | [pro1x-maemo-leste](https://github.com/smatkovi/pro1x-maemo-leste) | `8b721ad64b43` | builds; needs the Pro1X's ADSP (untested on the phone) |
| profiled | 1.0.8 | BSD3CLAUSE | [profiled](https://github.com/maemo-leste/profiled) | `8e16f3dc9d42` | builds |
| profilesx | 1.9_1 | GPLv2+ | [profilesx](https://github.com/maemo-leste/profilesx) | `4c89c9701fdc` | builds |
| qalendar | 1.0.3.1 | GPLv3 | [qalendar](https://github.com/maemo-leste/qalendar) | `04ccfca3ca17` | runs (VM) |
| qrtr | 1.2.18 | BSD3CLAUSE | [qrtr](https://github.com/linux-msm/qrtr) | `29e36ae16438` | builds |
| qshot | 0.0.2_1 | BSD3CLAUSE | [qshot](https://github.com/maemo-leste-extras/qshot) | `f10335de9c66` | runs (VM) |
| qt-components | 1.4.9 | BSD3CLAUSE | [qt-components](https://github.com/sailfishos/qt-components) | `c967fbd79e75` | builds (the theme it needs not yet packaged) |
| qt5-webengine | 5.15.19p0_6 | LGPL21 | [qt5-webengine-5.15.19p0_6~c5de7df005.pkg](https://pkg.freebsd.org/FreeBSD:15:aarch64/release_1/All/Hashed/qt5-webengine-5.15.19p0_6~c5de7df005.pkg) | `sha256 c5de7df00518` | runs (VM, through Jib) |
| qt5-webkit | 5.212.0.a4 | LGPL21 | [qtwebkit-5.212.0-alpha4.tar.xz](https://github.com/qtwebkit/qtwebkit/releases/download/qtwebkit-5.212.0-alpha4/qtwebkit-5.212.0-alpha4.tar.xz) | `sha256 9ca126da9273` | runs (VM, through Dorian) |
| qtstyleplugins | 5.1.14.1 | LGPL-2.1 with Qt-1.1 exception, GPLv3 | [qtstyleplugins](https://github.com/maemo-leste/qtstyleplugins) | `c371b3232025` | builds |
| rmtfs | 1.3 | BSD3CLAUSE | [rmtfs](https://github.com/linux-msm/rmtfs) | `b30a3eb38f9a` | builds; needs the Pro1X's modem (untested on the phone) |
| sapwood | 3.1.3.1 | LGPL21 | [sapwood](https://github.com/maemo-leste/sapwood) | `6849cd690ae2` | runs (VM) |
| status-area-applet-battery | 1.5.9.1 | GPLv2 | [status-area-applet-battery](https://github.com/maemo-leste/status-area-applet-battery) | `0e4fcf07b39b` | builds |
| status-menu-applet-profiles | 0.14.2 | GPLv2 | [status-menu-applet-profiles](https://github.com/maemo-leste/status-menu-applet-profiles) | `f655a3c400fd` | builds |
| statusbar-alarm | 0.6.3 | GPLv3 | [statusbar-alarm](https://github.com/maemo-leste/statusbar-alarm) | `55045c7780b2` | runs (VM: the status area clock with the alarm icon) |
| statusbar-alarm-dbus-api | 0.5.10.2 | BSD3CLAUSE | [statusbar-alarm-dbus-api](https://github.com/maemo-leste/statusbar-alarm-dbus-api) | `acf2954ea574` | builds |
| tqftpserv | 1.3.2 | BSD3CLAUSE | [tqftpserv](https://github.com/linux-msm/tqftpserv) | `0bf5b98c2422` | builds |
| wifi-assistant | 1.0.0 | BSD3CLAUSE | [wifi-assistant](https://github.com/smatkovi/wifi-assistant) | `604bbe98d8b5` | builds |
| wpeditor | 3.53.1 | LGPL21+ | [wpeditor](https://github.com/maemo-leste/wpeditor) | `84a17efe3392` | builds |
<!-- /packages -->

Untested on the phone so far.
