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

## The Events page (`tools/pro1x-events`, `apps/pro1x-events`, `hildon-desktop.patch`)

The first home view becomes the N9's Events page while the look is on:
below the status bar the day and the date, then the notifications (the
missed calls, SMS and chats that rtcom-eventlogger has unread, one entry
per caller or sender; the unread e-mail of each modest account, asked over
D-Bus only while modest runs) with Clear, which marks them read, then the
home timeline of a Mastodon account (`GET /api/v1/timelines/home` with the
account's access token, in a thread with libcurl, the avatars cached in
`~/.cache/pro1x-events`), with the posts' pictures (below). The list scrolls up and down with a flick; pulled
down at its top it updates (as the N9's feeds, "Pull down to update",
"Release to update"), else every 15 minutes while it is seen. A tap opens
the post in the browser, a notification its application (through
hildon-desktop's launcher, `LaunchApplication`), the hint without an account
Settings, Events. Sizes and colours are those of the N9's Events view (its
home screen's style), the N9's font and icons where they are installed (not
part of this repository).

Since pro1x-events 1.1 the page also has, each section switched on or off
in Settings, Events (GConf `/apps/pro1x/events/show_weather`,
`show_notifications`, `show_calendar`, `show_mastodon`):

- **The weather** right below the date: now (temperature, the conditions,
  feels like, wind) and today with the next four days (their icon, highest
  and lowest), beside it where the screen is wide enough, else below. From
  [Open-Meteo](https://open-meteo.com/)'s forecast API (`current=` and
  `daily=`, `timezone=auto`): no key, free for non-commercial use, its data
  under CC BY 4.0, which asks for the credit "Weather data by Open-Meteo.com"
  next to it (it is the item's last line; a tap on it opens the site, a tap
  elsewhere nomweather or MeeCast). Every 30 minutes while the page is seen
  (5 after a failed try) and when it is pulled down; the last answer is kept
  in `~/.cache/pro1x-events/weather-<lat>_<lon>.json` and shown, "not
  updated", without a connection. MeeCast's own data is not used: its
  sources need keys or have changed their APIs (see
  `freebsd/hildon/README.md`); nomweather, Leste's weather app, uses
  Open-Meteo as well. The place: `weather_place`, `weather_latitude`,
  `weather_longitude`, set in the applet by Search (Open-Meteo's geocoding,
  GeoNames' places, a list to pick from) or typed in; `weather_url` and
  `geocoding_url` take another Open-Meteo server (it is free software; the
  VM test serves its fixture there). The icons: MeeCast's icon set
  (`weather_icons`, default `Meecast`, GPL, Andrew Zhilin's) where MeeCast
  is installed, looked up in its directories and mapped from the WMO weather
  codes to its weather.com numbers; without it (or with `none`) plain glyphs
  drawn by the page, white outlines on black like that set. Neither MeeCast's
  nor Nokia's graphics are in this repository.
- **The calendar**: the appointments of today and the next days
  (`calendar_days`, 1, 3, 7 or 14 in the applet, default 7, at most 12),
  those already over left out, from calendar-backend, the library of
  Hildon's calendar qalendar, through its C++ API as qalendar's agenda reads
  them (`tools/pro1x-events/calendar.cpp`: each visible calendar's
  `getAllEventsTodos()`, the repeating ones expanded with
  `getInstanceTimes()`, open tasks, birthdays with the age). Each shows its
  day on a plate of its calendar's colour, the title, the place and when
  ("Today, 14:00–15:30", "Now, until 15:30", "Tomorrow, all day"); it is
  updated on calendar-backend's `dbChange` signal and when an appointment
  begins or ends. A tap opens it in qalendar (`com.nokia.calendar`
  `launch_view`; qalendar's agenda for the hint without appointments).
  calendar-backend expanded no repeating appointment on 64-bit systems:
  `patches/fixes/calendar-backend.patch` (Leste, in `49-hidpi.sh`'s parts)
  and `freebsd/hildon/patches/calendar-backend/0003` fix that, FreeBSD's
  `0004` lets its SQL's double-quoted strings through FreeBSD's SQLite
  (built without them); pro1x-events allows them on its own connection
  too, for a calendar-backend without that patch.

- `pro1x-events` is a Home widget of its own (a `_HILDON_WM_WINDOW_TYPE_HOME_APPLET`
  window, as Hildon's operator name is), started by the X session (Leste:
  `Xsession.post/47pro1x-events`, FreeBSD: `hildon-session`), shown while
  the root window has `_HILDON_MEEGO_LOOK`; it puts itself on the first
  view right below the bar, as wide as the screen. Closed in Home's edit
  mode, it comes back when the look is switched on again.
- hildon-desktop passes Home widgets only taps (it takes their drags to pan
  Home); a widget with `_HILDON_MEEGO_SCROLLING_APPLET` (CARDINAL 1) gets the
  drags up and down as motion and release events, with their times, while
  the drags across still change the page and a long press still edits
  Home. Widgets may begin right below the bar (else below Hildon's title
  bar).
- Settings, Events (`apps/pro1x-events`, a control panel applet): the
  Mastodon server and the access token (from the account's web page:
  Preferences, Development, New application with the scope
  `read:statuses`, then "Your access token"). Both are kept in GConf,
  `/apps/pro1x/events/mastodon_server` and `mastodon_token`, in plain text
  as Hildon keeps its other settings; empty fields remove the account.

Leste: `scripts/49-events.sh` (after `49-hidpi.sh` with this patch and the
calendar-backend fix), or the package of `scripts/mkdeb-tools.sh`. FreeBSD:
`freebsd/hildon/build.sh pro1x-events`, package `pro1x-events`.

Tried in the FreeBSD VM (`freebsd/vm/try-meego-events.sh EVENTS_FIXTURE=1`,
with a made-up timeline served on the VM itself and made-up unread calls
and messages, `freebsd/vm/events-fixture.sh`): the page with the
notifications and the feed, scrolled by drags, pulled down to update, the
swipes across to the second view, back, to the launcher and back, Clear,
the hint without an account opening Settings, Events, a token saved there
and the feed fetched with it. Not tried: a real Mastodon server, modest
with unread mail, a phone. (Portrait: below.)

The weather and the calendar (1.1) in the VM, `STEPS=step4` with
`EVENTS_FIXTURE=1` (a made-up answer in Open-Meteo's format, three places
called Wien for the search, and made-up appointments in calendar-backend's
database: one over, one now, one later, an all-day one, a weekly one, a
task and a birthday in three calendars), in German with
`LANG=de_DE.UTF-8`: the weather beside the date with MeeCast's icons and
with the page's own, the appointments (the weekly one only with the
calendar-backend fix), single sections off, no place, no connection (the
last answer), the applet's search with its list and the place taken, the
calendar's days, a tap opening qalendar (in the VM its month view: the
appointment's own view needs qalendar to answer within the call's minute,
which it did not in the emulated VM), and the real Open-Meteo for Vienna
from the VM. Not tried: a phone.

### The posts' pictures (pro1x-events 1.2)

The N9 showed pictures in its Events feeds (Twitter, Facebook, the RSS
reader, AP News): its home screen's style (`meegotouchhome.css` of the N9's
firmware, PR1.3) has `SwipeEventFeedItemStyle` with `maximum-images: 3`,
`maximum-video-images: 1` and a play button over a video
(`video-overlay-image: icon-l-common-video-playback`, 64 x 64 at 40 %), and
image styles `EventFeedImageLarge` (168 x 168 of its pixels),
`EventFeedImageSmall` (120 x 120) and `EventFeedImageVideo` (210 x 120), 1.6
mm above them and 0.8 mm right of each; its event feed API
(libmeegotouchevents, `MEventFeed::addItem`) takes an `imageList` and a
`video` flag ("limited to one thumbnail ... a play button overlay rendered
on top of it"), and the N9's own Twitter and Facebook clients pass their
posts' pictures. pro1x-events shows a Mastodon post's attachments so:
their previews (`preview_url`; of the boosted post for a boost) in a row
in the text column below the text, above the time; up to 3 images, 168
pixels square for one or two, 120 for three (three of them and their gaps
fill the N9's text column, 384 of its pixels: inferred, the rule itself is
in Nokia's closed home screen), cropped to squares as the N9's thumbnails
(its "grid" flavour, "cropped"); a video or GIF as one still in 210 x 120,
not cropped, under the N9's play button where its theme is installed, else
one drawn alike. A tap opens the post, as a tap anywhere on an item (the
N9 had no action of its own for a picture). A post marked sensitive gets
grey plates, "Sensitive content", instead of its pictures (which are not
fetched; the N9 had no such mark). The previews are cached in
`~/.cache/pro1x-events` (`media-*`) while posts show them, for two days
after. One older text of Nokia's (the Harmattan developer library's guide
to the event feed, of an earlier libmeegotouchevents) says the images show
when an item is expanded by a tap; the N9's own home screen (PR1.3) has no
such expanding and shows them in the list.

## Portrait

Leste's hildon-desktop turns the screen to portrait when mce reports the
phone upright (its accelerometer, through iio-sensor-proxy on the Pro1X),
while the keyboard is slid in and `/apps/osso/hildon-desktop/ui_can_rotate`
is on (the default). The N9 was made for portrait, and with the look the
pages work there as on it (`hildon-desktop.patch`, hildon-desktop 1:2.2.196+pro1x5,
FreeBSD hildon-desktop_6):

- The pages stay side by side, changed by a swipe across, and the Events
  page scrolls up and down: Leste's own portrait home, its views one below
  the other (GConf `scroll_vertical`), which took the drags up and down
  from the page, is not used while the look is on (it is again when it is
  off). A swipe on Home changes the page after a quarter of the screen's
  width in either orientation.
- The status bar lies across the top, 81 px high, as in landscape.
- The launcher has the N9's buttons (`SwipeLauncherButtonStyle`, 11.2 mm
  wide; `LauncherPageFlowLayoutPolicy`: 0.2 mm apart, rows 3 mm apart;
  `SwipeLauncherPannedWidget`: 1.3 mm free at the sides in portrait, 2.9 mm
  in landscape): as many in a row as fit, 4 in portrait and 7 in landscape,
  as on the N9, in either orientation.
- The open applications are two columns of thumbnails from the top left
  below the status bar, as the N9's (`SwitcherDetailviewPolicy`: 1.4 mm
  free at the sides, 2.4 mm at the top, 2.0 mm apart in portrait; 3, 3 and
  2.4 mm in landscape), as wide as that leaves and of their windows' shape,
  in landscape too. Hildon's title on each thumbnail, with its close
  button, stays.
- The Events page lays itself out for the narrower screen: the weather's
  days below the weather now, the pictures in the same sizes in both.
- Applications that can turn (their `_HILDON_PORTRAIT_MODE_SUPPORT`) begin
  below the bar in portrait; the edges' swipes and the menu as a list (as
  wide as the screen) work there as in landscape.

Tried in the FreeBSD VM (`freebsd/vm/try-meego-portrait.sh`): Xvfb cannot
change its screen's size, so Xorg's dummy driver with a landscape and a
portrait mode of the phone's size, and `fake-rotation.c` preloaded in the
session, which turns the rotation hildon-desktop asks for into a change of
mode (the screen is then 1080 x 2160, as on the phone after the turn; the
screenshots are upright); mce's orientation signal sent with `dbus-send`
(`session.sh rotate portrait|landscape`), and `scroll_vertical` on: the
Events page in landscape and in portrait, scrolled to the feed's pictures,
a swipe to the launcher and back, two X Terminals, one in portrait below
the bar, its menu, the left edge's swipe to the open applications, the
status menu from there, back to landscape: the open applications, the
launcher and the Events page. FreeBSD itself has no sensors on the Pro1X,
so there it stays in landscape. Not tried: a phone.
