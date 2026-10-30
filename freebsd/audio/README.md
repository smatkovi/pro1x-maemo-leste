# Sound under FreeBSD

The Pro1X's sound is not a device of the application processor: the
codecs (the LPASS RX, TX and VA macros, the WCD9370 behind two SoundWire
masters) are clocked and fed by the audio DSP (ADSP), and the PCM data
reaches them only through the ADSP's audio services over APR (Leste's
`docs/audio.md`, the device tree's `sound` node). This directory builds
FreeBSD's sound on the pieces `freebsd/dsp` (the ADSP, GLINK, `/dev/apr`)
and `freebsd/fm` (the LPASS register windows `/dev/qcom_lpass`, the AFE
over APR) already have.

| Stage | What | Here | State |
| --- | --- | --- | --- |
| 1 | playback: ASM/ADM/AFE over APR to `RX_CODEC_DMA_RX_0`, the RX macro, SoundWire, the WCD9370 (HPHL/HPHR, EAR), the AW87339s (patch 0026's order) | `pro1x-audiod` (`q6.c`, `pcm.c`, `backend.c`, `swr.c`), sequences from the Leste trace (`trace2seq.py`) | the ADSP's side and SoundWire written from Linux's code, host tested; the codec's register sequences wait for the trace (2026-10-31) |
| 2 | capture: VA macro DMIC3 to `VA_CODEC_DMA_TX_0` (the headset's analog mic on `TX_CODEC_DMA_TX_3`) | the same, the `mic` sequences | as stage 1 |
| 3 | sound(4): `/dev/dsp`, `/dev/mixer` | `snd_pro1x.ko` (kernel) + `pro1x-audiod` | built; tried in the VM with a simulated ADSP |
| 4 | Hildon: PulseAudio on `/dev/dsp`, the volume keys and bar; calls | `pro1x-volume-keys`; `voice` for the modem agent (`pro1x-callaudio`) | built; voice host tested |
| jack | headphones and headsets: the WCD9370's MBHC, the routes following the plug, the headset's buttons; events for devd, evdev and the socket; its interrupts as SoundWire alerts, the bus' clock stopped between them | `mbhc.c` (Linux's wcd-mbhc-v2.c), `swr.c`'s interrupts, `qcom_swr.ko`, `[jack-up]` from the trace's `jack` scenario | built; host tested against a simulated MBHC and a simulated master with alerts and clock stop; devd event tried in the VM |

Nothing here has run on the phone (unreachable until 2026-10-31).
Nothing is heard before the trace's sequences: without them the ADSP
plays into the codec DMA with no codec path behind it.

## The Leste trace (2026-10-31, first)

Everything of the codec path that Linux does at run time, in the order
it does it, is recorded once under Leste and replayed by FreeBSD. On the
phone under Leste, as root, headphones plugged in:

    scp freebsd/audio/test/leste-audio-trace.sh root@172.16.42.1:/tmp/
    sh /tmp/leste-audio-trace.sh                 # hph,spk,ear,mic -> /host/audio-trace.txt
    sh /tmp/leste-audio-trace.sh jack /host/audio-trace-jack.txt     # by hand: unplug, plug when told
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
  the WCD9370's SoundWire regmap, the AW87339s, the LPASS clock
  controllers' CGCR resets; the regcache syncs of the runtime resumes,
  which carry what the drivers set at probe), `asoc/*` (DAPM), `clk/*`
  (the q6afe clocks and votes), `qcom_glink/*` (the APR packets' sizes and
  order on `apr_audio_svc`: AFE, ADM and ASM commands told apart by size),
  `i2c/*` and `gpio/*` (the amplifiers), `regulator/*`;
- `--- state`: one second into the sound, every regmap's registers, the
  SoundWire masters' own registers (`qualcomm-registers`: the master's
  `writel()`s are not traced) and the slaves' SCP/DPn registers that the
  SoundWire core programmed, the powered DAPM widgets, the audio clocks,
  the rails, the LPASS pins, the PCM's `hw_params`;
- `mic`: the peak of what was recorded (the microphone under Linux).

Back on the PC:

    scp root@172.16.42.1:/host/audio-trace.txt $WORK/audio/
    python3 -I freebsd/audio/tools/trace2seq.py $WORK/audio/audio-trace.txt \
        $WORK/audio/audio-trace-jack.txt \
        -o freebsd/rootfs/usr/local/share/pro1x/audio/pro1x-audio.seq
    cp freebsd/rootfs/usr/local/share/pro1x/audio/pro1x-audio.seq $WORK/audio-test/audio/

The notes trace2seq prints (`note: ...`) say what it could not place:
writes to devices without a target, a scenario without the AFE port's
packets on GLINK, no slave registers for the SoundWire stream. The file
is read by `pro1x-audiod` at start (`pro1x-audioctl reload` again); the
phone takes `/host/freebsd/audio/pro1x-audio.seq` before the image's.

## Why a kernel driver and a daemon

sound(4) gives the OSS API (`/dev/dsp`, `/dev/mixer`), format and rate
conversion, mixing of several players (vchans) and the mmap PulseAudio's
OSS module may use, all in the kernel: what a driver has to give is a
channel with a hardware buffer and an interrupt per block. A user space
OSS device (cuse) would have to do all of that itself. But every packet to
the ADSP, the PCM's included, is APR on one GLINK channel, and the codec
path is a few thousand register writes in sequences that only a trace
from Linux makes exact; keeping both in one user space process
(`pro1x-audiod`) keeps `qcom_apr.c` (`/dev/apr`, one user) unchanged,
keeps every packet in host-tested code as `pro1x-fmaudio`'s are, and a
newer daemon or sequence file on userdata needs no new kernel.

So the split is: the kernel is sound(4)'s DMA engine whose hardware is the
daemon; the daemon moves the blocks through the ADSP.

## sound(4): snd_pro1x.ko

`sys/arm64/qualcomm/audio/snd_pro1x.c` attaches to the device tree's
sound card (`fxtec,pro1x-sndcard`) as `pcm0`, after `sound.ko` (the PRO1X
kernel has no `device sound`; `build.sh` builds it as a module, like
`usb/usb`):

- one playback and one recording channel, 48 kHz, 16 bit; playback
  stereo, recording mono or stereo (sound(4) converts everything else).
  Blocks are multiples of 480 frames (Linux's q6asm-dai constraint):
  playback 40-100 ms in up to 8 blocks, recording 3840 bytes (Linux's
  capture periods are at most 4 KiB) in 8 blocks;
- each hardware buffer is 64 KiB, physically contiguous, below 4 GiB and
  uncached (`bus_dmamem_alloc` with `BUS_DMA_COHERENT` on a non-coherent
  tag): the ADSP reads and writes it by physical address. sound(4)'s
  mmap maps a channel's soft buffer, never this one, so the uncached
  attribute never reaches user space. The ADSP's address is the physical
  one with the SMMU stream's low bits above bit 32 (Linux's
  `q6asm_dai_probe`: `iommus` of `qcom,q6asm-dais`, 0x1c1 & 0xf = 1), and
  that stream is switched to translation off at attach
  (`qcom_smmu_bypass`, `freebsd/dsp`);
- `/dev/pro1x_pcm` (one opener, `pro1x_pcm.h`): `read(2)` gives events
  (the buffers at open, a channel's start with its block size, count,
  how many blocks sound(4) has filled, its stop; mixer levels, the
  recording source), `PRO1X_PCM_ADVANCE` says the ADSP is done with
  blocks: the driver moves the channel's pointer and runs sound(4)'s
  interrupt (`chn_intr`) once per block (its delta is modulo the buffer,
  so a whole ring at once would be lost) and answers how many blocks are
  filled from there on; `PRO1X_PCM_FAIL` hands a channel to the clock;
- without the daemon (the device closed, or FAIL) a clock in the driver
  consumes (or fills with silence) one block per block time, so writers
  never hang; a daemon that opens later takes running channels over;
- the mixer: PCM in software (`SD_F_SOFTPCMVOL`); the master volume
  (`vol`), `speaker`, `phout` (earpiece), `mic`, `line` (the headset's
  microphone), `radio` (FM) and the recording source go to the daemon
  as events (it applies the master volume as the RX macro's digital gain;
  the others are kept, the route is the daemon's `route` request);
- `sysctl dev.pcm.0.pro1x`: the channels, who drives them, block counts.

Playback hands the ADSP only blocks sound(4) has filled. sound(4) fills
the hardware buffer from the channel's soft buffer after each interrupt
(silence when the writer is late), but a hardware channel that is the
parent of vchans keeps only two blocks of it ready (the VM showed
`ready at most 2`): that is the playback latency, 80 ms with 40 ms
blocks, and the daemon has one block time to answer each done event.

## pro1x-audiod

`tools/pro1x-audiod` (`main.c` the loop and the control socket):

**APR** (`apr.c`): headers as Linux's `apr_send_pkt` completes them;
transactions wait for `APR_BASIC_RSP_RESULT` of their opcode and token (or
for the answer's own opcode), and every other packet meanwhile goes to the
dispatcher: the ASM data events of a running stream arrive while another
back end is set up.

**A stream** (`q6.c`, `pcm.c`), as Linux's `q6asm_dai_prepare`,
`q6routing_stream_open` and `q6asm_dai_trigger` send it, for MultiMedia1
(session 1, playback) and MultiMedia2 (session 2, capture):
the back end first (below), then `ASM_CMD_SHARED_MEM_MAP_REGIONS` (one
region, 4 KiB aligned, the handle from the answer), `OPEN_WRITE_V3` (PCM
into the matrix, no post-processing) or `OPEN_READ_V3`, the ADM's COPP on
the back end's AFE port (`ADM_CMD_DEVICE_OPEN_V5`, NULL topology, 48 kHz,
2 channels, 16 bit: the back end's `hw_params` after
`sm8250_be_hw_params_fixup`) and `MATRIX_MAP_ROUTINGS_V5`, then
`MEDIA_FMT_UPDATE_V2` (or the encoder's `SET_ENCDEC_PARAM`), the blocks
(`WRITE_V2`/`READ_V2`, token the block, as `q6asm_write_async`/
`q6asm_read`) and `RUN_V2`. Each `WRITE_DONE`/`READ_DONE` is one ADVANCE
and the next block. A stop: `CLOSE`, the unmap, the COPP closed (Linux's
order). A refused step undoes the ones before and gives the channel to
the kernel's clock.

**Back ends** (`backend.c`): `rx` (`RX_CODEC_DMA_RX_0`, token 113), `va`
(`VA_CODEC_DMA_TX_0`, 110), `tx` (`TX_CODEC_DMA_TX_3`, 120, the headset's
microphone). Bringing one up: the ADSP's core state (`q6core`), the
sequences `power` (once), `<backend>-up`, `<route>-up` (`spk`, `hph`,
`ear`; `mic`, `amic`), `volume`, and the AFE port (codec DMA config, 48
kHz, 16 bit, 2 channels, mask 3, then `DEVICE_START`) where a sequence
says `afe start`, else after `<backend>-up` (Linux's back end prepares
before DAPM's stream event). Down 3 s after the last user (`-t`): the
AFE port first (Linux's back end shutdown), then `<route>-down`,
`<backend>-down`. A route change while playing runs the old route's down
and the new one's up.

**SoundWire** (`swr.c`): both masters (soundwire-v1.6.0) polled through
`/dev/qcom_lpass`, from Linux's `qcom.c` and the SoundWire core: init
(frame 50 x 16, the hardware's auto enumeration, command retries, the
component enable, the frame generator), the enumeration (the device
numbers the hardware assigned, their 48-bit ids, the one with the device
tree's unique id: 3 the WCD9370's TX device on swr0, which also carries
its registers; 4 its RX device on swr1), the command FIFO (one byte per
command, ids 1-14 then 0, broadcasts with id 15 waiting for
`SPECIAL_CMD_ID_FINISHED`), a stream's ports on master and slave from the
device tree's `qcom,ports-*` tables (the WCD937x's ports are SIMPLE with
lane control and no prepare handshake) and the bank switch. The master's
interrupt (`swr_irq`, Linux's `qcom_swrm_irq_handler`): a slave's alert
(its `SCP_INT1`, the implementation-defined bit to the WCD's handler,
acknowledged), status changes (a device back: `SCP_IntMask1` unmasked
again, as `sdw_initialize_slave`), command errors, clashes; the clock
stop and start of Linux's runtime PM with clock stop (`swr_clock_stop`:
ClockStopNow by broadcast, no prepare for a simple clock stop slave;
`swr_clock_start`: the bus clock, the clash unmasked, the frame
generator). The lines are `qcom_swr.ko`'s (below).

**The WCD9370's probe** (`backend.c` `wcd_init`, sequence line `swr
wcd-init`): `wcd937x_io_init` with its efuse-dependent branches and the
interrupt levels, once; no run-time trace shows it.

**FM** (`fm on|off`): the FM radio's sound (`freebsd/fm`, stage 3b) as
the daemon's: SLIMBUS_8_TX configured (channels 160/161) and started, its
loopback into `RX_CODEC_DMA_RX_0` with the gain, the `rx` back end behind
it: what `pro1x-fmaudio afe` sends, through the one `/dev/apr`. The
SLIMbus side (the NGD, the WCN3950's stream) stays `pro1x-fmaudio`'s,
which cannot run its AFE part while the daemon holds `/dev/apr`: stage 3c
of the FM radio is the daemon's `fm on` with the trace's `hph`/`spk`
sequences.

**Voice** (`voice.c`, `voice start|stop`): Linux's q6voice as KolibriOS
runs it on this phone (`~/ps/kolibrios-arm` `kernel/modem/apr.c`,
`docs/pro1x-audio.md`): MVM `CREATE_PASSIVE_CONTROL_SESSION` "11C05000"
(the handle is the answer's source port), `SET_POLICY_DUAL_CONTROL`, CVP
`CREATE_FULL_CONTROL_SESSION_V2` (tx `VA_CODEC_DMA_TX_0` without
topology, rx `RX_CODEC_DMA_RX_0` with `RX_DEFAULT`, `EC_INT_MIXING`; the
device tree asks for no CVD 2.3), CVP `ENABLE`, MVM `ATTACH_VOCPROC`,
`START_VOICE`; then the `rx` and `va` back ends (Linux's back ends prepare
after the front end's startup). Down: `STOP_VOICE`, `DETACH_VOCPROC`, CVP
`DISABLE`, `DESTROY_SESSION` for CVP and MVM. KolibriOS found the session
to work only when started while the call exists; the modem agent's
`pro1x-callaudio` starts it then.

Calls (freebsd/modem, stage 2): pro1x-modemd runs `pro1x-callaudio` while
a call exists; it sends `status`, `route ear` and `voice start` on this
daemon's control socket, `route spk`/`route ear` on SIGUSR1, and `voice
stop` and the route of before after the last call. It needs the earpiece
(`ear`) and the microphone (`mic`, VA DEC0 to `VA_CODEC_DMA_TX_0`) paths
for the voice session's back ends; the modem's device run `pro1x.modem=call`
starts the ADSP and `rc.d/pro1x_audiod` when `pro1x-audioctl status` does
not answer.

**The control socket** (`/var/run/pro1x-audio.sock`, `pro1x-audioctl`):
one line per request and answer, `ok ...` or `error ERRNO ...`:

    status                     ok play=running rec=idle route=spk rec_route=mic rx=up(1) va=down(0) ...
    route spk|hph|ear          the playback path
    rec mic|amic               the capture path (from the next recording)
    volume 0-100               the master volume (also mixer(8)'s vol)
    fm on [gain]|off           the FM loopback
    voice start|stop           a call's voice session and its back ends
    up rx|va|tx, down ...      hold a back end (tests)
    seq <section>, reload      run a sequence section; read the file again
    rd|wr <target> <reg> [val] one register (the value in the log)
    swr init|enum|stream|dump  SoundWire by hand

`pro1x-audioctl tone SECONDS [HZ]` and `record SECONDS [FILE]` test
`/dev/dsp` itself (48 kHz, 16 bit; their time says whether the ADSP or the
driver's clock moved the sound).

`rc.d/pro1x_audiod` (off by default): the modules, optionally the ADSP
(`pro1x_audiod_adsp="YES"`), the daemon under daemon(8) logging to
`/var/log/pro1x-audiod.log`, at real-time priority.

## The sequences

`codec.c` reads `pro1x-audio.seq`: text, one action per line, in
`[sections]` run by name. The FM radio's stage 3c uses the same file (its
`hph-up`/`spk-up`):

    w <target> <reg> <val>             register write (32 bit for windows)
    u <target> <reg> <mask> <val>      read, change the bits in mask, write
    r <target> <reg> [<val> [<mask>]]  read; with val a difference is logged
    poll <target> <reg> <mask> <val> <ms>
    delay <ms>, udelay <us>
    gpio <pin> 0|1                     a TLMM line (gpioc)
    rail <name> on|off                 an RPM regulator (qcom_dsp)
    clock <name|id> <hz>               an LPASS clock at the AFE (0: off)
    vote <name|block> on|off           an LPASS core block (AFE vote)
    afe start|stop                     the back end's AFE port, here
    swr init|enum|stream|dump|wcd-init SoundWire (swr.c), the WCD's probe
    call <section>, mark <text>

Targets: `rxmacro`, `txmacro`, `vamacro`, `swr0`, `swr1`, `lpass_tlmm`,
`lpass_tlmm_slew`, `lpass_audiocc`, `lpasscc` (the windows of
`/dev/qcom_lpass`, byte offsets), `wcd` (the WCD9370's registers over
swr0), `wcdrx` (its RX device), `aw0`, `aw1` (the AW87339s, I2C 0x59,
0x5a); `$vol_db` is the master volume as the RX macro's gain.

`seq/base.seq` has what only code and the device tree give: `power` (the
WCD9370's rails L14A, L9A, L22A and its reset, gpio 92), `pins` (the LPI
pins of the macros' default pinctrl states: SoundWire gpio0-5, DMIC
gpio8/9, written as `pinctrl-lpass-lpi.c` does), `volume`.
`trace2seq.py` puts it first and adds from the trace, per scenario: the
back end's part (`rx-up`/`va-up`: the macros' resume, the q6afe clocks and
votes, the CGCR resets, with `swr init`/`enum` after each master's reset,
`swr wcd-init` after swr0's, `call pins` after the DCODEC vote) up to the
AFE port's configuration (an 84-byte packet, then its 24-byte
`DEVICE_START`); the route's part (`hph-up`, ...: `swr stream swr1 on`
with the ports and channels of the snapshot's slave registers, `afe
start`, DAPM's writes, the amplifiers' enable lines and I2C writes in
patch 0026's order with its 40 ms); down the other way, split at the
first suspend; gaps of 1 ms or more as delays (at most 200 ms); reads as
comments; `[check-<scenario>]` compares every register written during the
power-up with the snapshot (`pro1x-audioctl seq check-hph` while the back
end is up). Until the trace exists the image carries `base.seq` alone.

## The headset jack

The Pro1X's 3.5 mm jack is the WCD9370's: its MBHC (multi-button headset
control) detects a plug mechanically (the L-detect switch, normally open),
tells a headphone from a headset by the ADC on the mic line with micbias 2
on (below 50 mV a headphone, up to 1.7 V a headset, above a high
impedance plug), and a headset's buttons by comparator thresholds. Its
interrupts are in-band: the WCD node has no interrupt line; its TX device
raises a SoundWire alert, which Linux's master (swr0, SPI 296, or its wake
interrupt SPI 79 while the bus' clock is stopped) turns into the slave's
interrupt callback, which reads `DIGITAL_INTR_STATUS_0-2` (regmap-irq).

`tools/pro1x-audiod/mbhc.c` is Linux's `wcd-mbhc-v2.c` (ADC detection)
with `wcd937x.c`'s callbacks and register fields: the setup of
`wcd_mbhc_initialise` (2 uA pull-up, the switch types, 96 ms insertion
debounce, 16 ms button debounce, the MBHC bias and RCO clock, HS_VREF for
1.6 V), the micbias voltages of the device tree (1.8 V), the switch
interrupt's plug detection, `wcd_correct_swch_plug` (cross connection,
plug type, the special headset at 2.7 V, 3 s of corrections every 180 ms:
here one step per tick), the electrical interrupts (a headphone that
turns out a headset), buttons (short press, held after 400 ms). Not
ported: impedance detection (a line-out counts as a headphone), moisture
detection (off in Linux's default as well), the US/EU switch (the Pro1X
has no `us-euro` GPIO). The button thresholds are the vendor kernel's
(`bengal.c` `def_wcd_mbhc_cal`: 75, 150, 237, 500 mV). Leste's device tree
had no `qcom,mbhc-buttons-vthreshold-microvolt` up to 7.1.0-pro1x.8: its
Linux logged "missing qcom,mbhc-buttons-vthreshold-microvolt" and took
500000 "mV" for all eight, which the registers truncate to 0 mV. Patch 0027
(pro1x.9) gives it the vendor's values (`docs/audio.md`).

**The interrupts** (`qcom_swr.ko`, `/dev/qcom_swr0`): an interrupt-only
driver on swr0's device tree node hands the master's line (SPI 296,
INTID 328) and its wake-up line (SPI 79, INTID 111) to the daemon, as
Linux's `uio_pdrv_genirq` would: the filter masks the line in the GIC,
the daemon (with the master clocked) handles and clears the master's
status and arms the line again (`QCOM_SWR_ARM`); the kernel never reads
the master, whose clock is the TX macro's. The master's line runs in
level mode (Linux: pulse, with a rising edge asked of the GIC). After
`[jack-up]` the daemon unmasks the WCD's SCP interrupts (`SCP_IntMask1`:
implementation-defined, clash, parity; Linux's `sdw_initialize_slave`,
which no trace shows) and from then on the MBHC's interrupts arrive as
alerts: `swr_irq` finds the alerting device, its implementation-defined
interrupt runs `mbhc_irq` (the latched `DIGITAL_INTR_STATUS_0`, until
none is left), and only the MBHC's own timers (the corrections, a long
press) wake the daemon besides.

`pro1x-audiod -j` (`rc.d/pro1x_audiod`: `pro1x_audiod_jack`):

| mode | | |
| --- | --- | --- |
| `poll` | `DIGITAL_INTR_STATUS_0` read every 100 ms | swr0's bus and the TX macro's clocks run all the time |
| `irq` | the alerts | the bus runs, no polling |
| `auto` (default) | `irq` where `/dev/qcom_swr0` is, else `poll`; checked by one read every 2 s: a latched interrupt without an alert twice and it polls from then on | the bus runs |
| `clkstop` (the power profile's) | the alerts, and 3 s (`-C`) after the last event the bus' clock stopped (ClockStopNow), the TX macro's clocks off (`[jack-sleep]`, else the trace's `[jack-down]`), the wake-up line armed; a plug or a button wakes it: `[jack-wake]`, else `[jack-up]` with its master init and enumeration taken as the clock's start (Linux's resume after a clock stop: the CGCR reset, no new init), then the alert | with nothing in the jack the ADSP's TX core clocks and votes are off, so the LPASS can sleep |

Anything else that needs swr0, the TX macro or the WCD (a back end's
sequence, a register by hand) starts the clock first; no clock stop while
a back end, a call or FM is up. `-W` writes the WCD's WakeUpEnable at
each stop (Linux does not for a simple clock stop slave; device run 18
of `../pm/README.md` tries both). Both lines are MPM pins (5, 24):
`pro1x_pm` enables them while they are armed, so the jack wakes the SoC
from vlow/vmin too. `pro1x-audioctl jack` counts polls, alerts, the
WCD's interrupts, misses, clock stops and wake-ups. A line that keeps
firing (more than 100 a second, or 20 in a row with nothing to do: a
status bit the clear does not clear; the daemon runs at real-time
priority) is disarmed and the jack polled (`storms`).

Without `[jack-up]` (no trace yet) the jack is not watched. `-J` turns it
off.

What follows a plug:

- **Routes**: a plug takes the playback (`hph`), a headset's microphone
  the recording (`amic`, `TX_CODEC_DMA_TX_3`); without one, what was asked
  for (`route`, `rec`; default `spk`, `mic`). A playing rx back end is
  switched at once (the old route's down, the new one's up); a recording
  takes its microphone at its next start.
- **Calls**: `voice start` takes the headset's microphone if one is in
  (the CVP session's tx port is `TX_CODEC_DMA_TX_3`), else DMIC3
  (`VA_CODEC_DMA_TX_0`), and the earpiece the modem agent asks for
  (`route ear`) only without a plug; a plug or unplug during the call
  switches the playback at once and starts the session anew with the
  other microphone (its port is fixed at the session's creation). The
  speaker toggle (`route spk`) has no effect while a plug is in.
- **Events**: on the control socket (`pro1x-audioctl watch`: `event jack
  headset`, `event button 0x4 press`), in `status` (`jack=headset
  in_use=hph/amic`) and `jack` (`ok plug=headset watched=1 mv=... polls=...`);
  to devd through snd_pro1x (`!system=PRO1X subsystem=audio type=JACK
  plug=headset`, `type=BUTTON event="button 0x1 press"`; `sysctl
  dev.pcm.0.pro1x` has the last one); to evdev through `/dev/uinput`
  ("Pro1X headset jack": `SW_HEADPHONE_INSERT`, `SW_MICROPHONE_INSERT`,
  `SW_LINEOUT_INSERT`, the buttons as Linux maps them: `KEY_MEDIA`,
  `KEY_VOICECOMMAND`, `KEY_VOLUMEUP`, `KEY_VOLUMEDOWN`), so X sees the
  buttons as keys (the volume buttons reach `pro1x-volume-keys`).
  PulseAudio's OSS module has no jack: the switching happens below it, its
  volume bar is unaffected.
- **The FM app** (`apps/pro1x-fm`, FreeBSD part) asks the daemon's `jack`
  each second while it plays: without a plug it says "Plug in headphones:
  their cable is the antenna."

**Linux on the Pro1X (Leste)**: the pieces are there. The sound card sets
the jack on the `WCD Capture` link (`qcom_snd_wcd_jack_setup` for
`TX_CODEC_DMA_TX_3`: the "Headset Jack" input device, the controls
"Headphone Jack" and "Mic Jack", the buttons as above), `wcd937x` starts
its MBHC with the defaults above, and its interrupts come over SoundWire
as described. Since 7.1.0-pro1x.9 (patch 0027) the device tree has the
vendor's button thresholds, and the UCM profile's patch
(`patches/nixos-audio/0002-ucm-headset-jack.patch`) has PulseAudio follow
the jack (`JackControl "Headphone Jack"` on Headphones, a Headset
microphone device with `JackControl "Mic Jack"`); `docs/audio.md` has both
and the test. Whether it works on the phone is not known yet. The trace's
`jack` scenario answers it: it prints whether Linux saw the removal and
the insertion (the "Headphone Jack" control), the input device, and the
MBHC's writes.

## Hildon

PulseAudio, which `hildon-session` starts, finds `/dev/dsp` as on any
FreeBSD (its OSS module; in the VM it plays through the VM's sound card
the same way): Hildon's sounds (`maemo-input-sounds`, the notification
daemon), the media player and everything else go through it, and
`pro1x-volume-osd` shows its default sink's volume. FreeBSD's Hildon has
nothing that takes the volume keys: `tools/pro1x-volume-keys` grabs
`XF86AudioRaiseVolume`, `LowerVolume` and `Mute` on the root window and
steps the default sink by 5 % (up to 100 %) with pactl; `hildon/build.sh
pro1x-volume-keys` builds it, `hildon-session` starts it. PulseAudio's
volume stays PulseAudio's (or goes to sound(4)'s PCM level, which is
software too); the daemon's master volume, the RX macro's gain, is 0 dB
from 75 % up. Not tried yet: PulseAudio on this `/dev/dsp`, the keys in a
running session.

## Tests on the PC

    make -C freebsd/audio/test

under ASan and UBSan:

- `test_audiod`: every ASM, ADM and voice packet byte for byte against
  Linux's packed structures filled as `q6asm.c`, `q6adm.c` and q6voice
  fill them (the matrix map's room for a u32 per COPP, the ADM close's
  source port being the port's index); the PCM path against a simulated
  ADSP and snd_pro1x: the order of a start (core, AFE configuration and
  start, map, open, COPP, matrix, format, blocks, run), never more blocks
  in flight than sound(4) filled, their addresses and handle, 40 blocks
  round, the stop's order, the back end's idle power-down, a restart, a
  stream with nothing ready, capture (all blocks queued for reading before
  RUN), a refused COPP undone with the kernel told; the sequence parser
  (every line kind, errors with file and line) and runner against
  simulated windows, SoundWire, I2C, GPIOs, rails, clocks and votes,
  `$vol_db`; the SoundWire master against a simulated one with the
  WCD9370 behind it (init's registers, the enumeration's id
  `0x230217010a00` from the enumerator's byte order, command packing and
  id wrap, reads, a stream's slave and master registers, the bank
  switch's broadcast); the master's interrupts (an alert with the
  callback and the acks, IntMask1 unmasked, a status change, a command
  error, the clock stop's broadcast and the start); the jack by alerts
  (a plug's alert, the clock stopped 3 s later with the TX macro's clock
  off and the wake-up armed, a plug while stopped waking it without a new
  init, a back end waking it first, none while a back end is up, the
  fall-back to polling, the poll mode as before; no access to the master
  while its clock is off); the voice session (six commands up, five down,
  handles from the answers' source ports, a refused CVP undone);
  `base.seq` and trace2seq's output parsed;
- `trace2seq.py --selftest`: a synthetic trace in the script's and
  ftrace's format: the split, the inserted SoundWire and WCD lines, the
  pins after the DCODEC vote, the stream's ports, `afe start` at the
  start packet, the PA's 20 ms delay, the amplifier, the checks.

In the FreeBSD VM (`test/vm-pcm.sh`, the driver built with
`SND_PRO1X_VM`: no SMMU, attached on nexus), 2026-10-30: the clock plays
2 s in 2.6 s and records silence; `vm_pcm_test` as the daemon with a
simulated ADSP reads the tone out of the blocks through the driver's mmap
(51 of 73 blocks with sound, the rest the start's silence and the tail)
and its ramp arrives at the reader (peak 8031); the daemon gone after 20
blocks, the clock finishes the tone; two players at once arrive mixed
(vchans); the module unloads.

Built for FreeBSD/arm64: `sound.ko`, `snd_pro1x.ko` (every symbol
resolved by the PRO1X kernel, `sound.ko` and `qcom_dsp.ko`),
`pro1x-audiod`, `pro1x-audioctl` (`build.sh`), `pro1x-volume-keys`
(against Hildon's sysroot).

## Tests on the phone

Unattended: `pro1x.test=audio pro1x.audio=STEP` in `/host/freebsd/cmdline`,
`rootfs/usr/local/libexec/pro1x-audiotest` reports to the console (the RAM
console keeps it for Leste) and reboots; `hint.qcom_wdt.0.lifetime` ends a
hang. It needs a root image with this directory (`rc.local`'s `audio`
test); a newer `pro1x-audiotest`, `pro1x-audiod`, `pro1x-audioctl` and
`pro1x-audio.seq` in `/host/freebsd/audio/`, the modules in
`/host/freebsd/modules/` are taken first. `$WORK/audio-test/` has them laid
out as `/host/freebsd/` wants them, with the command lines
(`cmdline.audio-STEP`). Each step does the ones above it:

| Run | cmdline | Expected |
| --- | --- | --- |
| audio-pcm | `pro1x.test=audio pro1x.audio=pcm hint.qcom_wdt.0.lifetime=240` | `snd_pro1x loaded`; `qcom_smmu: snd_pro1x: stream 0x1c1, context bank N, translation off` (or `match entry M, context bank N ...`); `pcm0: playback at 0x..., recording at 0x... (ADSP addresses \| 0x100000000)`; `/dev/sndstat` with `pcm0: <F(x)tec Pro1X sound (ADSP)> (play/rec) default`; `tone: 96000 frames of 440 Hz in 2.x s`, `record: 48000 frames in 1.0x s, peak 0`; `dev.pcm.0.pro1x: daemon absent ... by the clock N` |
| audio-adsp | `pro1x.test=audio pro1x.audio=adsp hint.qcom_wdt.0.lifetime=300` | the ADSP up (`hw.qcom_apr: open`); `core: ADSP ready`; `up rx`: `afe: port 0xb030 started`, `backend rx up (hph)` with `codec: no path [hph-up]` while the sequences are `base.seq`; `down rx` 3 s later: `afe: port 0xb030 stopped`; the same for `va` and `0xb021` — or which command the ADSP refused (`afe: port 0xb030 start: ADSP status 0x..`) |
| audio-play | `pro1x.test=audio pro1x.audio=play hint.qcom_wdt.0.lifetime=360` (headphones in; `pro1x.audio_route=spk` the loudspeakers) | `pcm: playback started: session 1, 48000 Hz, 2 ch, 8 blocks of 7680 bytes at 0x..., COPP N on port 0xb030, 2 blocks queued`; `tone: 144000 frames of 1000 Hz in 3.x s` (real time: the ADSP takes the blocks); `status` with `blocks_out` about 75; `pcm: playback stopped after N blocks`; with the trace's sequences the tone in the headphones |
| audio-rec | `pro1x.test=audio pro1x.audio=rec hint.qcom_wdt.0.lifetime=420` | play, then `pcm: capture started: session 2, ... port 0xb021`; `record: 144000 frames in 3.0 s, peak P` (P above the noise floor with the `mic` sequences) |
| audio-codec | `pro1x.test=audio pro1x.audio=codec hint.qcom_wdt.0.lifetime=480` (needs the trace's sequences) | rec, then `seq check-hph`: no `... is 0x.., Linux had 0x..` lines (or the registers that differ), `swr0 (sequence): version 0x1060000, status 0x1, ... slaves 0x..`, the same for swr1, `check-mic` |
| audio-voice | `pro1x.test=audio pro1x.audio=voice hint.qcom_wdt.0.lifetime=540` | codec, then `voice: up (MVM 0x.., CVP 0x..)` or the ADSP's status of the refused step (without a call the passive session may be refused); `voice stop` |
| audio-jack | `pro1x.test=audio pro1x.audio=jack hint.qcom_wdt.0.lifetime=600` (by hand, the screen says when: headphones out, in, a headset in, its button short and held, out, headphones in; needs the trace's `[jack-up]`) | play, then `qcom_swr loaded`, `jack: MBHC set up`, `swr0: device 3: SCP interrupts unmasked`, `jack: watched (N registers held, masters 0x1; irq, checked by a slow poll)` (`pro1x.audio_opts="-j poll"`: as before, polled; `-j clkstop`: `../pm/README.md` run 18); per change `jack: removed`/`jack: something inserted`, `jack: headphone`/`headset`, `jack: detection done: headset (NNN mV)`, `route hph -> spk` and back, `jack: button 0x1 pressed`/`released`; `status` with `in_use=hph/amic`; a tone after each change on the route in use; the watcher's `event jack ...` lines; `check-jack`: the MBHC's registers against Linux's |

`pro1x.audio_trace=1` logs every register access (`-T`);
`pro1x.audio_opts=` passes options to the daemon. A new trace's sequences
need only `/host/freebsd/audio/pro1x-audio.seq` replaced.

What a failing run says, in the order it can fail: no `pcm0` (sound.ko
or snd_pro1x.ko missing from the modules, or no buffer below 4 GiB); the
SMMU line with `no free match entry` (the ADSP cannot reach the buffers:
playback runs on the clock, the daemon's writes come back with errors);
`hw.qcom_apr` not open (the ADSP's start, `freebsd/dsp` run dsp3); a
refused AFE port (the codec DMA port wants its clocks or votes first:
`clock`/`vote` lines in `rx-up`); `map: ADSP status` (the address or the
pool); `COPP open` refused (the port not started); WRITE_DONE never
coming (`tone` taking the clock's time with `blocks_out` 0: the ADSP not
reading, or the SMMU); a hang right after `backend rx` (a macro register
read with its clock off: the watchdog ends it; `pro1x.audio_trace=1`
names the register).

## Risks

- **The trace's split**: `trace2seq.py` was tested on a synthetic trace
  only; the real one may order things differently (the macros' resume
  around the AFE start, SoundWire masters resuming without a CGCR reset,
  writes to devices without a target). Its notes and the `check` sections
  show where.
- **SoundWire from code**: init, enumeration and the stream's registers
  follow Linux's code, checked only against a simulated master; the
  snapshot of the real master's registers (`qualcomm-registers`) and the
  slave's (DPn) in the trace is what to compare (`swr dump`). Without
  interrupts, a slave's alert or a lost command is only seen by polling.
- **The SMMU stream 0x1c1**: after the boot menu's Linux, which never
  probed q6asm, the stream may have no match entry; `qcom_smmu_bypass`
  then takes a free one. If the hypervisor refuses, the ADSP cannot reach
  the buffers.
- **Latency**: two filled blocks ahead (80 ms) is what sound(4) gives; a
  daemon late by more than a block makes the ADSP run dry (a click). The
  daemon runs at real-time priority; `qcom_apr` queues 64 packets, about
  a second of data events.
- **One thread**: the daemon serves the ADSP's done events while it waits
  for an APR answer, but not during a sequence's delays, a SoundWire
  enumeration (up to 1 s) or its command FIFO's polling. A route change
  while music plays (the amplifiers' 40 ms), or a recording or call that
  starts then (`va-up`), can let the playback run dry for that long (a
  click): acceptable for now, a second thread for the sequences later.
- **The amplifiers' bus**: the AW87339s answer only with their enable
  line high; the daemon looks for their I2C bus at the first access (after
  the sequence's `gpio 85 1`), `-i /dev/iicN` names it.
- **Power**: the codec stays powered while a back end is up and 3 s
  after; the rails stay on. The jack keeps swr0's bus and the TX macro's
  clocks running unless `-j clkstop` (`-J`: no jack).
- **The jack**: `mbhc.c` was tested against a simulated MBHC only (its
  ADC as a divider on the mic line, its interrupt status latched until
  cleared), its alerts and the clock stop against a simulated master; the
  real WCD's alert and wake-up are the device run's (`auto` falls back to
  polling should the alert not come). Polled, an insertion's report is up
  to 100 ms later than an interrupt's, and the 3 s of corrections happen
  in steps between other work. A plug in at the daemon's start is seen when the
  mechanical detection fires (or not at all: then a re-plug).
  In the VM devd logged the `JACK` notification; the `BUTTON` one reached
  snd_pro1x's sysctl but not devd's log yet (to check: its quoted value).
- **The ADSP's power domains** (LPI_CX, LPI_MX) are what the boot menu's
  Linux left voted (`freebsd/dsp/README.md`).
- **Voice without a call**: untestable before the modem places one.

## Files

| File | |
| --- | --- |
| `sys/arm64/qualcomm/audio/snd_pro1x.c`, `pro1x_pcm.h` | the sound(4) device, its interface to the daemon |
| `sys/modules/snd_pro1x/`, `sys/modules/snd_pro1x_vm/` | the module, and its VM variant |
| `tools/pro1x-audiod/audiod.h` | the daemon's structures |
| `tools/pro1x-audiod/apr.c`, `q6.c` | APR transactions; ASM, ADM, core |
| `tools/pro1x-audiod/pcm.c` | the streams between `/dev/pro1x_pcm` and ASM |
| `tools/pro1x-audiod/backend.c` | the back ends, the AFE ports, clocks, votes, FM, the WCD's init |
| `tools/pro1x-audiod/codec.c` | the sequences: parser and runner |
| `tools/pro1x-audiod/swr.c` | the SoundWire masters, their interrupts and clock stop |
| `sys/arm64/qualcomm/audio/qcom_swr.c`, `qcom_swr.h`, `sys/modules/qcom_swr/` | `qcom_swr.ko`: the masters' interrupt lines for the daemon (`/dev/qcom_swrN`) |
| `tools/pro1x-audiod/voice.c` | the voice session |
| `tools/pro1x-audiod/mbhc.c` | the headset jack (MBHC) |
| `tools/pro1x-audiod/main.c` | the loop, the platform (windows, GPIOs, rails, I2C), the control socket |
| `tools/pro1x-audioctl/` | the client; `tone`, `record` |
| `tools/pro1x-volume-keys/` | the volume keys in Hildon |
| `tools/trace2seq.py` | the Leste trace as sequences |
| `seq/base.seq` | the sequences only code gives |
| `test/leste-audio-trace.sh` | the trace under Leste |
| `test/test_audiod.c`, `test/Makefile`, `test/shim/` | the host tests |
| `test/vm-pcm.sh`, `test/vm_pcm_test.c` | snd_pro1x in the VM |
| `../rootfs/usr/local/libexec/pro1x-audiotest`, `../rootfs/etc/rc.local` | the device runs |
| `../rootfs/usr/local/etc/rc.d/pro1x_audiod` | the daemon at boot (off) |
| `../rootfs/usr/local/share/pro1x/audio/pro1x-audio.seq` | the image's sequences (`base.seq` until the trace) |
| `../fm/tools/pro1x-fmaudio/afe.c`, `mmio.c` | the AFE's packets and the windows, shared with the FM radio |
