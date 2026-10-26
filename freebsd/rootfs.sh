#!/bin/bash
# Builds the FreeBSD root filesystem for the Pro1X on a Linux PC: the
# release's base system (base.txz), packages from pkg.freebsd.org (the
# desktop: Xorg on the framebuffer, Xfce) and the Pro1X configuration
# (rootfs/), as a UFS image. On the phone it lives at
# /host/freebsd/root.ufs; the RAM root (mfsroot/etc/rc) mounts userdata read
# only, attaches the image and reroots into it.
#
#   WORK=/path/to/freebsd-build bash freebsd/rootfs.sh   (after build.sh)
#   PKGS="" ...                  base system only
#
# The package index is checked against the release's trusted pkg key and
# every package against the index (pkgfetch.py). Owners, modes and flags
# come from mtrees of the tarballs (mkspec.py), since everything is
# unpacked as an ordinary user. What packages do when they are installed
# is done here: their users and groups (pwd_mkdb of the FreeBSD build), and
# with the host's tools the GSettings schemas and the MIME database.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
WORK=${WORK:?set WORK=<build directory>}
ABI=${ABI:-FreeBSD:15:aarch64}
PKGURL=${PKGURL:-https://pkg.freebsd.org/$ABI/quarterly}
PKGS=${PKGS-xorg-minimal xf86-video-scfb xf86-input-libinput xinit xfce xfce4-terminal xfce4-genmon-plugin dejavu xset xwd}
# Pulled in only for building or for optional parts: LLVM (mesa's OpenCL
# and llvmpipe, 1.8 GiB), Samba (gvfs's SMB backend), Qt and bsdisks
# (automounting), Python and Perl (scripts)
PRUNE=${PRUNE-llvm* spirv-llvm-translator* samba* qt6-* bsdisks python3* py3*-* perl5*}
# Hildon (freebsd/hildon/build.sh), when built: HILDON= leaves it out
HILDON=${HILDON-$WORK/hildon/stage}
if [ -n "$HILDON" ] && [ -d "$HILDON" ]; then
	. "$REPO/freebsd/hildon/deps.sh"
	PKGS="$PKGS $HILDON_PKGS $APPS_PKGS"
else
	HILDON=
fi
OUT=$WORK/out
B=$WORK/dl/base.txz
R=$WORK/rootfs
P=$WORK/pkgs
log() { echo ">> $*"; }

LEGACY=$(dirname "$(find "$WORK/obj" -path '*/tmp/legacy/bin/makefs' -type f | head -1)")
[ -x "$LEGACY/makefs" ] || { echo "makefs missing: run build.sh first"; exit 1; }
[ -f "$B" ] || { echo "$B missing: run build.sh first"; exit 1; }

log "base system"
rm -rf "$R" "$WORK"/spec.*
mkdir -p "$R" "$P/cache"
bsdtar -xf "$B" -C "$R"
bsdtar -cf - --format=mtree @"$B" > "$WORK/spec.base"
specs=("$WORK/spec.base")

if [ -n "$PKGS" ]; then
	log "package index ($PKGURL)"
	curl -sSfL -o "$P/packagesite.pkg" "$PKGURL/packagesite.pkg"
	rm -rf "$P/site"
	mkdir -p "$P/site"
	bsdtar -xf "$P/packagesite.pkg" -C "$P/site"
	# The key must be the release's trusted one; pkg signs the SHA-256 of
	# the index as a hex string.
	trusted=$(sed -n 's/^fingerprint: "\(.*\)"/\1/p' \
		"$R"/usr/share/keys/pkg/trusted/pkg.freebsd.org.*)
	echo "$trusted" | grep -qx "$(sha256sum < "$P/site/packagesite.yaml.pub" | cut -d' ' -f1)" ||
		{ echo "package index signed by an untrusted key"; exit 1; }
	sha256sum < "$P/site/packagesite.yaml" | cut -d' ' -f1 | tr -d '\n' |
		openssl dgst -sha256 -verify "$P/site/packagesite.yaml.pub" \
		-signature "$P/site/packagesite.yaml.sig" > /dev/null ||
		{ echo "package index signature does not verify"; exit 1; }

	log "packages: $PKGS"
	x=()
	for p in $PRUNE; do x+=(-x "$p"); done
	python3 -I "$REPO/freebsd/pkgfetch.py" "${x[@]}" "$P/site" "$P/cache" \
		"$PKGURL" $PKGS > "$P/list"
	n=0
	: > "$WORK/spec.pkgs"
	: > "$P/users"
	while read -r f; do
		bsdtar -xf "$f" -C "$R" --exclude '+*' 2> /dev/null
		bsdtar -cf - --format=mtree --exclude '+*' @"$f" | grep -v '^#' \
			>> "$WORK/spec.pkgs"
		# The users and groups its install script would add with pw(8)
		bsdtar -xOf "$f" +MANIFEST | python3 -I -c '
import json, re, sys
m = json.load(sys.stdin)
for s in m.get("scripts", {}).values():
    for g in re.finditer(r"groupadd (\S+) -g (\d+)", s):
        print("group", g.group(1), g.group(2), sep="\t")
    for u in re.finditer(r"useradd (\S+) -u (\d+) -g (\d+)\s+-c \"([^\"]*)\" -d (\S+) -s (\S+)", s):
        print("user", *u.groups()[:3], u.group(5), u.group(6), u.group(4), sep="\t")
' >> "$P/users"
		n=$((n + 1))
	done < "$P/list"
	specs+=("$WORK/spec.pkgs")
	log "$n packages unpacked"

	if [ -n "$HILDON" ]; then
		# Mesa's software renderer (hildon-desktop draws with GLES)
		# needs LLVM's library, not the 1.8 GiB of the package
		f=$(python3 -I "$REPO/freebsd/pkgfetch.py" -x libedit -x 'lua5*' \
		    -x 'perl5*' -x 'python3*' -x zstd "$P/site" "$P/cache" \
		    "$PKGURL" llvm19 2> /dev/null)
		bsdtar -xf "$f" -C "$R" '/usr/local/llvm19/lib/libLLVM*.so*' 2> /dev/null
		log "Hildon ($HILDON) and libLLVM from $(basename "$f")"
		# What the image needs of the build: no headers, static
		# libraries, pkg-config files or manuals
		(cd "$HILDON" && find . -type f -o -type l | grep -v \
		    -e '^\./usr/local/include/' -e '\.a$' -e '/pkgconfig/' \
		    -e '^\./usr/local/share/man/' -e '^\./usr/local/share/gtk-doc/') |
		    (cd "$HILDON" && cpio -pdm --quiet "$R")
		cp -a "$REPO/freebsd/hildon/rootfs/." "$R/"
		# HiDPI (tools/pro1x-hidpi): the theme for the default factor
		# made here, other factors at boot into /var
		s=$(sed -n 's/^DEFAULT=//p' "$REPO/tools/pro1x-hidpi/pro1x-hidpi")
		# shellcheck disable=SC2046
		gcc -O2 -o "$WORK/pro1x-theme-scale.host" \
		    "$REPO/tools/pro1x-hidpi/pro1x-theme-scale.c" \
		    $(pkg-config --cflags --libs gdk-pixbuf-2.0) -lm
		rm -rf "$R/usr/local/share/pro1x/hidpi/alpha-$s"
		mkdir -p "$R/usr/local/share/pro1x/hidpi"
		"$WORK/pro1x-theme-scale.host" -r "$R" /usr/local/share/themes/alpha \
		    "/usr/local/share/pro1x/hidpi/alpha-$s" "$s"
	fi

	log "their users and groups"
	sort -u "$P/users" | while IFS=$'\t' read -r kind name id gid home shell gecos; do
		case "$kind" in
		group)
			grep -q "^$name:" "$R/etc/group" || echo "$name:*:$id:" >> "$R/etc/group"
			;;
		user)
			grep -q "^$name:" "$R/etc/master.passwd" ||
				echo "$name:*:$id:$gid::0:0:$gecos:$home:$shell" >> "$R/etc/master.passwd"
			;;
		esac
	done
	"$LEGACY/pwd_mkdb" -p -d "$R/etc" "$R/etc/master.passwd"

	log "GSettings schemas, MIME database (host tools)"
	if [ -d "$R/usr/local/share/glib-2.0/schemas" ]; then
		glib-compile-schemas "$R/usr/local/share/glib-2.0/schemas"
	fi
	if [ -d "$R/usr/local/share/mime/packages" ]; then
		update-mime-database "$R/usr/local/share/mime" > /dev/null 2>&1 || true
	fi
fi

log "Pro1X configuration (rootfs/)"
# root's home is a tmpfs filled at boot from here (rc.d/pro1x_setup)
mkdir -p "$R/usr/local/share/pro1x/root"
cp -a "$R/root/." "$R/usr/local/share/pro1x/root/"
cp -a "$REPO/freebsd/rootfs/." "$R/"
# A fixed host id: rc would write one into /etc, which is read only.
uuidgen > "$R/etc/hostid"
tr -d '-' < "$R/etc/hostid" > "$R/etc/machine-id"

log "specification"
python3 -I "$REPO/freebsd/mkspec.py" "$R" "$WORK/spec.root" "${specs[@]}"

size=$(du -sm "$R" | cut -f1)
size=$(( (size * 5 / 4 + 256) / 256 * 256 ))
log "image (${size} MiB for $(du -sh "$R" | cut -f1))"
rm -f "$OUT/root.ufs"
"$LEGACY/makefs" -t ffs -B little -o version=2,label=pro1xroot -s "${size}m" \
	-F "$WORK/spec.root" -N "$R/etc" "$OUT/root.ufs" "$R"
ls -la "$OUT/root.ufs"
log "done: copy to /host/freebsd/root.ufs on the phone"
