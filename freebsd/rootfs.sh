#!/bin/bash
# Builds the FreeBSD root filesystem for the Pro1X on a Linux PC: the
# release's base system (base.txz), packages from pkg.freebsd.org (the
# desktop: Xorg on the framebuffer, Xfce) and the Pro1X configuration
# (rootfs/), as a UFS image: the step from which rootzfs.sh makes the ZFS
# pool (root.zfs). The phone's root is ZFS only; root.ufs stays on the PC.
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
# Time zone, as tzsetup sets it (TIMEZONE= keeps UTC)
TIMEZONE=${TIMEZONE-Europe/Vienna}
# Private, Nokia's: the stand-by screen's icons and font (see below)
HARMATTAN=${HARMATTAN-$WORK/private/harmattan}
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
	: > "$P/samples"
	while read -r f; do
		bsdtar -xf "$f" -C "$R" --exclude '+*' 2> /dev/null
		bsdtar -cf - --format=mtree --exclude '+*' @"$f" | grep -v '^#' \
			>> "$WORK/spec.pkgs"
		# The users and groups its install script would add with pw(8),
		# and its @sample files (into the second argument's file): the
		# configuration pkg copies from FILE.sample to FILE when there is
		# none, in a Lua post-install script per file
		bsdtar -xOf "$f" +MANIFEST | python3 -I -c '
import json, os, re, sys
m = json.load(sys.stdin)
for s in m.get("scripts", {}).values():
    for g in re.finditer(r"groupadd (\S+) -g (\d+)", s):
        print("group", g.group(1), g.group(2), sep="\t")
    for u in re.finditer(r"useradd (\S+) -u (\d+) -g (\d+)\s+-c \"([^\"]*)\" -d (\S+) -s (\S+)", s):
        print("user", *u.groups()[:3], u.group(5), u.group(6), u.group(4), sep="\t")
prefix = m.get("prefix", "/usr/local")
full = lambda a: a if a.startswith("/") else os.path.join(prefix, a)
with open(sys.argv[1], "a") as out:
    for s in m.get("lua_scripts", {}).get("post-install", []):
        a = re.match(r"-- args: (.*)", s)
        if a is None or "pkg.copy(sample_file" not in s:
            continue
        a = a.group(1).split()
        if not a:
            continue
        src = full(a[0])
        dst = full(a[1]) if len(a) > 1 else re.sub(r"\.sample$", "", src)
        print(src, dst, sep="\t", file=out)
' "$P/samples" >> "$P/users"
		n=$((n + 1))
	done < "$P/list"
	specs+=("$WORK/spec.pkgs")
	log "$n packages unpacked"
	# pkg's @sample: the configuration files that a package installs as
	# FILE.sample, copied to FILE where there is none, as pkg install
	# does (and the test VM, whose packages pkg installed, has them).
	# Without them PulseAudio refused to start (no default.pa: "Daemon
	# startup without any loaded modules"), and with it went
	# module-suspend-on-idle, which closes /dev/dsp 5 s after the last
	# sound so that pro1x-audiod powers the codec path down.
	ns=0
	while IFS=$'\t' read -r src dst; do
		if [ -f "$R$src" ] && [ ! -e "$R$dst" ]; then
			cp -p "$R$src" "$R$dst"
			ns=$((ns + 1))
		fi
	done < "$P/samples"
	log "$ns configuration files from their .sample (pkg's @sample)"

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
		# Leste's texts over the parts' own, as the packages have it
		# (pkgmeta.py EXCLUDE): the stage holds the catalogue of
		# whichever part was installed last, and libhildon's en_GB one
		# showed text ids (wdgt_bd_done) in place of "Done"
		for l in "$WORK"/hildon/pkgroot/leste-l10n-*/usr/local/share/locale; do
			if [ -d "$l" ]; then cp -a "$l/." "$R/usr/local/share/locale/"; fi
		done
		cp -a "$REPO/freebsd/hildon/rootfs/." "$R/"
		# Nokia's files for the stand-by screen (icons, Nokia Pure),
		# never in this repository: from a private directory when
		# there is one (HARMATTAN=, standby/*.png and fonts/*.ttf)
		if [ -d "$HARMATTAN/standby" ]; then
			mkdir -p "$R/usr/local/share/pro1x/harmattan/standby" \
			    "$R/usr/local/share/fonts/nokia"
			cp "$HARMATTAN"/standby/*.png \
			    "$R/usr/local/share/pro1x/harmattan/standby/"
			cp "$HARMATTAN"/fonts/*.ttf "$R/usr/local/share/fonts/nokia/"
			log "Nokia's stand-by icons and font from $HARMATTAN"
		fi
		# The N9's meegotouch themes (base, blanco) and the Nokia Pure
		# fonts, which com.nokia.meego (qt-components), meego-clock and
		# the N9's own user interface draw with (themes/, fonts/nokia-pure/:
		# tools/extract-blanco.sh there). Without overwriting: the
		# packages' own files stay (Nokia's base names Nokia's home
		# view, meegotouchhome's names its own)
		if [ -d "$HARMATTAN/themes/blanco" ]; then
			mkdir -p "$R/usr/local/share/themes" "$R/usr/local/share/fonts/nokia"
			cp -Rn "$HARMATTAN"/themes/base "$HARMATTAN"/themes/blanco \
			    "$R/usr/local/share/themes/"
			cp "$HARMATTAN"/fonts/nokia-pure/*.ttf "$R/usr/local/share/fonts/nokia/"
			log "the N9's blanco theme and Nokia Pure from $HARMATTAN"
		fi
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
# pkg through pro1x-pkg: a boot environment before it changes packages
printf '\n# a boot environment before pkg changes packages (pro1x-pkg)\nalias pkg=pro1x-pkg\n' >> "$R/root/.shrc"
# root's home is filled at boot from here (rc.d/pro1x_setup)
mkdir -p "$R/usr/local/share/pro1x/root"
cp -a "$R/root/." "$R/usr/local/share/pro1x/root/"
cp -a "$REPO/freebsd/rootfs/." "$R/"
# The USB gadget's sshd (rc.d/pro1x_usbnet, pm/README.md): the PC user's
# public key, as scripts/40-make-rootfs.sh puts it into Leste; only a .pub
# file, never a private key
SSH_PUBKEY=${SSH_PUBKEY:-$HOME/.ssh/id_ed25519.pub}
case "$SSH_PUBKEY" in
*.pub)
	if [ -f "$SSH_PUBKEY" ] && ! grep -q "PRIVATE KEY" "$SSH_PUBKEY"; then
		mkdir -p "$R/usr/local/etc/ssh"
		install -m 644 "$SSH_PUBKEY" \
		    "$R/usr/local/etc/ssh/pro1x_authorized_keys"
	fi
	;;
esac
# The zone copied, as tzsetup does; its name for clockd goes to
# /var/db/zoneinfo at boot (rc.d/pro1x_setup: /var is in memory)
if [ -n "$TIMEZONE" ]; then
	cp "$R/usr/share/zoneinfo/$TIMEZONE" "$R/etc/localtime"
	echo "$TIMEZONE" > "$R/usr/local/share/pro1x/zoneinfo"
fi
# The DSP stack (dsp/): its kernel modules and tools from build.sh, and the
# DSPs' firmware, signed by the phone's maker, from the local Leste image
# only (LESTE_IMG; not in this repository or its releases)
if [ -d "$OUT/modules" ]; then
	mkdir -p "$R/boot/modules" "$R/usr/local/sbin"
	cp "$OUT"/modules/*.ko "$R/boot/modules/"
	cp "$OUT"/dsp-tools/* "$R/usr/local/sbin/"
fi
# Power management (pm/README.md): the Pro1X's zzz
mkdir -p "$R/usr/local/sbin"
install -m 755 "$REPO/freebsd/pm/tools/pro1x-suspend" "$R/usr/local/sbin/"
# and the battery's drain for the power profile's device runs
install -m 755 "$REPO/freebsd/pm/tools/pro1x-drain" "$R/usr/local/sbin/"
LESTE_IMG=${LESTE_IMG:-$WORK/../out/maemo-leste.img}
FW_CHECKOUT=${FW_CHECKOUT:-$WORK/../build/firmware-fxtec-qx1050/lib/firmware}
FW_ATH10K=${FW_ATH10K:-/usr/lib/firmware/ath10k/WCN3990}
FW=usr/local/share/pro1x/firmware
if [ -f "$LESTE_IMG" ] && command -v debugfs > /dev/null; then
	log "DSP firmware from $LESTE_IMG"
	mkdir -p "$R/$FW/qcom/sm6115/Fxtec" "$R/$FW/ath10k"
	debugfs -R "rdump /lib/firmware/qcom/sm6115/Fxtec/QX1050 $R/$FW/qcom/sm6115/Fxtec" \
	    "$LESTE_IMG" > /dev/null 2>&1
	debugfs -R "rdump /lib/firmware/ath10k/WCN3990 $R/$FW/ath10k" \
	    "$LESTE_IMG" > /dev/null 2>&1
	ls "$R/$FW/qcom/sm6115/Fxtec/QX1050" | tr '\n' ' '; echo
elif [ -d "$FW_CHECKOUT/qcom/sm6115/Fxtec/QX1050" ]; then
	# else the device's firmware as 40-make-rootfs.sh fetches it for Leste
	# (sm6115-mainline/firmware-fxtec-qx1050), and the Wi-Fi's from the
	# build host's linux-firmware (ath10k/WCN3990, as Leste has it),
	# unpacked where the host keeps it compressed
	log "DSP firmware from $FW_CHECKOUT, Wi-Fi's from $FW_ATH10K"
	mkdir -p "$R/$FW/qcom/sm6115/Fxtec" "$R/$FW/ath10k"
	cp -R "$FW_CHECKOUT/qcom/sm6115/Fxtec/QX1050" "$R/$FW/qcom/sm6115/Fxtec/"
	if [ -d "$FW_ATH10K" ]; then
		cp -R "$FW_ATH10K" "$R/$FW/ath10k/"
		find "$R/$FW/ath10k" -name '*.zst' -exec zstd -dq --rm {} \;
		find "$R/$FW/ath10k" -name '*.xz' -exec xz -dq {} \;
	else
		log "no $FW_ATH10K: the root image has no Wi-Fi firmware"
	fi
	ls "$R/$FW/qcom/sm6115/Fxtec/QX1050" "$R/$FW/ath10k"/*/* | tr '\n' ' '; echo
else
	log "no Leste image ($LESTE_IMG) nor firmware checkout ($FW_CHECKOUT): the root image has no DSP firmware"
fi
# The phone's sensor configuration and registry for hexagonrpcd, as Leste
# installs them (scripts/46-sensor-stack.sh): from the NixOS port's checkout
NIXOS=${NIXOS:-$WORK/../build/nixos-pro1x}
if [ -d "$NIXOS/device/hexagonfs" ]; then
	mkdir -p "$R/usr/local/share/pro1x"
	rm -rf "$R/usr/local/share/pro1x/hexagonfs"
	cp -R "$NIXOS/device/hexagonfs" "$R/usr/local/share/pro1x/hexagonfs"
fi
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
log "done: now rootzfs.sh (the phone's root is ZFS only)"
