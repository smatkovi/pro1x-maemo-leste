# The FreeBSD packages Hildon builds on and runs with (freebsd/hildon/build.sh
# for the sysroot, rootfs.sh for the root image)
HILDON_PKGS="gtk2 gconf2 dbus dbus-glib gnome-menus libcanberra libcanberra-gtk2
	gdk-pixbuf-xlib startup-notification mesa-libs mesa-dri libglvnd png
	libSM libXcomposite libXdamage libXfixes libXi libXt libXrandr libXft
	sqlite3 libxml2 pango glib libinotify xorg-server libexif xrdb"
# Not needed to build against
HILDON_PRUNE="llvm* spirv-llvm-translator* python3* py3*-* perl5* samba* qt6-*"
