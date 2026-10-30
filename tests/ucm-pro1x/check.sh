#!/bin/sh
# Checks of the Pro1X's UCM profile: the NixOS port's device/audio/ucm2/Pro1X
# with patches/nixos-audio/0002-ucm-headset-jack.patch.
#
#   sh tests/ucm-pro1x/check.sh parse
#       On any Linux PC with alsa-utils (alsaucm) and patch: the profile
#       made from the NixOS port's files (NIXOS, default
#       $WORK/build/nixos-pro1x, WORK the repository's parent) and the
#       patch; alsa-lib parses it as a virtual card (no sound card needed;
#       ${CardId} is replaced by "Pro1X" in the copy), and the verbs,
#       devices and the values the headset jack needs are what they should
#       be.
#   sh check.sh controls [CARD]
#       On the phone (the profile installed, the card present, default
#       card Pro1X): every control the profile's sequences set exists on
#       the card, and every enumerated value it sets is one of the
#       control's items. Sets nothing.
#
# UCM2=<dir> (parse): the ucm2 tree to start from (default
# /usr/share/alsa/ucm2); PROFILE=<dir>: a profile made already (parse:
# instead of NIXOS and the patch; controls: default
# /usr/share/alsa/ucm2/Pro1X, the installed one).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
fail=0
bad() { echo "FAIL: $*"; fail=1; }
ok() { echo "ok: $*"; }

# the csets of all files: "name<TAB>value" per line
csets() {
	sed -n "s/^[[:space:]]*cset \"name='\([^']*\)' \(.*\)\"[[:space:]]*\$/\1	\2/p" "$@"
}

case "${1:-}" in
parse)
	command -v alsaucm > /dev/null || { echo "alsaucm missing (alsa-utils)"; exit 2; }
	t=$(mktemp -d /tmp/ucm-pro1x.XXXXXX)
	trap 'rm -r "$t"' EXIT
	if [ -z "${PROFILE:-}" ]; then
		n=${NIXOS:-${WORK:-$(cd "$repo/.." && pwd)}/build/nixos-pro1x}/device/audio/ucm2/Pro1X
		PROFILE=$t/profile
		mkdir "$PROFILE"
		cp "$n/Pro1X.conf" "$n/HiFi.conf" "$n/VoiceCall.conf" "$PROFILE/"
		if patch -s -d "$PROFILE" -p1 < "$repo/patches/nixos-audio/0002-ucm-headset-jack.patch"; then
			ok "the patch applies to $n"
		else
			bad "the patch does not apply to $n"; exit 1
		fi
	fi
	cp -a "${UCM2:-/usr/share/alsa/ucm2}" "$t/ucm2"
	rm -rf "$t/ucm2/Pro1X"
	mkdir "$t/ucm2/Pro1X"
	for f in "$PROFILE"/*.conf; do
		sed 's/\${CardId}/Pro1X/g' "$f" > "$t/ucm2/Pro1X/$(basename "$f")"
	done
	u() { ALSA_CONFIG_UCM2=$t/ucm2 alsaucm -c Pro1X "$@" 2>&1; }
	if out=$(u list _verbs); then ok "parsed: $(echo "$out" | tr -s ' \n' ' ')"
	else bad "alsa-lib does not parse it:"; echo "$out"; exit 1; fi
	want() {	# want IDENTIFIER VALUE
		# alsaucm prints "  ID=VALUE"; alsa-lib prefixes the PCMs of a
		# card with "_ucmNNNN." (PulseAudio strips it)
		v=$(u get "$1" | tail -1 | sed 's/^[^=]*=//; s/^_ucm[0-9A-F]*\.//')
		[ "$v" = "$2" ] && ok "$1 = $v" || bad "$1 = '$v', not '$2'"
	}
	devs=$(u list "_devices/HiFi" | sed -n 's/^ *[0-9]*: //p' | tr '\n' ' ')
	[ "$devs" = "Speaker Headphones Mic Headset Earpiece " ] &&
	    ok "HiFi devices: $devs" || bad "HiFi devices: $devs"
	devs=$(u list "_devices/Voice Call" | sed -n 's/^ *[0-9]*: //p' | tr '\n' ' ')
	[ "$devs" = "Speaker Headphones Mic Earpiece " ] &&
	    ok "Voice Call devices: $devs" || bad "Voice Call devices: $devs"
	want JackControl/Headphones/HiFi "Headphone Jack"
	want JackControl/Headset/HiFi "Mic Jack"
	want "JackControl/Headphones/Voice Call" "Headphone Jack"
	want PlaybackPCM/Headphones/HiFi "hw:Pro1X,0"
	want CapturePCM/Headset/HiFi "hw:Pro1X,1"
	want CapturePCM/Mic/HiFi "hw:Pro1X,1"
	# a plugged-in device above the built-in one, in both verbs
	for p in "PlaybackPriority/Headphones/HiFi 300" "PlaybackPriority/Speaker/HiFi 200" \
	    "CapturePriority/Headset/HiFi 200" "CapturePriority/Mic/HiFi 100" \
	    "PlaybackPriority/Headphones/Voice Call 300" "PlaybackPriority/Earpiece/Voice Call 200"; do
		want "${p% *}" "${p##* }"
	done
	# no JackControl where nothing tells the plug apart
	for d in Speaker Mic Earpiece; do
		if v=$(u get "JackControl/$d/HiFi"); then
			bad "JackControl on $d: $v"
		else
			ok "no JackControl on $d"
		fi
	done
	# the whole profile as alsa-lib holds it (dump: alsa-lib >= 1.2.6)
	if d=$(u dump text) && echo "$d" | grep -q "Headset"; then
		ok "dump text: $(echo "$d" | wc -l) lines"
	else
		echo "(no dump text: $(echo "$d" | tail -1))"
	fi
	;;
controls)
	command -v amixer > /dev/null || { echo "amixer missing (alsa-utils)"; exit 2; }
	PROFILE=${PROFILE:-/usr/share/alsa/ucm2/Pro1X}
	card=${2:-Pro1X}
	n=0
	csets "$PROFILE"/*.conf | sort -u > /tmp/ucm-pro1x-csets.$$
	trap 'rm -f /tmp/ucm-pro1x-csets.$$' EXIT
	while IFS='	' read -r name value; do
		n=$((n + 1))
		if ! info=$(amixer -c "$card" cget "name=$name" 2>&1); then
			bad "no control '$name'"
			continue
		fi
		case $info in
		*type=ENUMERATED*)
			v=$(echo "$value" | sed "s/^'\(.*\)'\$/\1/")
			echo "$info" | grep -q "Item #[0-9]* '$v'\$" ||
			    bad "'$name': no item '$v' ($(echo "$info" | sed -n "s/.*Item #[0-9]* '\(.*\)'/\1/p" | tr '\n' ' '))"
			;;
		esac
	done < /tmp/ucm-pro1x-csets.$$
	for j in "Headphone Jack" "Mic Jack"; do
		amixer -c "$card" cget "iface=CARD,name=$j" > /dev/null 2>&1 &&
		    ok "jack control '$j'" || bad "no jack control '$j'"
	done
	[ "$fail" = 0 ] && ok "$n settings of the profile: controls and items exist"
	;;
*)
	sed -n '2,/^set -eu/p' "$0" | sed '$d'
	exit 2 ;;
esac
[ "$fail" = 0 ] && echo PASS || { echo FAIL; exit 1; }
