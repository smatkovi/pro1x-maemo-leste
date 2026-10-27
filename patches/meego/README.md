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

Next: swipes from the screen edges in applications (to the pages, to
close), apps without title bars, a thin status bar, the Events page.
