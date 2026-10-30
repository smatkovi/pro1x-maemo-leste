#!/bin/bash
# pro1x-waydroid, the preparation for Waydroid on Maemo Leste (Pro1X), as
# an architecture-independent .deb from tools/pro1x-waydroid
# (docs/waydroid.md):
#
#   /etc/init.d/waydroid-container    OpenRC service: Waydroid's container
#                                     manager, the modules it needs; at boot
#   /usr/bin/pro1x-waydroid           Android as one Hildon window (weston's
#                                     X11 backend), GPU/software switch,
#                                     status, check
#   /usr/share/applications/hildon/   the menu entry "Android" and its icon
#   /etc/apt/sources.list.d/          Waydroid's repository (postinst), its
#   /usr/share/keyrings/                key
#   /etc/dnsmasq.d/pro1x-waydroid     Leste's dnsmasq off Waydroid's bridge
#   /etc/apt/preferences.d/           lxc without its container templates,
#                                     lxcfs, libpam-cgfs
#   /usr/share/dbus-1/system-services/id.waydro.Container.service
#                                     (Waydroid's diverted) D-Bus activation
#                                     through the OpenRC service
#
# Needs dpkg-deb, gzip, md5sum (a Leste or Debian system, or the chroot of
# scripts/leste-chroot.sh); gpg, where present, checks the key's
# fingerprint. Version: VERSION below; raise it when the sources change.
#
#   OUT=/dir   where the package goes (default $SRC/debs)
#   COMMIT=, SOURCE_DATE_EPOCH=  the repository's commit and its time, for
#              the changelog and the files' dates (default: git, or now)
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
SRC=${SRC:-/root/src}
OUT=${OUT:-$SRC/debs}
COMMIT=${COMMIT:-$(git -C "$REPO" rev-parse --short HEAD 2> /dev/null || echo unknown)}
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git -C "$REPO" log -1 --format=%ct 2> /dev/null || date +%s)}
export SOURCE_DATE_EPOCH
MAINTAINER="Sebastian Matkovich <sebastian.matkovich@gmail.com>"
HOMEPAGE=https://github.com/smatkovi/pro1x-maemo-leste
NAME=pro1x-waydroid
VERSION=1.1
T=$REPO/tools/pro1x-waydroid
# Waydroid's repository key, https://repo.waydro.id/waydroid.gpg
# (2026-10-30): rsa3072 of 2023-08-03, "Czarek Nakamoto <ci@mrcyjanek.net>",
# the same on keyserver.ubuntu.com and keys.openpgp.org (docs/waydroid.md)
KEY_SHA256=71fe05d735c812e15fe229bf10106b02b62561be8aa5280d63a58e25a5c0c5e2
KEY_FPR=7CE0331F71E0A238BB1002D70E406D181DCEE19C
log() { echo ">> $*"; }

log "checking the sources"
echo "$KEY_SHA256  $T/waydroid-archive-keyring.gpg" | sha256sum -c --quiet
if command -v gpg > /dev/null; then
  g=$(mktemp -d)
  fpr=$(gpg --homedir "$g" --batch --with-colons --show-keys "$T/waydroid-archive-keyring.gpg" 2> /dev/null |
    awk -F: '$1 == "fpr" { print $10; exit }')
  rm -rf "$g"
  [ "$fpr" = "$KEY_FPR" ] || { echo "!! key fingerprint $fpr, $KEY_FPR wanted"; exit 1; }
fi
for f in pro1x-waydroid waydroid-container.initd dbus-activate deb/preinst deb/postinst deb/prerm deb/postrm; do
  sh -n "$T/$f"
done
python3 -I -c 'import ast, sys; ast.parse(open(sys.argv[1]).read())' "$T/pro1x-waydroid-x11"

d=$SRC/pkg/$NAME
rm -rf "$d"
log "$NAME $VERSION"
install -D -m 755 "$T/pro1x-waydroid" "$d/usr/bin/pro1x-waydroid"
install -D -m 755 "$T/pro1x-waydroid-x11" "$d/usr/lib/$NAME/pro1x-waydroid-x11"
install -D -m 755 "$T/dbus-activate" "$d/usr/lib/$NAME/dbus-activate"
install -D -m 644 "$T/weston.ini" "$d/usr/lib/$NAME/weston.ini"
install -D -m 755 "$T/waydroid-container.initd" "$d/etc/init.d/waydroid-container"
install -D -m 644 "$T/pro1x-waydroid.default" "$d/etc/default/pro1x-waydroid"
install -D -m 644 "$T/dnsmasq.conf" "$d/etc/dnsmasq.d/pro1x-waydroid"
install -D -m 644 "$T/apt-preferences" "$d/etc/apt/preferences.d/pro1x-waydroid"
install -D -m 644 "$T/waydroid-archive-keyring.gpg" "$d/usr/share/keyrings/pro1x-waydroid.gpg"
install -D -m 644 "$T/id.waydro.Container.service" \
  "$d/usr/share/dbus-1/system-services/id.waydro.Container.service"
install -D -m 644 "$T/pro1x-waydroid.desktop" "$d/usr/share/applications/hildon/pro1x-waydroid.desktop"
install -D -m 644 "$T/pro1x-waydroid.svg" "$d/usr/share/icons/hicolor/scalable/apps/pro1x-waydroid.svg"
install -D -m 644 "$T/pro1x-waydroid.png" "$d/usr/share/icons/hicolor/128x128/apps/pro1x-waydroid.png"
install -D -m 644 "$REPO/docs/waydroid.md" "$d/usr/share/doc/$NAME/waydroid.md"

cat > "$d/usr/share/doc/$NAME/copyright" <<EOF
$NAME, built from $HOMEPAGE
at commit $COMMIT by scripts/mkdeb-waydroid.sh.

Copyright 2026 $MAINTAINER

The repository states no licence.

/usr/share/keyrings/pro1x-waydroid.gpg is the public key of Waydroid's
package repository (https://repo.waydro.id/waydroid.gpg), fingerprint
7CE0 331F 71E0 A238 BB10 02D7 0E40 6D18 1DCE E19C.
EOF
printf '%s (%s) unstable; urgency=medium\n\n  * Built from %s at %s.\n\n -- %s  %s\n' \
  "$NAME" "$VERSION" "$HOMEPAGE" "$COMMIT" "$MAINTAINER" \
  "$(date -R -u -d "@$SOURCE_DATE_EPOCH")" | gzip -9n > "$d/usr/share/doc/$NAME/changelog.gz"
chmod 644 "$d/usr/share/doc/$NAME/"*

install -d -m 755 "$d/DEBIAN"
for s in preinst postinst prerm postrm; do
  install -m 755 "$T/deb/$s" "$d/DEBIAN/$s"
done
# lxc-net out of the runlevel when lxc brings it (postinst, triggered)
install -m 644 "$T/deb/triggers" "$d/DEBIAN/triggers"
{
  echo "Package: $NAME"
  echo "Version: $VERSION"
  echo "Architecture: all"
  echo "Maintainer: $MAINTAINER"
  echo "Installed-Size: $(du -sk --exclude=DEBIAN "$d" | cut -f1)"
  # not lxc and iptables: Waydroid depends on them, and installed with it
  # (after this package) lxc comes without the recommended packages the
  # preferences file holds back. dnsmasq-base: waydroid-net.sh runs dnsmasq,
  # which Waydroid's package does not declare.
  echo "Depends: dnsmasq-base, iproute2, kmod, weston, x11-xserver-utils, python3, libx11-6, dbus, openrc"
  echo "Recommends: pulseaudio-utils"
  echo "Suggests: waydroid"
  echo "Section: misc"
  echo "Priority: optional"
  echo "Homepage: $HOMEPAGE"
  echo "Description: Waydroid preparation for Maemo Leste on the F(x)tec Pro1X"
  printf '%s\n' "Everything Waydroid needs on Leste (Devuan, OpenRC, Hildon on X11) before 'apt install waydroid' and 'waydroid init': an OpenRC service for Waydroid's container manager (what upstream's systemd unit does) with the kernel modules of its network, enabled at boot and idle until Waydroid is installed; Waydroid's own apt repository (repo.waydro.id) with its key, for the Debian release under the Devuan one, and lxc without its container templates, lxcfs and libpam-cgfs (when Waydroid brings lxc); weston and the network tools; Leste's dnsmasq kept off Waydroid's bridge; lxc's own bridge service lxc-net off; D-Bus activation of the container manager through the service. The menu entry Android starts a nested Wayland compositor (weston, X11 backend) as one Hildon window and Waydroid's full UI in it, at Hildon's scale; closing the window stops Android. pro1x-waydroid switches between GPU (Mesa freedreno) and software rendering and checks the system (pro1x-waydroid check)." |
    fold -s -w 72 | sed 's/ *$//; s/^/ /'
} > "$d/DEBIAN/control"
(cd "$d" && find etc -type f | sort | sed 's|^|/|') > "$d/DEBIAN/conffiles"
(cd "$d" && find . -type f ! -path './DEBIAN/*' -printf '%P\0' | sort -z |
  xargs -0 md5sum) > "$d/DEBIAN/md5sums"
chmod 644 "$d/DEBIAN/control" "$d/DEBIAN/conffiles" "$d/DEBIAN/md5sums" "$d/DEBIAN/triggers"
find "$d" -type d -exec chmod 755 {} +
find "$d" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
mkdir -p "$OUT"
dpkg-deb --root-owner-group -Zxz --build "$d" "$OUT/${NAME}_${VERSION}_all.deb"
ls -l "$OUT/${NAME}_${VERSION}_all.deb"
