# The image loaders' list is built at boot (pro1x_caches): the root is
# read only.
export GDK_PIXBUF_MODULE_FILE=/var/db/gdk-pixbuf-loaders.cache
exec startxfce4
