#!/bin/bash
# Copies a block's files from the PC to the phone for the device day
# (docs/device-day-2026-10-31.md). Copies and checks only: it installs,
# flashes, mounts, kexecs and reboots nothing. Installing, putting a new
# kernel.bin in place, creating pools are the runbook's steps.
#
#   bash tools/devday/deploy.sh BLOCK...     copy
#   bash tools/devday/deploy.sh list BLOCK   the files, nothing copied
#   DRY=1 bash tools/devday/deploy.sh BLOCK  the commands only
#
# Each file goes over scp (lib.sh: the system's pinned host key) to
# DEST.part, is compared by sha256 there, and replaces DEST; a DEST with
# other content is kept as DEST.before-2026-10-31 (once). A DEST that is
# the same already is left alone. Big images go under dated names
# (kernel.bin-2026-10-31, root.zfs-2026-10-31): the runbook puts them in
# place. The phone must keep 1 GiB free where a block writes.
#
# Blocks (SYS=leste unless named):
#   tools        ramconsole.py, leste-install.sh -> /root/devday/
#   audio        the test sounds (1 s silence, 100 ms beep), the UCM check,
#                the Leste audio trace for FreeBSD, pro1x-fm-test and
#                pro1x-fm-audio-test
#   leste-debs   $WORK/devday/leste-debs/<group> -> /root/devday/debs/
#                (tools/devday/fetch.sh first; leste-install.sh installs)
#   ofono        oFono's packages -> /root/mobile-data/, 49-mobile-data.sh's
#                parts -> /root/leste-md.tar (docs/leste-mobile-data.md)
#   waydroid     pro1x-waydroid_*.deb -> /root/ (docs/waydroid.md)
#   kali         kali/ -> /root/kali.tar, the fixed pro1x-rtc.service ->
#                /root/devday/kali/ (docs/kali-mobile-data.md)
#   arch         arch-hildon packages -> /host/devday/arch-pkgs/, arch/ ->
#                /host/devday/arch.tar (arch/README.md)
#   freebsd      out/kernel.bin -> /host/freebsd/kernel.bin-DAY,
#                out/root.zfs -> /host/freebsd/root.zfs-DAY (3 GB),
#                out/modules -> /host/freebsd/modules/
#   freebsd-pm   pm-out/kernel.bin -> /host/freebsd/kernel.bin-pm-DAY, the
#                pm runs' modules -> /host/freebsd/pm/
#   freebsd-tests  the test tools, firmware and cmdlines of dsp-test,
#                fm-test, audio-test, cam-test (-> /host/freebsd/{dsp-tools,
#                fm,firmware/qca,audio,camera,cmdlines}/), their extra
#                modules where out/modules has none, dsp-test/kernel.bin ->
#                kernel.bin-dsp-DAY, pro1x-kexec-freebsd.sh and
#                pro1x-camfetch -> /root/devday/
#   freebsd-wifi phone-private/freebsd/wpa_supplicant.conf ->
#                /host/freebsd/wpa_supplicant.conf (600; never the SIM PIN:
#                the user writes /host/freebsd/sim-pin and data.conf)
#   kolibri      KolibriOS (~/ps/kolibrios-arm): build/arm64/kernel.bin ->
#                /host/kolibri/kernel.bin-DAY, the new pro1x-kexec.sh (the
#                -m adopt flow) with mkefs.py and pro1x-read.py ->
#                /root/kolibri/, kolibri-home -> /usr/local/sbin/
#   whisperfish  the Leste deb and leste/ -> /host/wf-leste/in/
#   n950         (SYS=n950) KolibriOS's build/n950/kernel.bin ->
#                /boot/kolibri-new.bin, n950-dss-dump.sh -> /root/devday/
set -euo pipefail
. "$(dirname "$0")/lib.sh"
FBSD=${FBSD:-$WORK/freebsd}
STAGE=$WORK/devday/stage
WF_DEB=${WF_DEB:-$WORK/devday/whisperfish/whisperfish-leste_0.6.0.dev-1_arm64.deb}

ITEMS=()
missing=0
item() {    # item SRC DEST [MODE]
	if [ ! -f "$1" ]; then
		echo "MISSING  $1" >&2
		missing=$((missing + 1))
		return 0
	fi
	ITEMS+=("$1|$2|${3:-644}")
}
items_dir() {   # items_dir DIR DESTDIR [MODE] [GLOB]: the files directly in DIR
	local f
	[ -d "$1" ] || { echo "MISSING  $1/" >&2; missing=$((missing + 1)); return 0; }
	for f in "$1"/${4:-*}; do
		[ -f "$f" ] && ITEMS+=("$f|$2/${f##*/}|${3:-644}")
	done
	return 0
}
archive() {     # archive REPO NAME PATH...: a tar of PATHs at the repo's HEAD
	local r=$1 n=$2
	shift 2
	mkdir -p "$STAGE"
	git -C "$r" archive -o "$STAGE/$n" HEAD "$@"
	echo "$STAGE/$n"
}
stale() {       # stale FILE REPO: warn when FILE is older than REPO's last commit
	[ -f "$1" ] || return 0
	local c f
	c=$(git -C "$2" log -1 --format=%ct)
	f=$(stat -c %Y "$1")
	[ "$f" -ge "$c" ] || echo "WARNING  $1 is older than $2's last commit: rebuild it" >&2
}

block() {
	case $1 in
	tools)
		item "$REPO/tools/devday/ramconsole.py" /root/devday/ramconsole.py 755
		item "$REPO/tools/devday/leste-install.sh" /root/devday/leste-install.sh 755
		;;
	audio)
		mkdir -p "$STAGE"
		python3 -I -c 'import math,struct,wave,sys
def wav(p, n, a):
    w = wave.open(p, "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
    w.writeframes(b"".join(struct.pack("<hh", v, v) for i in range(n)
        for v in [int(a * min(1, i / 240, (n - i) / 240) * math.sin(2 * math.pi * 1000 * i / 48000))]))
wav(sys.argv[1], 48000, 0); wav(sys.argv[2], 4800, 8000)' "$STAGE/pro1x-silence.wav" "$STAGE/pro1x-beep.wav"
		item "$STAGE/pro1x-silence.wav" /root/devday/pro1x-silence.wav
		item "$STAGE/pro1x-beep.wav" /root/devday/pro1x-beep.wav
		item "$REPO/tests/ucm-pro1x/check.sh" /root/devday/ucm-check.sh 755
		item "$REPO/freebsd/audio/test/leste-audio-trace.sh" /root/devday/leste-audio-trace.sh 755
		item "$REPO/tools/pro1x-fm/pro1x-fm-test" /usr/local/bin/pro1x-fm-test 755
		item "$REPO/tools/pro1x-fm/pro1x-fm-audio-test" /usr/local/bin/pro1x-fm-audio-test 755
		;;
	leste-debs)
		for g in 1-gtk 2-libs 3-system 4-apps 5-harmattan; do
			items_dir "$WORK/devday/leste-debs/$g" "/root/devday/debs/$g" 644 '*.deb'
		done
		;;
	ofono)
		items_dir "$WORK/devday/leste-debs/ofono" /root/mobile-data 644 '*.deb'
		item "$(archive "$REPO" leste-md.tar scripts/49-mobile-data.sh \
		    rootfs-overlay/etc/init.d/pro1x-modem-wait tools/pro1x-rmnet-flags)" /root/leste-md.tar
		;;
	waydroid)
		items_dir "$WORK/devday/leste-debs/waydroid" /root 644 'pro1x-waydroid_*.deb'
		;;
	kali)
		item "$(archive "$REPO" kali.tar kali)" /root/kali.tar
		item "$REPO/kali/rootfs-overlay/etc/systemd/system/pro1x-rtc.service" /root/devday/kali/pro1x-rtc.service
		;;
	arch)
		items_dir "$WORK/devday/arch-pkgs" /host/devday/arch-pkgs
		item "$(archive "$REPO" arch.tar arch freebsd/hildon patches)" /host/devday/arch.tar
		;;
	freebsd)
		item "$FBSD/out/kernel.bin" "/host/freebsd/kernel.bin-$DAY"
		item "$FBSD/out/root.zfs" "/host/freebsd/root.zfs-$DAY"
		items_dir "$FBSD/out/modules" /host/freebsd/modules
		;;
	freebsd-pm)
		item "$FBSD/pm-out/kernel.bin" "/host/freebsd/kernel.bin-pm-$DAY"
		for m in qcom_dsp pro1x_rpmsleep usb usb_template uether if_cdce pro1x_usb; do
			item "$FBSD/pm-out/modules/$m.ko" "/host/freebsd/pm/$m.ko"
		done
		;;
	freebsd-tests)
		item "$FBSD/dsp-test/kernel.bin" "/host/freebsd/kernel.bin-dsp-$DAY"
		items_dir "$FBSD/dsp-test/dsp-tools" /host/freebsd/dsp-tools 755
		items_dir "$FBSD/fm-test/fm" /host/freebsd/fm 755
		items_dir "$FBSD/fm-test/firmware/qca" /host/freebsd/firmware/qca
		items_dir "$FBSD/audio-test/audio" /host/freebsd/audio 755
		items_dir "$FBSD/cam-test/camera" /host/freebsd/camera 755
		for t in dsp-test fm-test audio-test cam-test; do
			items_dir "$FBSD/$t" /host/freebsd/cmdlines 644 'cmdline.*'
		done
		# extra modules: out/modules' own where it has them (the joint
		# build, matching out/kernel.bin), else the test directory's
		for m in audio-test/modules/sound.ko audio-test/modules/snd_pro1x.ko \
		    cam-test/modules/qcom_camss.ko data-out/modules/qcom_ipa.ko; do
			if [ -f "$FBSD/out/modules/${m##*/}" ]; then
				echo "note     ${m##*/}: out/modules has it (block freebsd)" >&2
			else
				item "$FBSD/$m" "/host/freebsd/modules/${m##*/}"
			fi
		done
		item "$REPO/freebsd/dsp/tools/pro1x-kexec-freebsd.sh" /root/devday/pro1x-kexec-freebsd.sh 755
		item "$REPO/freebsd/camera/tools/pro1x-camfetch" /root/devday/pro1x-camfetch 755
		;;
	freebsd-wifi)
		item "$WORK/phone-private/freebsd/wpa_supplicant.conf" /host/freebsd/wpa_supplicant.conf 600
		;;
	kolibri)
		stale "$KOS/build/arm64/kernel.bin" "$KOS"
		item "$KOS/build/arm64/kernel.bin" "/host/kolibri/kernel.bin-$DAY"
		item "$KOS/tools/pro1x-kexec.sh" /root/kolibri/pro1x-kexec.sh 755
		item "$KOS/tools/mkefs.py" /root/kolibri/mkefs.py 755
		item "$KOS/tools/pro1x-read.py" /root/kolibri/pro1x-read.py 755
		item "$KOS/tools/kolibri-home" /usr/local/sbin/kolibri-home 755
		;;
	whisperfish)
		item "$WF_DEB" "/host/wf-leste/in/${WF_DEB##*/}"
		item "$(archive "$HOME/ps/whisperfish-leste" wf-leste-src.tar leste)" /host/wf-leste/in/wf-leste-src.tar
		;;
	n950)
		[ "$SYS" = n950 ] || die "block n950: SYS=n950"
		stale "$KOS/build/n950/kernel.bin" "$KOS"
		item "$KOS/build/n950/kernel.bin" /boot/kolibri-new.bin
		item "$KOS/tools/n950-dss-dump.sh" /root/devday/n950-dss-dump.sh 755
		;;
	*) die "unknown block '$1' (see: sed -n 2,60p $0)" ;;
	esac
}

mode=copy
[ "${1:-}" != list ] || { mode=list; shift; }
[ $# -gt 0 ] || { sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 1; }
for b in "$@"; do block "$b"; done
[ "${#ITEMS[@]}" -gt 0 ] || die "nothing to copy"

total=0
for it in "${ITEMS[@]}"; do
	IFS='|' read -r src dest m <<< "$it"
	sz=$(stat -c %s "$src")
	total=$((total + sz))
	[ "$mode" = list ] && printf '%10d  %s  %s -> %s\n' "$sz" "$m" "$src" "$dest"
done
log "${#ITEMS[@]} files, $((total / 1048576)) MiB, $missing missing"
[ "$mode" = copy ] || exit 0
[ "$missing" = 0 ] || [ "${ALLOW_MISSING:-0}" = 1 ] ||
    die "$missing source(s) missing (ALLOW_MISSING=1 copies the rest)"

# room on the phone: 1 GiB more than the block, where it writes
for d in /host /root /usr/local /boot; do
	n=0
	for it in "${ITEMS[@]}"; do
		IFS='|' read -r src dest m <<< "$it"
		case $dest in "$d"/*) n=$((n + $(stat -c %s "$src"))) ;; esac
	done
	[ "$n" -gt 0 ] || continue
	free=$(ph "mkdir -p \$R$d && df -Pk \$R$d | awk 'NR == 2 { print \$4 }'")
	[ "$DRY" = 1 ] && continue
	[ $((free * 1024 - n)) -ge $((1 << 30)) ] ||
	    die "phone: $d has $((free / 1024)) MiB free, the block needs $((n / 1048576)) MiB and 1 GiB is kept"
done

logf=$RUNS/deploy.log
[ "$DRY" = 1 ] || mkdir -p "$RUNS"
for it in "${ITEMS[@]}"; do
	IFS='|' read -r src dest m <<< "$it"
	h=$(sha "$src")
	there=$(ph "[ -f \$R$dest ] && sha256sum < \$R$dest | cut -c1-64; true")
	if [ "$DRY" != 1 ] && [ "$there" = "$h" ]; then
		echo "same     $dest"
		continue
	fi
	ph "mkdir -p \$R${dest%/*}"
	put "$src" "$dest.part"
	ph "set -e; d=\$R$dest; [ \"\$(sha256sum < \$d.part | cut -c1-64)\" = $h ] || { rm -f \$d.part; echo 'sha256 differs after the copy'; exit 1; }
	    if [ -f \$d ] && [ ! -e \$d.before-$DAY ]; then mv \$d \$d.before-$DAY; fi
	    chmod $m \$d.part; mv \$d.part \$d; sync"
	echo "copied   $dest"
	[ "$DRY" = 1 ] || echo "$(date -Iseconds) $SYS $h $src -> $dest" >> "$logf"
done
log "done (log: $logf)"
