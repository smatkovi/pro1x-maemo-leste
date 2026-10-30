#!/bin/bash
# The own tools of Hildon's MeeGo-like look as Maemo Leste packages
# (arm64 .debs), from this repository's sources as 49-events.sh and
# 49-volume-osd.sh build them:
#
#   pro1x-events      the Events page (tools/pro1x-events), its control
#                     panel applet (Settings, Events: apps/pro1x-events),
#                     Xsession.post/47pro1x-events
#   pro1x-volume-osd  the N9's volume bar (tools/pro1x-volume-osd),
#                     Xsession.post/46pro1x-volume-osd
#
# Depends: the libraries they link (dpkg-shlibdeps) and the hildon-desktop
# with patches/meego they need (49-hidpi.sh's +pro1xN). Neither has GConf
# defaults: pro1x-events writes its widget's keys itself when it starts.
# Version: VERSIONS below; raise it when a tool's sources change.
#
# Runs as root INSIDE the target (a chroot of the image, as
# 41-leste-debs-chroot.sh sets one up, or the running phone), after
# 49-hidpi.sh: the applet builds against its libhildon1-dev.
#
#   OUT=/dir   where the packages go (default $SRC/debs)
#   ONLY="pro1x-events"  build only these
#   COMMIT=, SOURCE_DATE_EPOCH=  the repository's commit and its time, for
#              the changelog and the files' dates (default: git, or now)
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
SRC=${SRC:-/root/src}
OUT=${OUT:-$SRC/debs}
ONLY=${ONLY:-pro1x-events pro1x-volume-osd}
COMMIT=${COMMIT:-$(git -C "$REPO" rev-parse --short HEAD 2> /dev/null || echo unknown)}
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git -C "$REPO" log -1 --format=%ct 2> /dev/null || date +%s)}
export SOURCE_DATE_EPOCH
export DEBIAN_FRONTEND=noninteractive
MAINTAINER="Sebastian Matkovich <sebastian.matkovich@gmail.com>"
HOMEPAGE=https://github.com/smatkovi/pro1x-maemo-leste
ARCH=$(dpkg --print-architecture)
declare -A VERSIONS=([pro1x-events]=1.1 [pro1x-volume-osd]=1.0)
log() { echo ">> $*"; }

# Build dependencies (49-events.sh's and 49-volume-osd.sh's). The -dev
# packages of what 49-hidpi.sh rebuilt (+pro1xN, held) from that build,
# where they are missing: the archive's want the archive's libraries
# exactly
pkgs=(gcc g++ libc6-dev pkg-config make gettext binutils dpkg-dev libgconf2-dev
  libjson-glib-dev librtcom-eventlogger-dev hildon-control-panel-dev libosso-dev
  libx11-dev libpulse-dev libtime-dev libical-dev
  libsqlite3-dev libdbus-1-dev)
pkg-config --exists libcurl 2> /dev/null || pkgs+=(libcurl4-openssl-dev)
debs=()
for p in libgtk2.0-dev libhildon1-dev calendar-backend-dev; do
  dpkg-query -W -f='${Status}' "$p" 2> /dev/null | grep -q "ok installed" && continue
  f=$(ls "$SRC"/"$p"_*+pro1x*_*.deb 2> /dev/null | tail -1 || true)
  if [ -n "$f" ]; then debs+=("$f"); else pkgs+=("$p"); fi
done
apt-get install -y -q --no-install-recommends "${pkgs[@]}" "${debs[@]}"

CPPFLAGS=$(dpkg-buildflags --get CPPFLAGS)
CFLAGS=$(dpkg-buildflags --get CFLAGS)
CXXFLAGS=$(dpkg-buildflags --get CXXFLAGS)
LDFLAGS=$(dpkg-buildflags --get LDFLAGS)
export CPPFLAGS CFLAGS CXXFLAGS LDFLAGS

# mkdeb NAME DEPENDS RECOMMENDS SYNOPSIS DESCRIPTION: $SRC/pkg/NAME (the
# files below their paths) as NAME_<version>_<arch>.deb in $OUT
mkdeb() {
  local name=$1 extra=$2 rec=$3 synopsis=$4 desc=$5
  local d=$SRC/pkg/$name version=${VERSIONS[$1]} t f elfs=() shlibs deps
  # the binaries stripped as dh_strip does
  while IFS= read -r -d '' f; do
    case $(head -c 4 "$f" | od -An -c | tr -d ' ') in
    177ELF)
      elfs+=("$f")
      if [[ $f == *.so ]]; then
        strip --remove-section=.comment --remove-section=.note --strip-unneeded "$f"
      else
        strip --remove-section=.comment --remove-section=.note "$f"
      fi
      ;;
    esac
  done < <(find "$d" -type f ! -path "$d/DEBIAN/*" -print0)
  # Depends from the libraries they link (dpkg-shlibdeps wants a
  # debian/control, and the package's root, which has DEBIAN)
  install -d -m 755 "$d/DEBIAN"
  t=$(mktemp -d)
  mkdir "$t/debian"
  printf 'Source: %s\nMaintainer: %s\n\nPackage: %s\nArchitecture: %s\n' \
    "$name" "$MAINTAINER" "$name" "$ARCH" > "$t/debian/control"
  shlibs=$(cd "$t" && dpkg-shlibdeps -O "${elfs[@]/#/-e}" 2> "$t/warnings" |
    sed -n 's/^shlibs:Depends=//p')
  sed 's/^/   /' "$t/warnings"
  rm -rf "$t"
  deps=$shlibs${extra:+, $extra}

  install -d -m 755 "$d/usr/share/doc/$name"
  cat > "$d/usr/share/doc/$name/copyright" <<EOF
$name, built from $HOMEPAGE
at commit $COMMIT by scripts/mkdeb-tools.sh.

Copyright 2026 $MAINTAINER

The repository states no licence.
EOF
  printf '%s (%s) unstable; urgency=medium\n\n  * Built from %s at %s.\n\n -- %s  %s\n' \
    "$name" "$version" "$HOMEPAGE" "$COMMIT" "$MAINTAINER" \
    "$(date -R -u -d "@$SOURCE_DATE_EPOCH")" | gzip -9n > "$d/usr/share/doc/$name/changelog.gz"
  chmod 644 "$d/usr/share/doc/$name/copyright" "$d/usr/share/doc/$name/changelog.gz"

  {
    echo "Package: $name"
    echo "Version: $version"
    echo "Architecture: $ARCH"
    echo "Maintainer: $MAINTAINER"
    echo "Installed-Size: $(du -sk --exclude=DEBIAN "$d" | cut -f1)"
    echo "Depends: $deps"
    [ -z "$rec" ] || echo "Recommends: $rec"
    echo "Section: x11"
    echo "Priority: optional"
    echo "Homepage: $HOMEPAGE"
    echo "Description: $synopsis"
    printf '%s\n' "$desc" | fold -s -w 72 | sed 's/ *$//; s/^/ /'
  } > "$d/DEBIAN/control"
  # the session hooks are configuration files
  (cd "$d" && find etc -type f 2> /dev/null | sort | sed 's|^|/|') > "$d/DEBIAN/conffiles"
  [ -s "$d/DEBIAN/conffiles" ] || rm -f "$d/DEBIAN/conffiles"
  (cd "$d" && find . -type f ! -path './DEBIAN/*' -printf '%P\0' | sort -z |
    xargs -0 md5sum) > "$d/DEBIAN/md5sums"
  chmod 644 "$d"/DEBIAN/*
  find "$d" -type d -exec chmod 755 {} +
  find "$d" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
  mkdir -p "$OUT"
  dpkg-deb --root-owner-group -Zxz --build "$d" "$OUT/${name}_${version}_${ARCH}.deb"
}

want() { case " $ONLY " in *" $1 "*) return 0 ;; esac; return 1; }

if want pro1x-events; then
  log "pro1x-events ${VERSIONS[pro1x-events]}"
  d=$SRC/pkg/pro1x-events b=$SRC/pro1x-events-deb
  rm -rf "$d" "$b"
  mkdir -p "$SRC"
  cp -r "$REPO/apps/pro1x-events" "$b"
  make -C "$b" install PREFIX=/usr DESTDIR="$d" HAVE_CAL=1 \
    PAGE="$REPO/tools/pro1x-events/pro1x-events.c"
  install -D -m 644 "$REPO/rootfs-overlay/etc/X11/Xsession.post/47pro1x-events" \
    "$d/etc/X11/Xsession.post/47pro1x-events"
  # the page is a Home widget of hildon-desktop's MeeGo-like look, which
  # scrolls it (step 3: _HILDON_MEEGO_SCROLLING_APPLET); the certificates
  # for Open-Meteo's and Mastodon's https; the event types of calls,
  # messages and chats; the calendar whose appointments it shows
  mkdeb pro1x-events \
    "hildon-desktop (>= 1:2.2.196+pro1x4), hildon-control-panel, ca-certificates" \
    "librtcom-eventlogger-plugins1, qalendar" \
    "Nokia N9 style Events page for Hildon's MeeGo-like look" \
    "pro1x-events shows on the first home view, while the MeeGo-like look is on (GConf /apps/osso/hildon-desktop/meego_pages), the day and the date, the weather of a place (Open-Meteo, no key; MeeCast's icons where MeeCast is installed), the missed calls, messages and chats (rtcom-eventlogger), unread e-mail (modest), the next days' appointments of Hildon's calendar (calendar-backend) and a Mastodon account's home timeline; pulled down it updates. Its control panel applet, Settings, Events, turns each section on or off and takes the weather's place (with Open-Meteo's geocoding) and the Mastodon server and access token. Started by the X session (Xsession.post). No Nokia graphics or fonts are included."
fi

if want pro1x-volume-osd; then
  log "pro1x-volume-osd ${VERSIONS[pro1x-volume-osd]}"
  d=$SRC/pkg/pro1x-volume-osd
  rm -rf "$d"
  install -d -m 755 "$d/usr/bin"
  # shellcheck disable=SC2046,SC2086
  gcc $CPPFLAGS $CFLAGS -Wall -o "$d/usr/bin/pro1x-volume-osd" \
    "$REPO/tools/pro1x-volume-osd/pro1x-volume-osd.c" $LDFLAGS \
    $(pkg-config --cflags --libs gtk+-2.0 gconf-2.0 libpulse-mainloop-glib) -lm
  chmod 755 "$d/usr/bin/pro1x-volume-osd"
  install -D -m 644 "$REPO/rootfs-overlay/etc/X11/Xsession.post/46pro1x-volume-osd" \
    "$d/etc/X11/Xsession.post/46pro1x-volume-osd"
  # the look it waits for (meego_pages) is patches/meego's since +pro1x2
  mkdeb pro1x-volume-osd \
    "hildon-desktop (>= 1:2.2.196+pro1x2)" \
    "pulseaudio" \
    "Nokia N9 style volume bar for Hildon's MeeGo-like look" \
    "pro1x-volume-osd shows the volume of PulseAudio's default sink as the N9's system UI does when it changes, while Hildon's MeeGo-like look is on (GConf /apps/osso/hildon-desktop/meego_pages); a tap or a drag sets it. Started by the X session (Xsession.post). The N9's graphics are used only where its theme is installed; none are included."
fi

ls -l "$OUT"/*.deb
