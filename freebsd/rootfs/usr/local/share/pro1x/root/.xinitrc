# The image loaders' list is built at boot (rc.d/pro1x_setup): the root is
# read only.
export GDK_PIXBUF_MODULE_FILE=/var/db/gdk-pixbuf-loaders.cache
# The Nokia N9's stand-by screen (pro1x-standby): as on the N9, after the
# display time-out without input (30 s, pro1x.display_timeout in
# freebsd/cmdline; the N9 offered 15, 30, 60, 120, 180) and 3 s more, or
# at once on a short press of the power key, the time and the
# notifications on black; a double tap or the power key bring the desktop
# back. 403 dpi: the Pro1X, for the N9's sizes in mm.
# The display off after 2 minutes (the decision of 2026-10-31, on every
# system): pro1x.standby_off=N, the panel switched off N minutes after the
# last input, is 2 by default (the stand-by screen comes after the time-out
# and 3 s, the panel goes off the rest of the N minutes later: -O in
# seconds); 0: at once in the stand-by screen's place, never: the N9's
# stand-by screen for good. pro1x-standby also says what the display does
# as mce's display_status_ind (no mce runs here), so that hildon-desktop
# and the clients stop drawing while the panel is off. With the panel off the touchscreen sleeps (no double tap) and the
# SoC can sleep; the power key brings the desktop back. pro1x.standby=off:
# no stand-by screen, the panel off after 2 minutes (pro1x.display_timeout
# then counts to the panel off); without pro1x-standby (or with
# pro1x.standby=off pro1x.standby_off=never): X's screen saver, black after
# 2 minutes (scfb has no DPMS; the AMOLED panel shows black with its pixels
# off).
o=$(kenv -q pro1x.standby_off)
: "${o:=2}"
[ "$o" != never ] || o=
if [ -x /usr/local/bin/pro1x-standby ] && [ "$(kenv -q pro1x.standby)" != off ]; then
	xset s off
	t=$(kenv -q pro1x.display_timeout)
	# the panel's partial mode around the content: X is turned clockwise
	# on the portrait panel (xorg.conf.d/10-pro1x.conf);
	# pro1x.standby_partial=0 drives all of it
	r="-R cw"
	[ "$(kenv -q pro1x.standby_partial)" = 0 ] && r=
	# N minutes after the last input: the stand-by screen's seconds
	case $o in
	'' | 0) ;;
	*[!0-9]*) o= ;;
	*) o=$((o * 60 - ${t:-30} - 3)); [ "$o" -gt 0 ] || o=0; o=${o}s ;;
	esac
	# shellcheck disable=SC2086
	/usr/local/bin/pro1x-standby -d 403 -s "${t:-30}" $r ${o:+-O "$o"} &
elif [ -x /usr/local/bin/pro1x-standby ] && [ -n "$o" ]; then
	# no stand-by screen, the panel off after 2 minutes (or the time-out;
	# pro1x-standby blanks 3 s after it)
	xset s off
	t=$(kenv -q pro1x.display_timeout)
	/usr/local/bin/pro1x-standby -d 403 -s "${t:-117}" -O 0 &
else
	xset s 120 0
	xset s blank
fi

# Xfce, Hildon or the N9's user interface: `startx hildon` /
# `startx harmattan` / `startx xfce`, else pro1x.desktop=hildon (or
# harmattan) in freebsd/cmdline, else Xfce. pro1x-desktop switches between
# them: it names the next one and ends the session, X stays.
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
	harmattan)
		if [ ! -x /usr/local/bin/harmattan-session ]; then
			desktop=xfce
			continue
		fi
		echo harmattan > /tmp/.pro1x-desktop
		/usr/local/bin/harmattan-session
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
