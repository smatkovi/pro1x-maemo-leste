# The image loaders' list is built at boot (rc.d/pro1x_setup): the root is
# read only.
export GDK_PIXBUF_MODULE_FILE=/var/db/gdk-pixbuf-loaders.cache
# The Nokia N9's stand-by screen (pro1x-standby): as on the N9, after the
# display time-out without input (30 s, pro1x.display_timeout in
# freebsd/cmdline; the N9 offered 15, 30, 60, 120, 180) and 3 s more, or
# at once on a short press of the power key, the time and the
# notifications on black; a double tap or the power key bring the desktop
# back. 403 dpi: the Pro1X, for the N9's sizes in mm. pro1x.standby=off,
# or without pro1x-standby: X's screen saver, black after 3 minutes (scfb
# has no DPMS). The AMOLED panel shows black with its pixels off.
if [ -x /usr/local/bin/pro1x-standby ] && [ "$(kenv -q pro1x.standby)" != off ]; then
	xset s off
	t=$(kenv -q pro1x.display_timeout)
	/usr/local/bin/pro1x-standby -d 403 -s "${t:-30}" &
else
	xset s 180 0
	xset s blank
fi

# Xfce or Hildon: `startx hildon` / `startx xfce`, else pro1x.desktop=hildon
# in freebsd/cmdline, else Xfce. pro1x-desktop switches between them: it
# names the next one and ends the session, X stays.
NEXT=/tmp/.pro1x-desktop-next
desktop=${1:-$(kenv -q pro1x.desktop)}
while :; do
	rm -f "$NEXT"
	case $desktop in
	hildon)
		if [ ! -x /usr/local/bin/hildon-session ]; then
			desktop=xfce
			continue
		fi
		echo hildon > /tmp/.pro1x-desktop
		/usr/local/bin/hildon-session
		;;
	*)
		echo xfce > /tmp/.pro1x-desktop
		# What Hildon's session left in the server: its font resolution
		# (Xfce sets its own) and the keys it took the repeat from
		xrdb -nocpp -remove 2> /dev/null
		xset r 36 r 70 r 72 r 9 r 104 2> /dev/null
		startxfce4
		;;
	esac
	[ -s "$NEXT" ] || break
	desktop=$(cat "$NEXT")
done
rm -f /tmp/.pro1x-desktop "$NEXT"
