# The image loaders' list is built at boot (rc.d/pro1x_setup): the root is
# read only.
export GDK_PIXBUF_MODULE_FILE=/var/db/gdk-pixbuf-loaders.cache
# scfb has no DPMS: black after 3 minutes, which the AMOLED panel shows
# with its pixels off
xset s 180 0
xset s blank

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
