#!/bin/bash
# Leste's mce, able to close Harmattan's lock screen.
#
# Runs as root on Leste (the phone or the rootfs chroot). Needs network.
#
# In the MeeGo boot Harmattan's sysuid owns com.nokia.system_ui. Leste's mce
# closes the lock screen with Fremantle's tklock_close(s,s,s,s,b), sysuid
# only has tklock_close(b): the call fails, mce retries it every 500 ms
# for good and stays locked. patches/mce-tklock-close-harmattan.patch falls
# back to the one-argument call. The package gets a +pro1x1 version and is
# held, so an apt upgrade does not put the unpatched mce back.
set -euo pipefail
MEEGO=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC:-/root/src}
MCE_TAG=1.10.15
MCE_VERSION=1.10.15+4m7.2+pro1x1   # Leste's 1.10.15+4m7.2, patched
export DEBIAN_FRONTEND=noninteractive

apt-get install -y -q --no-install-recommends git build-essential devscripts equivs

mkdir -p "$SRC"
[ -d "$SRC/mce/.git" ] || git clone -q https://github.com/maemo-leste/mce.git "$SRC/mce"
cd "$SRC/mce"
git fetch -q --tags origin
git checkout -q -f "$MCE_TAG"
git clean -fdq
git apply "$MEEGO/patches/mce-tklock-close-harmattan.patch"
DEBEMAIL="pro1x-maemo-leste" dch -v "$MCE_VERSION" -D unstable \
  "Close a Harmattan system UI's lock screen too (F(x)tec Pro1X MeeGo boot)."
# mce builds against libcal: next to 47-libcal.sh's patched libcal1 only
# the libcal-dev of the same build fits.
if ls "$SRC"/libcal-dev_*+pro1x1_*.deb > /dev/null 2>&1; then
	apt-get install -y -q "$SRC"/libcal-dev_*+pro1x1_*.deb
fi
mk-build-deps -i -r -t "apt-get -y -q --no-install-recommends" debian/control
dpkg-buildpackage -b -us -uc

# Restarting mce leaves the display off: the new one runs after a reboot.
echo exit 101 > /usr/sbin/policy-rc.d
chmod 755 /usr/sbin/policy-rc.d
apt-get install -y -q "$SRC/mce_${MCE_VERSION}_"*.deb || { rm -f /usr/sbin/policy-rc.d; exit 1; }
rm -f /usr/sbin/policy-rc.d
apt-mark hold mce
echo ">> done, reboot to start the new mce"
