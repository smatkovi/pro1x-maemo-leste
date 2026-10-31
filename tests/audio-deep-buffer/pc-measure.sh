#!/bin/bash
# The music sink's effect on the CPU's wake-ups, measured on the PC: Leste's
# PulseAudio and a GStreamer player in the Leste chroot (target.sh) against
# pcm_q6sim, a PCM with q6asm-dai's limits; perf on the PC counts when
# their threads ran. docs/audio.md, "Music with a deep buffer", has the
# results.
#
#   BASE_IMG=<image> CHROOT=<dir> bash scripts/leste-chroot.sh mount
#   BASE_IMG=<image> CHROOT=<dir> bash scripts/leste-chroot.sh run \
#       apt-get install -y --no-install-recommends gcc libasound2-dev \
#       gstreamer1.0-tools gstreamer1.0-pulseaudio      (once)
#   BASE_IMG=<image> CHROOT=<dir> bash tests/audio-deep-buffer/pc-measure.sh [SCENARIO...]
#
# SCENARIO: before, after, after-0034 (target.sh); each with the players
# mafw and qt, SECS seconds (default 20) each. Per run (and, after, the
# volume pro1x_music has when the default sink is set to 37 %):
#   periods/s    the PCMs' periods: on the phone one ASM write and one
#                WRITE_DONE interrupt each
#   switches/s   how often PulseAudio's and the player's threads went to
#                sleep (each a wake-up before)
#   wake-ups/s   the same, but runs closer than GAP_MS (default 2) to the
#                previous one counted once: how often the CPU has to leave
#                idle for the sound (the decoder's buffers to PulseAudio
#                come in bursts)
#   idle ms      the median time between those wake-ups
# The chroot runs under qemu (slower than the phone's CPU, so bursts last
# longer); the counts are what the programs do, not how long it takes.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
CHROOT=${CHROOT:?set CHROOT (and BASE_IMG) as for scripts/leste-chroot.sh}
SECS=${SECS:-20}
GAP_MS=${GAP_MS:-2}
SCENARIOS=${*:-before after after-0034}
R=$CHROOT/root
T=/tmp/q6sim-run
chr() { bash "$REPO/scripts/leste-chroot.sh" run "$@" 2> /dev/null; }
tmp=$(mktemp -d /tmp/pc-measure.XXXXXX)
trap 'chr sh /root/q6sim/target.sh stop > /dev/null || true; rm -rf "$tmp"' EXIT

sudo -n mkdir -p "$R/root/q6sim"
sudo -n cp "$REPO/tests/audio-deep-buffer/pcm_q6sim.c" "$REPO/tests/audio-deep-buffer/target.sh" \
	"$REPO/tools/pro1x-music-sink/pro1x-music-sink.c" "$R/root/q6sim/"

csw() {	# the voluntary context switches of all threads of the PIDs
	cat $(printf '/proc/%s/task/*/status ' "$@") 2> /dev/null |
		awk '/^voluntary_ctxt_switches/ { s += $2 } END { print s + 0 }'
}
periods() {	# the periods played by the simulated PCM
	sudo -n sed -n 's/^periods=\([0-9]*\).*/\1/p' "$R$T/$1.stats" 2> /dev/null || echo 0
}

printf '%-10s %-4s  %-11s %-17s %10s %10s %8s\n' scenario play "stream on" "periods/s mm1 mm4" \
	switches/s wake-ups/s "idle ms"
for sc in $SCENARIOS; do
	for pl in mafw qt; do
		out=$(chr sh /root/q6sim/target.sh start "$sc" "$pl")
		on=$(echo "$out" | sed -n 's/^stream on: //p')
		vol=$(echo "$out" | sed -n 's/^pro1x_music follows .*: /volume mirrored: /p')
		read -r pa gp _ < <(sudo -n cat "$R$T/pids")
		c0=$(csw "$pa" "$gp") a0=$(periods mm1) b0=$(periods mm4) t0=$(date +%s.%N)
		sudo -n perf record -q -e sched:sched_switch -p "$pa,$gp" -o "$tmp/perf.data" \
			-- sleep "$SECS" 2> /dev/null
		c1=$(csw "$pa" "$gp") a1=$(periods mm1) b1=$(periods mm4) t1=$(date +%s.%N)
		setup=$(chr sh /root/q6sim/target.sh stop | tr '\n' ';')
		# the times our threads went to sleep, in ms; a wake-up is one more
		# than GAP_MS after the previous one
		sudo -n perf script -i "$tmp/perf.data" -F time 2> /dev/null | tr -d ' :' | sort -n |
			awk -v gap="$GAP_MS" '{ t = $1 * 1000; if (NR == 1 || t - last > gap) { if (NR > 1) print t - last } last = t }' \
			> "$tmp/idle"
		w=$(wc -l < "$tmp/idle")
		med=$(sort -n "$tmp/idle" | awk '{ a[NR] = $1 } END { print NR ? a[int((NR + 1) / 2)] : 0 }')
		awk -v s="$SECS" -v el="$(echo "$t1 - $t0" | bc)" -v sc="$sc" -v pl="$pl" -v on="${on:-?}" -v c="$((c1 - c0))" \
		    -v m1="$((a1 - a0))" -v m4="$((b1 - b0))" -v w="$((w + 1))" -v med="$med" 'BEGIN {
			printf "%-10s %-4s  %-11s %6.1f %6.1f     %10.1f %10.1f %8.1f\n",
			    sc, pl, on, m1 / el, m4 / el, c / el, w / s, med }'
		echo "    ${setup%;}${vol:+; $vol}"
		sudo -n rm -f "$tmp/perf.data"
	done
done
