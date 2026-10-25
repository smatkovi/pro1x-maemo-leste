# headphoned

Pauses the media players when the headphones are unplugged: a port of
Thomas Perl's headphoned for Maemo 5 (1.9, GPL-2.0-or-later) to Maemo Leste.

The N900 version woke on a POLLPRI of the gpio-switch sysfs file. This one
sleeps in poll() on the kernel's jack input device (SW_HEADPHONE_INSERT; on
the Pro1X "F(x)tec Pro1X Headset Jack") and costs nothing until the plug is
pulled. It then pauses every MPRIS2 player on the session bus and Leste's
MAFW renderer (the Media Player), and like the original Panucci, FM Radio and
MediaBox; during a call it waits until the call has ended.

`make install` installs the daemon (/usr/local/bin), its start in the
session (/etc/X11/Xsession.post/45headphoned) and a udev rule that gives
the jack device to the group audio.
