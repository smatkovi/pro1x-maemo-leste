# Hildon on FreeBSD

Maemo Leste's Hildon desktop (hildon-desktop, hildon-home,
hildon-status-menu and the libraries under them), cross-compiled for
FreeBSD/arm64 on a Linux PC, for the root image of `freebsd/rootfs.sh`.

    WORK=/path/to/freebsd-build bash freebsd/hildon/build.sh [part...]

`build.sh` builds a sysroot from the release's base system and FreeBSD's
packages (`deps.sh`: gtk2, gconf2, dbus-glib, mesa, ...), then each part
from `github.com/maemo-leste/<part>` with clang for
`aarch64-unknown-freebsd15.0`, Debian's own patches (`debian/patches`)
and the FreeBSD changes in `patches/<part>/`. The results go to
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
and the GConf defaults, made from the schemas at boot.

Untested on the phone so far.
