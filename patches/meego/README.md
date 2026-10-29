# A MeeGo-like home for Hildon

Patches that make Hildon (on Leste and on FreeBSD) look and behave like the
Nokia N9's Harmattan, step by step. Each is applied after `patches/hidpi/`
by `scripts/49-hidpi.sh` (Leste) and `freebsd/hildon/build.sh` (FreeBSD),
and each is off until switched on, so the tested Hildon stays as it is.

## Pages (`hildon-desktop.patch`)

The home views (Hildon's desktops, up to four), the launcher and the task
navigator as pages in a ring, changed by a horizontal swipe, as the N9's
Events, Applications and Open applications: past the last desktop comes
the launcher, then the task navigator (when applications are open), then
the first desktop again. The launcher and task switcher buttons in the
top left corner go.

    gconftool-2 -s -t bool /apps/osso/hildon-desktop/meego_pages true

or in Settings, Desktop (the control panel applet `apps/pro1x-look`):
Maemo or MeeGo, and on FreeBSD Xfce in Hildon's place. hildon-desktop
follows the key at once. On Arch the session decides (`pro1x-desktop
hildon|meego`, `arch/hildon/hildon-session` sets the key at its start).

A swipe counts when it is twice as far across as down and covers a third
of the screen, or a tenth within 300 ms (Harmattan's home screen,
`meegotouchhome`'s `PagedPanning`). hildon-desktop sees it in the X events
themselves, before Clutter, since the launcher's and the task navigator's
scrollers grab the pointer.

Tried in the FreeBSD VM (`freebsd/vm/try-meego-pages.sh`, swipes with
xdotool): task navigator → the four desktops → launcher → task navigator,
and back. Untested on a phone.

## Edge gestures (`hildon-desktop.patch`, with the pages)

In an application, as on the N9: a swipe from the left or right edge
inwards goes to the open applications, a swipe from the top edge down
closes it, a tap on the top edge opens its menu (as a tap on its title
does). Thin input-only windows along those three edges (1.5 % of the
screen's width, 32 px at 2160) catch them, above the application, only
while one is in front; windows mapped or restacked later get them on top
again. A swipe goes a tenth of the screen in within 1.5 s, twice as far
in as across; a tap moves less than 2 %.

Tried in the FreeBSD VM (`freebsd/vm/try-meego-edges.sh`): swipe from the
left edge → task navigator, tap on the thumbnail → the application, tap on
the top edge → its menu, tap elsewhere → closed, swipe down from the top
edge → the application closed. Untested on a phone.

## The N9's look (`hildon-desktop.patch`, `hildon-status-menu.patch`, `libhildon.patch`)

With the same key, hildon-desktop sets `_HILDON_MEEGO_LOOK` (CARDINAL 1)
on the root window, and the other parts follow that property at once (no
GConf in libhildon):

- **No title bar, a thin status bar.** Applications begin below a bar 36
  of the N9's 480 pixels high (81 px at 2160x1080): hildon-desktop makes
  the theme's north decor of applications that high (the applications are
  laid out anew, as after a change of theme), hides its title bar (but in
  Home's edit mode) and places the status area across the top;
  hildon-status-menu draws it black in one row, the signal and the other
  icons on the left, the battery and the clock on the right. No bottom
  toolbar is added. The top edge of the edge gestures is now the left half
  of the bar (tap: the application's menu, swipe down: close it); the
  right half opens the status menu.
- **The application menu as a list** below the bar, one column, as wide
  as the screen in portrait (half of it in landscape).
- **Launcher icons in the N9's shape**: the N9's own icon (its theme,
  blanco, where installed, `n9_icons` in `hd-meego.c` maps Hildon's icon
  names) or else the application's icon in the N9's rounded square, a
  superellipse with n = 2.6 as the outlines of the N9's icons are: cut to
  the shape when it (nearly) fills the square, else on a plate (light
  grey, dark grey for a light icon) at 72 %. The launcher's tiles are made
  anew when the look changes.
- **Wheels for the date and time pickers**: selectors with several
  columns show the chosen row in a band (not in the theme's colour of
  chosen rows), the rows above and below fading out; a column that stops
  rolling chooses the row in its middle and centres it, a tapped row rolls
  to the middle. Lists of one column stay Hildon's.
- **The volume bar** (`tools/pro1x-volume-osd`, Leste: `scripts/49-volume-osd.sh`,
  started from Xsession.post; FreeBSD: built by `freebsd/hildon/build.sh`,
  started by `hildon-session` after PulseAudio, which plays FreeBSD's
  sound through OSS): on a change of the default sink's volume the
  screen darkens and the volume rises from the bottom, the loudspeaker at
  the bottom, 1.5 s, as the N9's system UI draws it; a tap or a drag sets
  the volume. N9 graphics where blanco is installed, else drawn alike.

- **The status bar over the launcher and the task navigator** too, as over
  the N9's Applications and Open applications: hildon-desktop moves the
  status area's actor above them (its front group) and leaves its place to
  the status area's own window, so a tap opens the status menu there as on
  Home. Hildon shows that menu over Home; when it closes, hildon-desktop
  goes back to the launcher or the task navigator (unless a dialog came
  meanwhile).

Tried in the FreeBSD VM (`freebsd/vm/try-meego-look.sh`, with a small
picker program as `PICKER=`): Home with the bar, the launcher's icons, an X
Terminal without title bar, its menu as a list, the look off and on again
with the terminal open, the time picker rolled by a drag, the date picker;
the volume bar with a null sink and `pactl`. And with
`freebsd/vm/try-meego-events.sh`: the volume bar started by FreeBSD's
hildon-session, the status bar over the launcher and the task navigator,
the status menu from either and back. Untested on a phone.

Next: the Events page.
