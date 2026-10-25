# Hildon at the N900's size (HiDPI)

Hildon was made for the N900's 800x480 screen: its theme, its widgets and
the desktop count in those pixels. On the Pro1X's 2160x1080 they come out
less than half as large. Instead of a lower screen resolution (blurry,
and the browser would lose its pixels too), everything Hildon draws is
scaled by one factor at the full resolution:

    SCALE=2.25

in `pro1x-display.conf` on userdata (`/host` in Leste,
`/dev/pro1x-host` in FreeBSD), the same for both systems. 2.25 gives the
N900's proportion, 480 of 1080 lines: the title bar is 126 pixels high
instead of 56. Without the file the factor is 2.25; 1 turns it off.

| Part | What it scales |
| --- | --- |
| `pro1x-theme-scale` | a copy of the theme: images, the gtkrc's lengths (borders, thicknesses, style properties, icon sizes with libhildon's), matchbox2's decorations |
| `pro1x-hidpi theme` | at boot: that copy for the configured factor (made once per factor) as `/etc/hildon/theme` |
| `pro1x-hidpi env`, `x11` | in the session: `HILDON_SCALE`, `Xft.dpi` = 96 x factor (fonts are in points), GTK 3 (`GDK_SCALE` integer, `GDK_DPI_SCALE` the rest) and Qt |
| `patches/hidpi/` | the lengths built into gtk (Hildon's button heights), libhildon, hildon-desktop (title bar, launcher, task switcher, Clutter's font resolution), hildon-status-menu, hildon-home (and its wallpapers at the screen's size instead of 800x480) |

Every length becomes `round(length * factor)`, in the theme and in the
code alike, so images, sprite rectangles and layout agree. The patched
programs take the factor from `HILDON_SCALE`, or else from the theme they
run with (`/etc/hildon/theme/.pro1x-theme`, which `pro1x-theme-scale`
writes), so a process started without the session's environment still
matches its theme.

Changing the factor (Leste, as root): `pro1x-hidpi set 2`, then restart
X; FreeBSD reads the same file at its next boot.

Leste: `scripts/49-hidpi.sh` (the patched packages as `+pro1x1`, held).
FreeBSD: `freebsd/hildon/build.sh` applies the same patches, `rootfs.sh`
makes the default factor's theme into the image.
