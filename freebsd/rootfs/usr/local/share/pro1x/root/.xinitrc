# The image loaders' list is built at boot (rc.d/pro1x_setup): the root is
# read only.
export GDK_PIXBUF_MODULE_FILE=/var/db/gdk-pixbuf-loaders.cache
# scfb has no DPMS: black after 3 minutes, which the AMOLED panel shows
# with its pixels off
xset s 180 0
xset s blank
# Hildon instead of Xfce: pro1x.desktop=hildon in freebsd/cmdline
if [ "$(kenv -q pro1x.desktop)" = hildon ] && [ -x /usr/local/bin/hildon-session ]; then
	exec /usr/local/bin/hildon-session
fi
exec startxfce4
