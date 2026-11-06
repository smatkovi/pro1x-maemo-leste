#!/bin/sh
# try-wifi-status.sh [stop]: in the FreeBSD test VM (vm.sh), as root: the
# status menu's Wi-Fi item (pro1x-wifi-status) against a made-up
# wpa_supplicant (fake-wpa-supplicant.py next to this script, as
# /root/fake-wpa-supplicant.py), Hildon at 2160x1080 on a virtual screen
# as try-hildon.sh starts it. The session stays up to be tapped with
# xdotool and photographed with xwd (DISPLAY=:1); the status area is at
# 394,50, the menu's Internet item at 1448,80. What the plugin asked for:
# /tmp/fakewpa.log. "stop" ends it all and puts the status menu back.
#
# The plugin from its package, or from SRC= (a copy of the build's
# $WORK/hildon/pkgroot/pro1x-wifi-status) when the package is not
# installed. libhildondesktop's example plugins are moved aside meanwhile,
# as the phone's image has none.
set -u
P=/usr/local/etc/hildon-desktop/status-menu.plugins
A=/usr/local/share/applications/hildon-status-menu
SO=/usr/local/lib/hildon-desktop/pro1x-wifi-status.so
KEEP=/root/wifitest
if [ "${1:-}" = stop ]; then
	for p in $(ps -axo pid=); do
		procstat -e "$p" 2> /dev/null | grep -q PRO1X_TRY=1 && kill "$p" 2> /dev/null
	done
	[ -f $KEEP/fake.pid ] && kill "$(cat $KEEP/fake.pid)" 2> /dev/null
	[ -f $KEEP/status-menu.plugins ] && cp $KEEP/status-menu.plugins $P
	mv $KEEP/example-*.desktop $A/ 2> /dev/null
	# what this script put there, not a package's
	[ -f $KEEP/copied ] && rm -f $SO $A/pro1x-wifi-status.desktop
	rm -rf /var/run/wpa_supplicant $KEEP
	echo stopped
	exit 0
fi
mkdir -p $KEEP /root/shots
cp $P $KEEP/
mv $A/example-*.desktop $KEEP/ 2> /dev/null
if [ ! -f $SO ]; then
	SRC=${SRC:?the package is not installed: SRC=<copy of pkgroot/pro1x-wifi-status>}
	cp -R "$SRC/usr/local/." /usr/local/
	touch $KEEP/copied
fi
grep -q '^\[pro1x-wifi-status.desktop\]' $P ||
    printf '\n[pro1x-wifi-status.desktop]\nX-Desktop-File=%s\nX-Status-Area-Position=1\nX-Status-Menu-Position=5\n' \
    $A/pro1x-wifi-status.desktop >> $P
service dbus onestatus > /dev/null 2>&1 || service dbus onestart
rm -f /tmp/fakewpa.log
daemon -f -p $KEEP/fake.pid sh -c 'exec python3.11 /root/fake-wpa-supplicant.py > /tmp/fakewpa.out 2>&1'
export DISPLAY=:1 PRO1X_TRY=1
daemon -f sh -c 'exec Xvfb :1 -screen 0 2160x1080x24 -noreset -nolisten tcp > /tmp/xvfb.log 2>&1'
sleep 3
/usr/local/bin/pro1x-hidpi theme
daemon -f -o /tmp/session.log /usr/local/bin/hildon-session
echo "started: the session needs about a minute"
