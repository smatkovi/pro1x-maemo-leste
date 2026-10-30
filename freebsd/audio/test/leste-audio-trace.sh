#!/bin/sh
# For FreeBSD's sound (freebsd/audio/README.md, and the FM radio's stage
# 3c, freebsd/fm/README.md): run once on the Pro1X under Leste, as root,
# headphones plugged in. For each scenario (the UCM profile's devices of
# ucm2/Pro1X/HiFi.conf, their controls set here directly) it lets the card
# go idle until runtime PM has suspended the macros, then plays (or
# records) 3 s of digital silence from cold and waits until everything is
# suspended again. Meanwhile the kernel's tracepoints record
#   - every register write and read of the LPASS RX/TX/VA macros, the
#     WCD9370 (its SoundWire regmap) and the AW87339s (regmap/*, i2c/*),
#     with the regcache syncs of the runtime resumes,
#   - the DAPM power sequence (asoc/*),
#   - the ADSP's clocks as Linux's clock framework sees them (clk/*: the
#     q6afe clocks and the macros' fsgen/mclk),
#   - the APR packets' sizes and order on GLINK (qcom_glink/*, the channel
#     apr_audio_svc): AFE, ADM and ASM commands are told apart by size,
#   - the speaker amplifiers' enable lines and the codec's reset (gpio/*),
#     the rails (regulator/*);
# and one second into the sound a snapshot of the state FreeBSD has to
# reach: every regmap's registers (debugfs), the SoundWire masters' own
# registers (qcom.c's "qualcomm-registers": the master's writel()s are not
# traced) and the slaves' SCP/DPn registers the SoundWire core programmed,
# the DAPM widgets, the clocks and rails, the PCM's hw_params.
# PulseAudio is suspended meanwhile (pasuspender), so that each scenario
# starts from cold. Nothing is written to any partition but the output.
# The loudspeaker and earpiece scenarios play silence: at most a click.
#
#   sh leste-audio-trace.sh [scenarios] [output]
#       scenarios: comma separated, default hph,spk,ear,mic
#         hph   headphones (HPHL/HPHR, class AB)     MultiMedia1 playback
#         spk   loudspeakers (HPH + both AW87339)    MultiMedia1 playback
#         ear   earpiece (EAR PA from RX1, HPHL)     MultiMedia1 playback
#         mic   built-in microphone (VA DMIC3)       MultiMedia2 capture
#         probe the sound card unbound and bound again (its components'
#               probe-time writes; sound may need a reboot afterwards)
#       output: default /host/audio-trace.txt
#
# freebsd/audio/tools/trace2seq.py turns the output into the replay
# sequences of freebsd/audio/seq (README, "The sequences").
set -u
inner=0
if [ "${1:-}" = --inner ]; then
	inner=1
	shift
fi
scenarios=${1:-hph,spk,ear,mic}
out=${2:-/host/audio-trace.txt}
# freebsd/fm/test/leste-hph-trace.sh is this script: its one argument is
# the output
case $scenarios in
*/*) out=$scenarios; scenarios=hph,spk,ear,mic ;;
esac

# the card: Pro1X by its id, else the first card
card=$(sed -n 's/^ *[0-9][0-9]* \[\([^] ]*\) *\]:.*/\1/p' /proc/asound/cards |
    grep -x Pro1X | head -1)
[ -n "$card" ] || card=$(sed -n 's/^ *\([0-9][0-9]*\) \[.*/\1/p' \
    /proc/asound/cards | head -1)
[ -n "$card" ] || { echo "no sound card in /proc/asound/cards"; exit 1; }
cardno=$(sed -n "s/^ *\([0-9][0-9]*\) \[$card *\]:.*/\1/p" /proc/asound/cards)
[ -n "$cardno" ] || cardno=$card

D=/sys/kernel/debug
[ -d $D/regmap ] || mount -t debugfs nodev $D 2> /dev/null
T=/sys/kernel/tracing
[ -d $T/events ] || T=$D/tracing
if [ ! -d $T/events ]; then
	mount -t tracefs nodev /sys/kernel/tracing 2> /dev/null
	T=/sys/kernel/tracing
fi
[ -d $T/events/regmap ] || { echo "no regmap tracepoints under $T"; exit 1; }

# the controls of each scenario (ucm2/Pro1X/HiFi.conf: EnableSequence,
# DisableSequence); "Spk0/Spk1 OUT Switch" are the amplifiers' pin switches
common_on="RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1=1
RX_MACRO RX0 MUX=AIF1_PB
RX_MACRO RX1 MUX=AIF1_PB
RX INT0_1 MIX1 INP0=RX0
RX INT1_1 MIX1 INP0=RX1
RX INT0_1 INTERP=RX INT0_1 MIX1
RX INT1_1 INTERP=RX INT1_1 MIX1
RX INT0 DEM MUX=CLSH_DSM_OUT
RX INT1 DEM MUX=CLSH_DSM_OUT"
common_off="RX INT0 DEM MUX=NORMAL_DSM_OUT
RX INT1 DEM MUX=NORMAL_DSM_OUT
RX INT0_1 INTERP=ZERO
RX INT1_1 INTERP=ZERO
RX INT0_1 MIX1 INP0=ZERO
RX INT1_1 MIX1 INP0=ZERO
RX_MACRO RX0 MUX=ZERO
RX_MACRO RX1 MUX=ZERO"
controls() {	# scenario on|off
	case $1-$2 in
	hph-on) printf '%s\n' "$common_on" "EAR_RDAC Switch=0" \
	    "HPHL_RDAC Switch=1" "HPHR_RDAC Switch=1" "CLSH PA Switch=1" \
	    "RX HPH Mode=CLS_AB" "HPHL Switch=1" "HPHR Switch=1" \
	    "Spk0 OUT Switch=0" "Spk1 OUT Switch=0" ;;
	hph-off) printf '%s\n' "HPHL Switch=0" "HPHR Switch=0" \
	    "HPHL_RDAC Switch=0" "HPHR_RDAC Switch=0" "$common_off" ;;
	spk-on) printf '%s\n' "$common_on" "EAR_RDAC Switch=0" \
	    "HPHL_RDAC Switch=1" "HPHR_RDAC Switch=1" "CLSH PA Switch=1" \
	    "RX HPH Mode=CLS_AB" "HPHL Switch=1" "HPHR Switch=1" \
	    "Spk0 OUT Switch=1" "Spk1 OUT Switch=1" ;;
	spk-off) printf '%s\n' "Spk0 OUT Switch=0" "Spk1 OUT Switch=0" \
	    "HPHL Switch=0" "HPHR Switch=0" "HPHL_RDAC Switch=0" \
	    "HPHR_RDAC Switch=0" "$common_off" ;;
	ear-on) printf '%s\n' "$common_on" "HPHL_RDAC Switch=1" \
	    "HPHR_RDAC Switch=0" "CLSH PA Switch=1" "RX HPH Mode=CLS_AB" \
	    "HPHR Switch=0" "HPHL Volume=12" "RDAC3_MUX=RX1" \
	    "EAR_RDAC Switch=1" "EAR_PA Volume=8" "Spk0 OUT Switch=0" \
	    "Spk1 OUT Switch=0" "HPHL Switch=1" ;;
	ear-off) printf '%s\n' "EAR_RDAC Switch=0" "HPHL Switch=0" \
	    "HPHL_RDAC Switch=0" "CLSH PA Switch=0" "$common_off" \
	    "HPHL Volume=12" ;;
	mic-on) printf '%s\n' "MultiMedia2 Mixer VA_CODEC_DMA_TX_0=1" \
	    "VA DEC0 MUX=VA_DMIC" "VA DMIC MUX0=DMIC3" \
	    "VA_AIF1_CAP Mixer DEC0=1" "VA_DEC0 Volume=84" ;;
	mic-off) printf '%s\n' "VA_AIF1_CAP Mixer DEC0=0" "VA DMIC MUX0=ZERO" ;;
	esac
}
set_controls() {
	controls "$1" "$2" | while IFS= read -r c; do
		[ -n "$c" ] || continue
		if amixer -q -c "$card" cset "name=${c%%=*}" "${c#*=}" 2>&1; then
			echo "cset ${c%%=*} = ${c#*=}"
		else
			echo "cset ${c%%=*} = ${c#*=}: failed"
		fi
	done
}

# the devices whose runtime state tells that the card is cold or warm
pmdevs="a600000.codec a620000.codec a730000.codec a610000.soundwire a740000.soundwire"
pmstate() {
	for d in $pmdevs; do
		f=/sys/bus/platform/devices/$d/power/runtime_status
		[ -r $f ] && printf '%s=%s ' "$d" "$(cat $f)"
	done
	echo
}
wait_idle() {	# up to 30 s for every macro and master to suspend
	i=0
	while [ $i -lt 60 ]; do
		busy=0
		for d in $pmdevs; do
			f=/sys/bus/platform/devices/$d/power/runtime_status
			[ -r $f ] && [ "$(cat $f)" != suspended ] && busy=1
		done
		[ $busy = 0 ] && return 0
		sleep 0.5
		i=$((i + 1))
	done
	return 1
}

# gpio numbers in the trace are global: the TLMM's base plus the pin
gpio_filter() {
	base=$(sed -n 's/^gpiochip[0-9]*: GPIOs \([0-9]*\)-.*500000\.pinctrl.*/\1/p' \
	    $D/gpio 2> /dev/null | head -1)
	base=${base:-0}
	# AW87339 enable lines 85, 98; the WCD9370's reset 92 (device tree)
	echo "gpio == $((base + 85)) || gpio == $((base + 98)) || gpio == $((base + 92))"
}

EVENTS="regmap/regmap_reg_write regmap/regmap_reg_read regmap/regmap_bulk_write
regmap/regmap_hw_write_start regmap/regcache_sync regmap/regmap_cache_only
regmap/regmap_cache_bypass asoc/snd_soc_dapm_start asoc/snd_soc_dapm_done
asoc/snd_soc_dapm_widget_power asoc/snd_soc_dapm_widget_event_start
asoc/snd_soc_dapm_widget_event_done asoc/snd_soc_bias_level_start
asoc/snd_soc_dapm_walk_done clk/clk_prepare clk/clk_unprepare clk/clk_enable
clk/clk_disable clk/clk_set_rate qcom_glink/qcom_glink_cmd_tx_data
qcom_glink/qcom_glink_cmd_intent qcom_glink/qcom_glink_cmd_rx_done
i2c/i2c_write i2c/i2c_read i2c/i2c_reply i2c/i2c_result gpio/gpio_value
gpio/gpio_direction regulator/regulator_enable regulator/regulator_disable
regulator/regulator_set_voltage"
events() {	# 1 on, 0 off
	for e in $EVENTS; do
		[ -e $T/events/$e/enable ] || continue
		case $e in
		i2c/*) [ $1 = 1 ] && echo "addr == 0x59 || addr == 0x5a" > \
		    $T/events/$e/filter 2> /dev/null ;;
		gpio/*) [ $1 = 1 ] && gpio_filter > $T/events/$e/filter 2> /dev/null ;;
		esac
		echo $1 > $T/events/$e/enable
		[ $1 = 0 ] && echo 0 > $T/events/$e/filter 2> /dev/null
	done
}
mark() {
	echo "pro1x-audio: $*" > $T/trace_marker 2> /dev/null
}

# the state while the sound runs: what FreeBSD has to reach
dump() {
	echo "--- state ($1)"
	echo ">>> runtime pm"
	pmstate
	for r in $D/regmap/*; do
		case ${r##*/} in
		a600000.codec*|a620000.codec*|a730000.codec*|sdw:*|*-0059*|*-005a*) ;;
		*) continue ;;
		esac
		echo ">>> $r/registers"
		cat "$r/registers" 2> /dev/null
		[ -r "$r/name" ] && echo ">>> $r/name $(cat "$r/name")"
	done
	for m in $D/soundwire/master-*; do
		[ -d "$m" ] || continue
		echo ">>> $m/qualcomm-sdw/qualcomm-registers"
		cat "$m/qualcomm-sdw/qualcomm-registers" 2> /dev/null
		for s in "$m"/*; do
			[ -f "$s/registers" ] || continue
			echo ">>> $s/registers"
			cat "$s/registers" 2> /dev/null
		done
	done
	for f in $D/asoc/*/dapm/* $D/asoc/*/*/dapm/*; do
		[ -f "$f" ] || continue
		case $(head -1 "$f" 2> /dev/null) in
		*": On "*|*": On") echo ">>> $f"; cat "$f" ;;
		esac
	done
	echo ">>> clk_summary (audio)"
	grep -i -E 'lpass|q6afe|mclk|npl|fsgen|dcodec|vote|swr' \
	    $D/clk/clk_summary 2> /dev/null
	echo ">>> regulator_summary"
	cat $D/regulator/regulator_summary 2> /dev/null
	echo ">>> gpio"
	grep -E 'gpio(85|92|98)\b|gpio-(85|92|98)\b' $D/gpio 2> /dev/null
	echo ">>> lpass lpi pins"
	for p in $D/pinctrl/a7c0000.pinctrl*; do
		[ -d "$p" ] && cat "$p/pinmux-pins" "$p/pinconf-pins" 2> /dev/null
	done
	for p in /proc/asound/card$cardno/pcm*/sub0; do
		[ -r "$p/hw_params" ] || continue
		echo ">>> $p"
		cat "$p/hw_params" "$p/status" 2> /dev/null
	done
}

scenario() {	# hph|spk|ear|mic
	sc=$1
	echo "=== scenario $sc"
	echo "--- controls on"
	set_controls "$sc" on
	echo "--- before: $(pmstate)"
	wait_idle || echo "--- not idle after 30 s: $(pmstate)"
	echo 0 > $T/tracing_on
	echo > $T/trace
	echo 16384 > $T/buffer_size_kb
	events 1
	echo 1 > $T/tracing_on
	mark "$sc start (cold: $(pmstate))"
	sleep 0.5
	snap=/tmp/pro1x-audio-trace.state.$$
	( sleep 1.5; dump "$sc, 1 s into the sound" > $snap 2>&1 ) &
	dumper=$!
	mark "$sc stream on"
	t0=$(date +%s.%N)
	case $sc in
	mic)
		arecord -q -D hw:$card,1 -t raw -f S16_LE -r 48000 -c 2 -d 3 \
		    /tmp/pro1x-audio-trace.raw ;;
	*)
		aplay -q -D hw:$card,0 -t raw -f S16_LE -r 48000 -c 2 -d 3 \
		    /dev/zero ;;
	esac
	st=$?
	t1=$(date +%s.%N)
	mark "$sc stream off (exit $st)"
	wait $dumper
	# down to suspended again: the power-down sequence and the suspends
	wait_idle || mark "$sc not idle again after 30 s"
	sleep 1
	mark "$sc end ($(pmstate))"
	echo 0 > $T/tracing_on
	events 0
	echo "--- stream: exit $st, from $t0 to $t1"
	if [ $sc = mic ] && command -v python3 > /dev/null; then
		python3 -I -c 'import array,sys
a=array.array("h"); a.frombytes(open(sys.argv[1],"rb").read())
l=a[0::2]; r=a[1::2]
print("--- recorded: %d frames, peak left %d right %d" % (len(l),
    max(map(abs,l)) if l else 0, max(map(abs,r)) if r else 0))' \
		    /tmp/pro1x-audio-trace.raw 2>&1
	fi
	rm -f /tmp/pro1x-audio-trace.raw
	echo "--- trace"
	grep -v '^#' $T/trace
	cat $snap
	rm -f $snap
	echo "--- controls off"
	set_controls "$sc" off
	echo "--- after: $(pmstate)"
}

probe() {	# the card's components probed again, under the trace
	echo "=== scenario probe"
	drv=/sys/bus/platform/drivers/snd-sm8250
	dev=$(ls -d $drv/sound* 2> /dev/null | head -1)
	[ -n "$dev" ] || { echo "--- no card under $drv"; return; }
	dev=${dev##*/}
	echo 0 > $T/tracing_on
	echo > $T/trace
	echo 16384 > $T/buffer_size_kb
	events 1
	echo 1 > $T/tracing_on
	mark "probe unbind $dev"
	echo "$dev" > $drv/unbind
	sleep 2
	mark "probe bind $dev"
	echo "$dev" > $drv/bind
	sleep 5
	mark "probe end"
	echo 0 > $T/tracing_on
	events 0
	echo "--- card: $(cat /proc/asound/cards | tr '\n' ' ')"
	echo "--- trace"
	grep -v '^#' $T/trace
}

# PulseAudio suspended: this script again, inside pasuspender (as
# PulseAudio's user, if root has no server of its own)
if [ $inner = 0 ] && command -v pasuspender > /dev/null &&
    pu=$(ps -o user= -C pulseaudio 2> /dev/null | head -1) && [ -n "$pu" ]; then
	if [ "$pu" = "$(id -un)" ]; then
		pasuspender -- sh "$0" --inner "$scenarios" "$out" && exit 0
	else
		su "$pu" -c "pasuspender -- sudo sh '$0' --inner '$scenarios' '$out'" &&
		    exit 0
	fi
	echo "pasuspender did not work: running directly (stop PulseAudio if the card is busy)"
fi
{
	echo "# leste-audio-trace v1: $(uname -r), $(date -Is), card $card ($cardno)"
	echo "# scenarios: $scenarios"
	echo "# regmaps: $(ls $D/regmap 2> /dev/null | tr '\n' ' ')"
	echo "# soundwire: $(ls $D/soundwire 2> /dev/null | tr '\n' ' ') $(ls $D/soundwire/master-* 2> /dev/null | tr '\n' ' ')"
	echo "# events: $(for e in $EVENTS; do [ -e $T/events/$e ] && printf '%s ' $e; done)"
	echo "# controls: $(amixer -c "$card" controls 2> /dev/null | wc -l)"
	echo "--- amixer contents (before)"
	amixer -c "$card" contents 2> /dev/null
	echo "--- state (idle, before)"
	dump idle
} > "$out"
for sc in $(echo "$scenarios" | tr ',' ' '); do
	case $sc in
	hph|spk|ear|mic) scenario $sc >> "$out" 2>&1 ;;
	probe) probe >> "$out" 2>&1 ;;
	*) echo "unknown scenario $sc" ;;
	esac
	echo "$sc: $(sed -n "/^=== scenario $sc\$/,/^=== scenario/p" "$out" |
	    grep -c regmap_reg_write) register writes"
done
echo "$(grep -c regmap_reg_write "$out") register writes, $(grep -c \
    snd_soc_dapm_widget_power "$out") widget changes, $(grep -c \
    'apr_audio_svc' "$out") APR packets in $out"
