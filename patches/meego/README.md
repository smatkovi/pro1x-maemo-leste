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

Next: apps without title bars, a thin status bar, the Events page.
