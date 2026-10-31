#!/bin/sh
# The target side of pc-measure.sh (as root in a Leste system, on the PC in
# the Leste chroot): Leste's PulseAudio (16.1) and a GStreamer player
# against pcm_q6sim, an ALSA PCM with q6asm-dai's limits (pcm_q6sim.c).
#
#   target.sh start SCENARIO PLAYER   PulseAudio, pro1x-music-sink and the
#                                     player started, the PIDs of PulseAudio
#                                     and the player in $T/pids, the
#                                     stream's sink printed after WARM
#                                     seconds, and whether pro1x_music
#                                     follows the default sink's volume
#   target.sh stop                    all stopped, the sinks' setup printed
#
# SCENARIO:
#   before      one sink, MultiMedia1 as now (PulseAudio's default
#               fragments, what module-alsa-card's UCM sink gets)
#   after       plus pro1x-music-sink (tools/), which loads the music sink on
#               MultiMedia4 (2 periods of 122880 bytes, kernel patches 0034
#               and 0035) that the stream goes to
#   after-0034  the same with q6asm-dai's old 65536-byte period limit
#   after-bt    after, with another default sink ("bt", a null sink like a
#               Bluetooth headset's, of no ALSA card): the stream must play
#               there, not on pro1x_music
# PLAYER:
#   mafw        openmediaplayer's renderer: pulsesink buffer-time 600 ms,
#               latency-time 300 ms, media.role x-maemo (mafw-gst-renderer)
#   qt          a QtMultimedia player such as gPodder: pulsesink's defaults
#               (200 ms, 10 ms), media.role music (audioRole MusicRole)
# Both get 1024-frame buffers as fast as pulsesink takes them, as a decoder
# delivers them.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
WARM=${WARM:-5}
T=${T:-/tmp/q6sim-run}
export HOME=$T/home XDG_RUNTIME_DIR=$T/rt PULSE_RUNTIME_PATH=$T/rt
export PULSE_SERVER=unix:$T/native

start() {
	sc=$1 pl=$2
	[ -f "$here/libasound_module_pcm_q6sim.so" ] &&
		[ "$here/libasound_module_pcm_q6sim.so" -nt "$here/pcm_q6sim.c" ] ||
		gcc -O2 -Wall -shared -fPIC -DPIC -o "$here/libasound_module_pcm_q6sim.so" \
			"$here/pcm_q6sim.c" -lasound
	[ -f "$here/pro1x-music-sink" ] && [ "$here/pro1x-music-sink" -nt "$here/pro1x-music-sink.c" ] ||
		gcc -O2 -Wall -o "$here/pro1x-music-sink" "$here/pro1x-music-sink.c" \
			$(pkg-config --cflags --libs libpulse)
	rm -rf "$T"
	mkdir -p "$T/home" "$T/rt"
	cat > "$T/home/.asoundrc" <<- EOF
	pcm_type.q6sim { lib "$here/libasound_module_pcm_q6sim.so" }
	pcm.mm1 { type q6sim max_period_bytes 65536 stats "$T/mm1.stats" }
	pcm.mm4 { type q6sim max_period_bytes 122880 stats "$T/mm4.stats" }
	pcm.mm4old { type q6sim max_period_bytes 65536 stats "$T/mm4.stats" }
	EOF
	# alsa.card as both sinks have it on the phone (an ioplug PCM has no
	# card); pro1x-music-sink compares them
	case $sc in
	before) helper= ;;
	after) helper="-d mm4" ;;
	after-0034) helper="-d mm4old" ;;
	after-bt) helper="-d mm4" ;;
	*) echo "unknown scenario $sc" >&2; exit 2 ;;
	esac
	{
		echo "load-module module-native-protocol-unix auth-anonymous=1 socket=$T/native"
		echo "load-module module-alsa-sink device=mm1 sink_name=hifi rate=48000 format=s16le channels=2 sink_properties=alsa.card=0"
		echo "load-module module-intended-roles"
		echo "load-module module-suspend-on-idle timeout=3"
		if [ "$sc" = after-bt ]; then
			echo "load-module module-null-sink sink_name=bt"
			echo "set-default-sink bt"
		else
			echo "set-default-sink hifi"
		fi
	} > "$T/test.pa"
	pulseaudio -n -F "$T/test.pa" --daemonize=no --exit-idle-time=-1 --disallow-exit \
		--use-pid-file=no -v --log-target=file:"$T/pa.log" < /dev/null > /dev/null 2>&1 &
	pa=$!
	i=0
	while [ ! -S "$T/native" ] && [ $i -lt 150 ]; do i=$((i + 1)); sleep 0.2; done
	ms=
	if [ -n "$helper" ]; then
		# shellcheck disable=SC2086
		"$here/pro1x-music-sink" -v $helper -x "$here" -a alsa.card=0 \
			< /dev/null > /dev/null 2> "$T/music-sink.log" &
		ms=$!
		sleep 2
	fi
	case $pl in
	mafw) set -- PULSE_PROP_media.role=x-maemo pulsesink buffer-time=600000 latency-time=300000 ;;
	qt) set -- PULSE_PROP_media.role=music pulsesink ;;
	*) echo "unknown player $pl" >&2; exit 2 ;;
	esac
	r=$1
	shift
	env "$r" gst-launch-1.0 -q audiotestsrc wave=sine freq=440 num-buffers=100000 ! \
		audio/x-raw,rate=48000,channels=2,format=S16LE ! "$@" < /dev/null > /dev/null 2>&1 &
	echo "$pa $! $ms" > "$T/pids"
	sleep "$WARM"
	s=$(pactl list sink-inputs | sed -n 's/^[[:space:]]*Sink: //p' | tail -1)
	echo "stream on: $(pactl list short sinks | awk -v i="$s" '$1 == i { print $2 }')"
	if [ "$sc" = after ] || [ "$sc" = after-0034 ]; then
		# the volume keys set the default sink's volume
		pactl set-sink-volume hifi 37%
		sleep 1
		echo "pro1x_music follows hifi's volume: $(pactl list sinks |
			awk '/^[[:space:]]*Name: / { n = $2 } n == "pro1x_music" && /^[[:space:]]*Volume:/ { print $5; exit }')"
		pactl set-sink-volume hifi 100%
	fi
}

stop() {
	[ -f "$T/pids" ] || exit 0
	set -- $(cat "$T/pids")
	kill "$2" ${3:-} 2> /dev/null || true
	sleep 1
	kill "$1" 2> /dev/null || true
	i=0
	while kill -0 "$1" 2> /dev/null && [ $i -lt 50 ]; do i=$((i + 1)); sleep 0.2; done
	rm -f "$T/pids"
	sed -n 's/.*\(Using [0-9.]* fragments of size [0-9]* bytes ([0-9.]*ms)\).*/\1/p' "$T/pa.log"
}

case ${1:-} in
start) start "$2" "$3" ;;
stop) stop ;;
*) sed -n '2,/^set -eu/p' "$0" | sed '$d'; exit 2 ;;
esac
