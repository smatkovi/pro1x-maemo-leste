# Sound under FreeBSD

The Pro1X's sound is not a device of the application processor: the
codecs (the LPASS RX, TX and VA macros, the WCD9370 behind two SoundWire
masters) are clocked and fed by the audio DSP (ADSP), and the PCM data
reaches them only through the ADSP's audio services over APR (Leste's
`docs/audio.md`, the device tree's `sound` node). This directory builds
FreeBSD's sound on the pieces `freebsd/dsp` (the ADSP, GLINK, `/dev/apr`)
and `freebsd/fm` (the LPASS register windows `/dev/qcom_lpass`, the AFE
over APR) already have.

Nothing here has run on the phone (unreachable until 2026-10-31).

## The Leste trace (2026-10-31, first)

Everything of the codec path that Linux does at run time, in the order
it does it, is recorded once under Leste and replayed by FreeBSD. On the
phone under Leste, as root, headphones plugged in:

    scp freebsd/audio/test/leste-audio-trace.sh root@172.16.42.1:/tmp/
    sh /tmp/leste-audio-trace.sh                 # hph,spk,ear,mic -> /host/audio-trace.txt
    sh /tmp/leste-audio-trace.sh probe /host/audio-trace-probe.txt   # optional, last: rebinds the card

(`freebsd/fm/test/leste-hph-trace.sh` is a link to the same script; its
one argument is the output file.) It takes about two minutes: per
scenario the controls of the UCM device (`ucm2/Pro1X/HiFi.conf`), up to
30 s until runtime PM has suspended the macros and SoundWire masters,
3 s of digital silence played (or recorded, `mic`) from cold, the wait
until everything is suspended again. The loudspeaker and earpiece runs
play silence: at most a click. PulseAudio is held off with
`pasuspender` meanwhile. Per scenario the output has

- `--- trace`: ftrace of `regmap/*` (every hardware write of the macros,
  the WCD9370's SoundWire regmap, the AW87339s; the regcache syncs of
  the runtime resumes, which carry everything the drivers set at
  probe), `asoc/*` (DAPM), `clk/*` (the q6afe clocks and the macros'
  `fsgen`/`mclk`), `qcom_glink/*` (the APR packets' sizes and order on
  `apr_audio_svc`), `i2c/*` and `gpio/*` (the amplifiers), `regulator/*`;
- `--- state`: one second into the sound, every regmap's registers, the
  SoundWire masters' own registers (`qualcomm-registers`: the master's
  `writel()`s are not traced) and the slaves' SCP/DPn registers that the
  SoundWire core programmed, the powered DAPM widgets, the audio clocks,
  the rails, the LPASS pins, the PCM's `hw_params`;
- `mic`: the peak of what was recorded (a sanity check of the microphone
  under Linux).

The output comes back to the PC (`scp root@172.16.42.1:/host/audio-trace.txt
$WORK/audio/`) for `tools/trace2seq.py` (below).

## Plan

| Stage | What | Here |
| --- | --- | --- |
| 1 | playback: ASM/ADM/AFE over APR to `RX_CODEC_DMA_RX_0`, the RX macro, SoundWire, the WCD9370 (HPHL/HPHR, EAR), the AW87339s (patch 0026's order) | `pro1x-audiod`, sequences from the trace |
| 2 | capture: VA macro DMIC3 (and TX macro, the WCD's analog mics) to `VA_CODEC_DMA_TX_0` | the same, `mic` sequences |
| 3 | sound(4): `/dev/dsp`, `/dev/mixer` | `snd_pro1x.ko` (kernel) + `pro1x-audiod` |
| 4 | Hildon (PulseAudio's OSS module, the volume keys and bar), calls | config, `voice` for the modem agent |

The rest of this file grows with the stages.
