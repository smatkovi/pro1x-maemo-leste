#!/bin/sh
# For FreeBSD's FM sound, stage 3c (freebsd/fm/README.md): run once on the
# Pro1X under Leste, as root, with headphones plugged in. While one short
# playback goes to the headphones (the UCM profile's "Headphones" device:
# its controls are set here directly), the kernel's tracepoints record
# every regmap write and read of the LPASS RX/TX/VA macros and the WCD9370
# and the DAPM power sequence: the register sequence FreeBSD's programmer
# has to repeat. PulseAudio is suspended meanwhile, so that the card powers
# up from cold (it keeps the card open after a sound otherwise,
# docs/audio.md). Nothing is written to any partition but the output file.
#
#   sh leste-hph-trace.sh [output]      (default /host/hph-trace.txt)
set -u
inner=0
if [ "${1:-}" = --inner ]; then
	inner=1
	shift
fi
out=${1:-/host/hph-trace.txt}
card=Pro1X

T=/sys/kernel/tracing
[ -d $T/events ] || T=/sys/kernel/debug/tracing
if [ ! -d $T/events ]; then
	mount -t tracefs nodev /sys/kernel/tracing 2> /dev/null
	T=/sys/kernel/tracing
fi
[ -d $T/events/regmap ] || { echo "no regmap tracepoints under $T"; exit 1; }

play() {
	set -e
	# the "Headphones" device of ucm2/Pro1X/HiFi.conf
	for c in "RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1=1" \
	    "RX_MACRO RX0 MUX=AIF1_PB" "RX_MACRO RX1 MUX=AIF1_PB" \
	    "RX INT0_1 MIX1 INP0=RX0" "RX INT1_1 MIX1 INP0=RX1" \
	    "RX INT0_1 INTERP=RX INT0_1 MIX1" "RX INT1_1 INTERP=RX INT1_1 MIX1" \
	    "RX INT0 DEM MUX=CLSH_DSM_OUT" "RX INT1 DEM MUX=CLSH_DSM_OUT" \
	    "EAR_RDAC Switch=0" "HPHL_RDAC Switch=1" "HPHR_RDAC Switch=1" \
	    "CLSH PA Switch=1" "RX HPH Mode=CLS_AB" "HPHL Switch=1" \
	    "HPHR Switch=1" "Spk0 OUT Switch=0" "Spk1 OUT Switch=0"; do
		amixer -q -c $card cset "name=${c%%=*}" "${c#*=}"
	done
	echo 0 > $T/tracing_on
	echo > $T/trace
	echo 16384 > $T/buffer_size_kb
	for e in regmap/regmap_reg_write regmap/regmap_reg_read \
	    asoc/snd_soc_dapm_widget_power asoc/snd_soc_dapm_start \
	    asoc/snd_soc_dapm_done; do
		[ -e $T/events/$e/enable ] && echo 1 > $T/events/$e/enable
	done
	echo 1 > $T/tracing_on
	# 2 s of a 1 kHz tone (alsa-utils), else 1 s of silence
	if command -v speaker-test > /dev/null; then
		timeout 4 speaker-test -D hw:$card,0 -c 2 -r 48000 -F S16_LE \
		    -t sine -f 1000 -l 1 -s 1 > /dev/null 2>&1 || true
	else
		aplay -q -D hw:$card,0 -f S16_LE -r 48000 -c 2 -d 1 /dev/zero
	fi
	sleep 1
	echo 0 > $T/tracing_on
	for e in regmap/regmap_reg_write regmap/regmap_reg_read \
	    asoc/snd_soc_dapm_widget_power asoc/snd_soc_dapm_start \
	    asoc/snd_soc_dapm_done; do
		[ -e $T/events/$e/enable ] && echo 0 > $T/events/$e/enable
	done
	# the headphone PA switches off again
	amixer -q -c $card cset "name=HPHL Switch" 0
	amixer -q -c $card cset "name=HPHR Switch" 0
}

# PulseAudio suspended: this script again, inside pasuspender (as
# PulseAudio's user, if root has no server of its own)
if [ $inner = 0 ] && command -v pasuspender > /dev/null &&
    pu=$(ps -o user= -C pulseaudio 2> /dev/null | head -1) && [ -n "$pu" ]; then
	if [ "$pu" = "$(id -un)" ]; then
		pasuspender -- sh "$0" --inner "$out" && exit 0
	else
		su "$pu" -c "pasuspender -- sudo sh '$0' --inner '$out'" && exit 0
	fi
	echo "pasuspender did not work: playing directly (stop PulseAudio if the card is busy)"
fi
{
	echo "# $(uname -r), $(date -Is)"
	amixer -c $card contents 2> /dev/null | grep -c numid | sed 's/^/# controls: /'
} > "$out"
( play ) || echo "the playback failed (is $card free?)"
cat $T/trace >> "$out"
echo "$(grep -c regmap_reg_write "$out") register writes, $(grep -c \
    snd_soc_dapm_widget_power "$out") widget changes in $out"
