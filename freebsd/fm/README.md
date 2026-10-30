# The FM radio under FreeBSD

The Pro1X's Bluetooth chip, the WCN3950, has an FM receiver
(`docs/fm-radio.md`: how Leste drives it, the research behind it). Its
commands travel over the Bluetooth UART as H4 packets of their own type
(0x11 commands, 0x14 events); its audio leaves the chip only over SLIMbus,
to the ADSP. FreeBSD has none of the Linux pieces for that (no driver for
the SoC's GENI UART, no Bluetooth stack for the chip, no V4L2, no audio),
so this directory builds them the FreeBSD way, modelled on the Linux
code the Leste kernel runs:

| Stage | What | Here | State |
| --- | --- | --- | --- |
| 1 | the Bluetooth UART, the chip's bring-up | `qcom_geni_uart.ko` (uart(4) class), `pro1x-fmd` (Linux's hci_qca/btqca in user space) | built; host tested (protocol, arithmetic); not run on the phone |
| 2 | FM control: tune, seek, signal, stereo, RDS | `pro1x-fmd` (Linux's radio-qca-fm in user space), its socket, `pro1x-fmctl` | built; host tested against a simulated chip |
| 3 | FM sound: SLIMbus, the ADSP's AFE, the output | `qcom_lpass.ko` (`/dev/qcom_lpass`), `pro1x-fmaudio`; `qcom_apr.ko` (`/dev/apr`), `pro1x-q6` (in `freebsd/dsp`) | SLIMbus and AFE written, host tested, device runs prepared; the output (RX macro, SoundWire, WCD9370) not written; see "Stage 3" |
| 4 | the app on FreeBSD's Hildon, the sound switched with the radio | `apps/pro1x-fm` with a pro1x-fmd backend (part `pro1x-fm` of `hildon/build.sh`); pro1x-fmd's `audio on\|off` runs pro1x-fmaudio | built; backend and `audio` host tested; nothing to hear before 3c |

Nothing here has run on the phone (unreachable until 2026-10-31). What
can be checked on the PC is checked: `make -C freebsd/fm/test` (below).
Nothing is audible yet: the last piece of the sound path (stage 3c) is
not written.

## Stage 1: the UART and the chip

### qcom_geni_uart.ko

The Bluetooth UART is a QUP v3 serial engine at `4a8c000`
(`serial@4a8c000`, `qcom,geni-uart`, under `geniqup@4ac0000`) with the
WCN3950 as its `bluetooth` child. `sys/dev/uart/uart_dev_qcom_geni.c` is
Linux's `qcom_geni_serial.c` in its FIFO mode (the mode Linux's console
uses: no SE DMA, so no SMMU stream for the engine) as a uart(4) class:
`/dev/cuau0`. The PRO1X kernel has no uart(4), and uart(4) finds its
classes in linker sets, which are per module, so the module carries
uart(4)'s core and its device tree attachment as well
(`sys/modules/qcom_geni_uart`).

FreeBSD has no clock or pin control driver for the SM6115, and the boot
menu's Linux never opens this UART, so FreeBSD may find the engine's
clock gated and its pins in their sleep state. Reading an engine register
with its clock gated hangs the bus, so before the first access the driver:

- checks the clock `GCC_QUPV3_WRAP0_S3_CLK` (index 98 in the UART's
  `clocks`; halt register `0x1f4d4`, vote bit 13 of `0x7900c`, RCG at
  `0x1f4d8`, all from Linux's `gcc-sm6115.c`) and votes it on if it is
  gated (`hw.qcom_geni_uart.clk_enable=0`: never; a gated clock then
  refuses the attach with a message);
- puts the pins of the node's `default` pinctrl state into their function:
  gpio8-11 (CTS, RTS, TX, RX) into `qup3`, function 1 of Linux's
  `pinctrl-sm6115.c` (EAST tile), with the state's bias and drive
  strength (`hw.qcom_geni_uart.pinmux=0`: left alone). The CTL registers
  before and after are in `dev.uart.0.pins`.

The baud rate is set as Linux sets it: the serial engine's source clock
from the RCG's frequency table (Linux's `geni_se_clk_freq_match`, the
table `ftbl_gcc_qupv3_wrap0_s0_clk_src`), the RCG programmed as
`clk-rcg2.c` programs it, then the engine's divider, with 16 or 32 times
oversampling by the QUP's hardware version (read from the wrapper). For
the chip's rates: 2400 and 115200 baud from 7.3728 MHz, 3000000 from
48 MHz (QUP v2.5 or later) or 96 MHz. `hw.qcom_geni_uart.set_rate=0`
leaves the RCG as it is and only sets the divider. Hardware flow control
(RFR from the RX FIFO's level, CTS gating TX) follows CRTS_IFLOW and
CCTS_OFLOW; without it RTS is the manual RFR, as in Linux. The arithmetic
is in `uart_qcom_geni_calc.h`, which the host test checks.

`dev.uart.0.loopback=1` switches the engine's internal loopback on (TX to
RX, RTS to CTS): `pro1x-fmd -L 115200,3000000` then sends 4 KiB through it
at each rate, which tests the driver, its interrupts, the clock and flow
control without the chip.

What the boot menu could do instead: keep `4a8c000.serial` runtime active
before the kexec, as it does for the keyboard's I2C (`4a84000.i2c`,
`initramfs/local-top/pro1x-loop-root`). Not done: it needs a new boot
image, and whether Linux turns the clock on for a UART nobody opened is
unknown; the driver does not rely on it.

### pro1x-fmd: the chip's bring-up

`tools/pro1x-fmd` owns the UART and does what Leste's kernel does for the
WCN3950 (`qcabt.c`, with the Linux functions named in its comments):

1. the chip's four rails at the RPM: the `bluetooth` node's supplies
   (vddio, vddxo, vddrf, vddch0) by the names of their regulator nodes,
   read from the device tree through `/dev/openfirm` (L9, L16, L17, L23 of
   the PM6125), voted through `qcom_dsp`'s `hw.qcom_rpm.regulator`
   (`freebsd/dsp/README.md`); `-r none` leaves them;
2. into boot mode: flow control off and RTS deasserted, 0xC0 at 2400 baud,
   0xFC at 115200 baud, 100 ms (`qca_regulator_init`,
   `qca_send_power_pulse`);
3. the version at 115200 baud (`EDL_PATCH_VER_REQ_CMD`, answered in a
   vendor event: SoC 0x40070120, ROM 0x0102 on the Pro1X);
4. the baud rate command (0xFC48) and the host to 3000000 baud
   (`-s 115200` keeps the boot rate throughout);
5. the rampatch `qca/cmbtfw12.tlv` (its download mode 3: no answer for
   any segment but the last, then a command credit as hci_qca injects
   one) and the NVM `qca/cmnv12.bin` with tag 17 patched as Leste's patch
   0013 does it (in-band sleep on, the rate in byte 1: `15 11` becomes
   `95 0e`), each segment acknowledged;
6. HCI_RESET, after which the chip runs at the NVM's rate;
7. in-band sleep from then on: WAKE_IND/WAKE_ACK before sending to a
   sleeping chip (again every 100 ms), SLEEP_IND after 2 s without
   sending, every WAKE_IND of the chip acknowledged.

Up to three tries, as `qca_setup`; on the last, a chip that kept running
at 3000000 baud (its rails are the Wi-Fi's too) is tried at that rate.
The firmware files are linux-firmware's (`qca/cmbtfw12.tlv`,
`qca/cmnv12.bin`, as Leste has them), from the first of `-f` dir:dir...
that has them (default `/dev/pro1x-host/freebsd/firmware`, then
`/usr/local/share/pro1x/firmware`); the root image does not carry them
yet.

Only the FM receiver uses the chip here: there is no Bluetooth host stack
on top (FreeBSD's netgraph Bluetooth could sit on the same tty with
`ng_h4` one day; the FM packets would need a hook of their own then).

## Stage 2: the FM receiver

`qcafm.c` sends what Leste's `radio-qca-fm.c` sends, in the same order:
FM RX on (`4C01`), its configuration (87.5-108 MHz, 100 kHz, RDS, 50 µs
de-emphasis), stereo, the event mask, the chip's own RDS decoding, mute,
tune (kHz, then the tune status event), and `FC0E 01`, the SLIMbus data
port for the audio. A seek that finds nothing goes round the band and
reports `ENODATA` (Leste's patch 0017). The station's RSSI, stereo and RDS
sync come from `4C0A`; the station name and radio text from the chip's
events 0x09 and 0x0A.

pro1x-fmd serves the receiver on `/var/run/pro1x-fm.sock`, one text line
per request and per answer:

    on | off                    the receiver (and the chip) on or off
    tune 99.9 | tune 99900      "ok 99900"
    seek up | seek down         "ok <kHz>", or "error ENODATA no station found"
    mute 0|1, mono 0|1
    status                      "ok on=1 freq=99900 rssi=-85 signal=87 stereo=1 rds=1 mute=0 mono=0 pty=10 sinr=12 slimbus=1"
    station                     the same, asked of the chip first
    ps, rt                      "ok <station name>", "ok <radio text>"
    info                        the chip's versions, the files, in-band sleep and error counters
    debug 0|1|2                 every frame (2: and in-band sleep) to the log
    watch                       "event tune|ps|rt|on|off|audio ..." lines from then on
    audio on | off | status     the sound path (stage 4): "ok ...", "ok audio=on|starting|off"

`pro1x-fmctl` sends one request and prints the answer (`pro1x-fmctl tune
99.9`, `pro1x-fmctl -t 15 watch`). `rc.d/pro1x_fmd` starts the daemon
(off in `rc.conf` until the device tests pass); it loads `qcom_dsp` and
`qcom_geni_uart`.

## Stage 3: the sound

The receiver's audio leaves the chip only digitally. On Android (the
Pro1X's `mixer_paths.xml`, path `play-fm`; the vendor kernel's
`msm-pcm-routing-v2.c`, `q6afe.c`, `btfm_slim*.c`) it goes:

    WCN3950 FM ports 1, 2 --SLIMbus channels 160, 161--> LPASS SLIMbus (the ADSP is the bus's manager)
      --> AFE port SLIMBUS_8_TX (0x4011) --AFE loopback inside the ADSP--> AFE port RX_CODEC_DMA_RX_0 (0xb030)
      --> LPASS RX macro --SoundWire (swr1)--> WCD9370 HPHL/HPHR --> headphones (and the AW87339 amplifiers)

No PCM passes the application processor. FreeBSD does the same, in three
steps (the task's stages 1-3):

| Step | What | Here | State |
| --- | --- | --- | --- |
| 3a | the SLIMbus: the NGD as the ADSP's satellite, the WCN3950's logical addresses, its FM ports, the stream on channels 160/161 | `qcom_lpass.ko` (`/dev/qcom_lpass`), `pro1x-fmaudio` (`ngd.c`) | written; host tested against a simulated NGD, bus manager and chip; device run `fm-slim` prepared |
| 3b | the ADSP's side: SLIMBUS_8_TX, RX_CODEC_DMA_RX_0, the AFE loopback between them, the output path's LPASS clocks | `pro1x-fmaudio` (`afe.c`) over `/dev/apr` | written; packets checked byte for byte against Linux's structures, host tested against a simulated ADSP; device run `fm-afe` prepared |
| 3c | the output: LPASS RX macro, SoundWire master, WCD9370, AW87339 | — | not written; the plan below |

Nothing of it has run on the phone; nothing is audible before 3c.

### Why user space

`pro1x-fmaudio` is a user space programmer, not a set of kernel drivers:

- every phone test is an unattended reboot, and `pro1x-fmtest` takes a
  newer `pro1x-fmaudio` from `/host/freebsd/fm/` without a new kernel,
  module or root image;
- nothing on this path needs an interrupt or DMA on the application
  processor (below): the SLIMbus messages are a few dozen words, polled;
- the AFE is APR packets, which `/dev/apr` (`qcom_apr.ko`) already
  carries;
- the kernel part is one small module, `qcom_lpass.ko`: `/dev/qcom_lpass`
  maps the LPASS register windows of the device tree (SLIMbus NGD and its
  BAM, RX/TX/VA macros, both SoundWire masters, LPI pin control, the two
  LPASS clock controllers' resets; `sysctl hw.qcom_lpass.windows`) as
  device memory. `/dev/mem` cannot: arm64's `memmmap` leaves the
  attribute at write-back. No other page can be mapped through it.

When the path works, the pieces that need to stay (the stream as long as
the radio plays) are one daemon process, started and stopped by
pro1x-fmd (stage 4).

### 3a: the SLIMbus (`ngd.c`)

Linux's `qcom-ngd-ctrl.c` step by step, with the bits of Linux's SLIMbus
core the FM stream needs (`messaging.c`, `stream.c`) and the WCN3950's
side of Leste's patch 0019 (from the downstream `btfm_slim`):

1. the ADSP's SLIMbus QMI service (769, version 1, instance 0) over
   QRTR: select instance 0 with the ADSP as master, power request
   ACTIVE (`qcom_slim_qmi_init`, `qcom_slim_qmi_power_request`; the
   encoding is `pro1x-q6`'s). No NGD register is read before this has
   succeeded; the power stays requested until the stream ends.
2. the NGD (`slim-ngd@a5c0000`, NGD 1 at +0x1000): its version, then
   `NGD_INT_EN` = Linux's `DEF_NGD_INT_MASK`, the RX message timeout,
   `NGD_CFG` = ENABLE; the manager's MASTER_CAPABILITY, answered with
   REPORT_SATELLITE (D9 C5 01 01, retried on a NACK) —
   `qcom_slim_ngd_power_up`, `qcom_slim_ngd_master_worker`. An NGD that
   still has its logical address skips the exchange (downstream).
3. the WCN3950's two devices by enumeration address (`slim217,220`,
   device index 1 the ports, 0 the registers: bytes `00 01 20 02 17 02`
   and `00 00 20 02 17 02`, which is both mainline's packed
   `struct slim_eaddr` and the downstream `elemental-addr`): ADDR_QUERY,
   ADDR_REPLY with the logical address, retried for up to 5 s (`-w`)
   while the chip reports present; its slave revision read (REQUEST_VALUE).
4. the FM ports 1 and 2 (the chip's SoC 0x40070120 is no Cherokee 3.x,
   whose ports would be 5 and 4): multi-channel group 0x06, over- and
   underrun recovery, enable with watermark 8 (`wcn399x_fm_ports`,
   `btfm_slim_slave_enable_port`), CHANGE_VALUE to the interface device.
5. the stream (`slim_stream_prepare`, `qcom_slim_ngd_enable_stream`):
   CONNECT_SRC port→channel for both, DEF_ACT_CHAN `{device, 0x24, 0x20,
   0x83, tid, 160, 161}` (16 bit; 48 kHz on a 4000 Hz superframe:
   isochronous, rate multiplier 12 = 3·2², presence rate code 3 with
   the frequency-locked bit; the downstream core computes the same
   bytes), RECONFIG_NOW. Every one acknowledged (GENERIC_ACK, success bit
   0x20 as the downstream NGD checks).
6. the way back: CHAN_CTRL remove and RECONFIG_NOW (the downstream NGD;
   mainline's NGD has no `disable_stream`), DISCONNECT_PORT for both,
   the ports' enable off, QMI power INACTIVE.

**No BAM DMA.** Linux moves these messages through two BAM pipes
(`slimbam`, channels 3 and 4); the NGD also has message registers
(`NGD_TX_MSG` at +0x30, `NGD_RX_MSG` at +0x70), which the downstream
`slim-msm-ngd.c` uses whenever its message queues are off
(`msm_send_msg_buf`, `ngd_slim_interrupt`, `ngd_slim_setup` with neither
MSGQ_EN bit). That path needs no DMA, so no SMMU stream (unknown for the
BAM; the downstream device tree has none, and after the kexec unknown
streams are likely blocked) and no BAM driver. It is the same IP, but not
the mode this phone shipped with (bengal has no `qcom,rxreg-access`), so
only the device run proves it. If it fails (TX_MSG_SENT never comes, or
no RX), a BAM pipe pair is the fallback: the BAM window is already in
`/dev/qcom_lpass`, the descriptors would need a physically contiguous
buffer from the kernel, and the stream ID of the BAM (the Linux run of
pro1x.8 logs it if it is blocked, `docs/fm-radio.md`).

### 3b: the ADSP's side (`afe.c`)

All of it is APR to the AFE service (4), as Linux's `q6afe.c` sends it
(`q6afe_port_set_param_v2`, `q6afe_port_start`, `q6afe_port_stop`,
`q6afe_set_lpass_clock`, `q6afe_vote_lpass_core_hw`), and the downstream
`q6afe.c` for what mainline lacks (`afe_loopback`, `afe_loopback_gain`,
the devote), each answered by `APR_BASIC_RSP_RESULT` with the command's
token:

- SLIMBUS_8_TX: `AFE_PARAM_ID_SLIMBUS_CONFIG` (device 0 as the downstream
  `qcom,msm-dai-q6-slim-dev-id = <0>`, 16 bit, 2 channels on 160/161,
  48 kHz) and DEVICE_START, token 152 (its index in Linux's bindings with
  patch 0018). The configuration goes in the whole `union
  afe_port_config` (36 bytes), as mainline sends it for every port type.
- RX_CODEC_DMA_RX_0: `AFE_PARAM_ID_CODEC_DMA_CONFIG` (48 kHz, 16 bit,
  2 channels, mask 0x3), token 113.
- the loopback: `AFE_MODULE_LOOPBACK`, `AFE_PARAM_ID_LOOPBACK_CONFIG`
  `{1, dst 0xb030, LB_MODE_DEFAULT, enable}` on SLIMBUS_8_TX (the
  downstream "RX_CDC_DMA_RX_0 Port Mixer SLIM_8_TX"), its gain
  `AFE_PARAM_ID_LOOPBACK_GAIN_PER_PATH` in Q13 ("SLIMBUS_8 LOOPBACK
  Volume", default 0x2000 = unity, `-g`).
- the output path's clocks, as the macros' probes take them: the DCODEC
  block vote, TX_CORE_MCLK and its NPL (the VA macro's, 2 × 9.6 MHz) and
  RX_CORE_MCLK and its NPL at 19.2 MHz (`lpass-rx-macro.c` `MCLK_FREQ`).
  The vote's answer is its own packet with the client's handle (both
  kernels wait for it), which the devote names; a refused clock is noted
  and the rest goes on, since nothing behind them exists yet (3c).

The order is Android's (FM HAL and BE DAIs): the SLIMbus stream first
(the downstream BE prepares its codec DAI, btfm_slim, before the CPU
DAI), then clocks, gain, loopback, RX_CODEC_DMA_RX_0, SLIMBUS_8_TX; back
in reverse. `-N` leaves out the output side (clocks, RX port,
loopback): SLIMBUS_8_TX alone.

### 3c: the output (not written)

What Linux does for headphone playback on this phone (mixer path
`headphones` of the vendor's `mixer_paths.xml`: RX0/RX1 from AIF1_PB,
INT0/INT1, CLSH_DSM_OUT, the compander and RDAC switches), in order,
with where it comes from:

1. the WCD9370's rails: L14A (buck), L9A (RX/TX, PX), L22A (mic bias),
   through `hw.qcom_rpm.regulator` (`wcd937x.c` probe), and its reset
   GPIO 92 (active low; `gpioctl`).
2. the LPASS clocks (3b's clock step) and the LPI pins: gpio3-5 to
   `swr_rx_clk`/`swr_rx_data`, gpio0-2 to `swr_tx_*` (the register access
   to the WCD9370 goes over the TX SoundWire) — `pinctrl-sm6115-lpass-lpi.c`.
3. the VA macro's `fsgen`, the TX and RX macros' MCLK and SoundWire clock
   gates (`va_macro_mclk_enable`, `tx_macro_mclk_enable`,
   `rx_macro_mclk_enable`, `swclk_gate_*`), the `swr_audio_cgcr` resets of
   `lpasscc` and `lpass_audiocc`.
4. both SoundWire masters (`drivers/soundwire/qcom.c`, v1.6.0):
   `qcom_swrm_init`, enumeration of the WCD9370's TX device (swr0, 0,3)
   and RX device (swr1, 0,4), device numbers.
5. the WCD9370 (`wcd937x.c`, `wcd937x-sdw.c`, `wcd-clsh-v2.c`): its init
   writes over the TX SoundWire device, then for playback the RX clocks,
   class-H, HPHL/HPHR PA sequence; the SoundWire port setup of swr1
   (port 1 HPH L/R, 2 CLSH, 3 COMP; the device tree's port maps) and the
   bank switch (`sdw_stream` prepare/enable).
6. the RX macro's path (`lpass-rx-macro.c`): AIF1_PB to RX0/RX1,
   interpolators INT0/INT1, compander 1/2, the DSM; then 3b's
   RX_CODEC_DMA_RX_0 start feeds it.
7. the loudspeakers, if wanted: the AW87339s on I2C 0x59/0x5a, enable
   GPIO 85/98 (`aw87339.c` with patch 0026).

That is the larger part of the stack (rx-macro 4000 lines, SoundWire
master and core 6000, WCD937x 4000 in Linux), all MMIO and SoundWire
register sequences that can go into `pro1x-fmaudio` through the windows
`qcom_lpass.ko` already maps; but nothing of it can be tested before the
phone has shown that the bus and the AFE come up (3a, 3b). Two things
make it cheaper once the phone is back: Linux on the same phone plays to
the headphones (`docs/audio.md`), and its kernel has tracepoints
(`CONFIG_TRACING`): `test/leste-hph-trace.sh` (now a link to
`../audio/test/leste-audio-trace.sh`, the trace of the whole sound stack,
`../audio/README.md`), run once under Leste, records every regmap write
of the RX/TX/VA macros, the WCD9370 and the AW87339s, the DAPM power
sequence, the ADSP's clocks and the APR packets' order of a headphone,
loudspeaker, earpiece and microphone run, with a snapshot of all their
registers (the SoundWire masters' too) while the sound runs — the exact
register sequence 3c has to repeat. `../audio/tools/trace2seq.py` turns
it into the replay sequences that 3c and FreeBSD's sound share.

## Stage 4: the app and the sound

`apps/pro1x-fm` keeps its V4L2 code unchanged under `__linux__`; on
FreeBSD the same requests (on, tune, seek, mute, status, station name,
radio text) go to pro1x-fmd's socket. `hildon/build.sh pro1x-fm` builds
it (with its German texts and menu entry); the daemon itself comes with
`build.sh` (the DSP tools, `/usr/local/sbin`).

The sound is switched where Linux's app switches its PulseAudio loopback
(`sound()`: on with the receiver, off when muted or closed): on FreeBSD
that is pro1x-fmd's `audio on` / `audio off`, sent in the app's radio
thread like the other requests. pro1x-fmd runs `pro1x-fmaudio [-a
options] start` (`-A` names the program; the rc.d script passes
`/usr/local/sbin/pro1x-fmaudio`) as a child, logs its output, and answers
`audio on` once the child prints `fmaudio: ready` (`ok` with the rest of
that line) or `fmaudio: error ...` (`error EIO audio: ...`); until then
the other requests are served (the answer is deferred, the UART's
in-band sleep keeps running). `audio off`, `off` and the daemon's end
send the child SIGTERM, on which it takes its path down in reverse; a
child that has not ended after 10 s is killed. `status` has `audio=1`
while it runs; watchers get `event audio on|off`. `audio on` with the
receiver off is `ENETDOWN` (nothing on the bus then).

The app says what came of it: "the FM sound reaches the DSP; FreeBSD has
no headphone output for it yet" as long as pro1x-fmaudio says so (3c),
or that the path did not come up. The rc.d script loads `qrtr`,
`qcom_apr` and `qcom_lpass` with the daemon; the ADSP itself (with
`pd_mapper`) is not started at boot yet, so `audio on` needs `pro1x-dsp
start adsp` first.

## Tests on the PC

    make -C freebsd/fm/test

under ASan and UBSan:

- `test_qcabt`: linux-firmware's `cmbtfw12.tlv` (47307 bytes, download
  mode 3, 195 segments) and `cmnv12.bin` (2690 bytes, 12 segments; tag 17
  `15 11 22 00 2e 00` becomes `95 0e 22 00 2e 00` at 3000000 baud, `95
  00 ...` at 115200, nothing else changed), the file names from the
  Pro1X's version (ROM 0x12), the H4 framer fed in two pieces at every
  split with in-band sleep bytes, noise and an ACL frame between events,
  the wake retries and the idle sleep, the FM event parsers;
- `test_geni`: the clock level and divider for 2400, 115200, 921600 and
  3000000 baud at both oversamplings (all exact), the RCG registers and
  the rate read back from them, the FIFO packing words;
- `test_q6`: pro1x-q6's APR header against Linux's `struct apr_hdr` as
  q6core fills it, an answer parsed, the SLIMbus QMI requests against the
  TLVs of `qcom-ngd-ctrl.c` (and their maximum lengths), results;
- `test_fmaudio`: pro1x-fmaudio's AFE packets byte for byte against
  Linux's structures filled as `q6afe.c` fills them (`union
  afe_port_config` 36 bytes, the SLIMbus and codec DMA configurations,
  start/stop, the clock set on and off, the vote with `strscpy`'s 7
  characters) and the downstream loopback structures; its SLIMbus
  messages against `qcom_slim_ngd_xfer_msg`'s assembly (header word,
  `txn->rl - 1`, the payloads of `get_laddr`, `slim_xfer_msg`,
  `enable_stream`) and `qcom_slim_calc_coef`; then the whole program
  (its `main`, `qmi_up`, `map_window` and `/dev/apr` replaced) against a
  simulated NGD with the bus manager and a WCN3950 behind it and a
  simulated ADSP, written from the protocol: the exact order of every
  message and command for `probe`, `afe`, `afe -N`, a NACKed capability
  answer, a chip on the bus only at the third query, no chip, a retained
  NGD, an AFE port the ADSP refuses (everything taken back), no manager;
  that no register is touched before the QMI power request; the
  WCN3950's port registers;
- `run.sh`: pro1x-fmd on a pty against `fakechip.c`, a simulated WCN3950
  written from the protocol (not from qcabt.c) that checks the host's
  side: the power pulses, the rampatch byte for byte, the NVM as patch
  0013 patches it, no packet while the host's side sleeps. Then through
  the socket: on, tune, RDS, station, seek up (and around the band),
  seek down, mute, mono, the events, idle sleep both ways and waking
  again, off and on again; a run without any station (`ENODATA`); a run
  at 115200 baud throughout; `audio on|off|status` with a stand-in for
  pro1x-fmaudio (`fakeaudio.sh`: its answers, its output in the log, the
  events, `off` taking it down, `ENETDOWN` without the receiver, a failing
  one); the app's FreeBSD backend (`test_app.c`: the app's file with
  libhildon stubbed, `audio on|off` included); the UART loopback test
  against an echo.

Built for FreeBSD/arm64: the three modules (in the kernel's object tree,
every symbol resolved by the PRO1X kernel and `qcom_dsp.ko`), pro1x-fmd,
pro1x-fmctl, pro1x-fmaudio, pro1x-q6, the app (`hildon/build.sh
pro1x-fm`).

## Tests on the phone

Unattended, as the DSP runs: `pro1x.test=fm pro1x.fm=STEP` in
`/host/freebsd/cmdline`, `rootfs/usr/local/libexec/pro1x-fmtest` reports
to the console (the RAM console keeps it for Leste) and reboots;
`hint.qcom_wdt.0.lifetime` ends a hang. It needs a root image built after
this directory came (`rc.local`'s `fm` test); after that, a newer
`pro1x-fmtest`, `pro1x-fmd`, `pro1x-fmctl`, `pro1x-fmaudio` and `pro1x-q6`
in `/host/freebsd/fm/`, the modules in `/host/freebsd/modules/` and the
firmware in `/host/freebsd/firmware/qca/` are taken first, so later fixes
need no new image. `$WORK/fm-test/` has them laid out as
`/host/freebsd/` wants them, with the command lines:

| Run | cmdline | Expected |
| --- | --- | --- |
| fm-uart | `pro1x.test=fm pro1x.fm=uart hint.qcom_wdt.0.lifetime=180` | `uart0: clock: CBCR ... (on)` (or `voted on: ... on`), `pins (CTL before>after): 8:...>0x... 9:... 10:... 11:...` with function 1, `QUP v.., protocol 2, FIFOs n/n words`, `/dev/cuau0`; `loopback 115200 baud: 4096 of 4096 bytes sent, 4096 back, 0 differ ...: ok` and the same at 3000000 |
| fm-bt | `pro1x.test=fm pro1x.fm=bt hint.qcom_wdt.0.lifetime=240` | `rail l9 on: voted` (and l16, l17, l23), `QCA SOC Version :0x40070120`, `QCA ROM Version :0x00000102`, `UART at 3000000 baud`, both files downloaded, `QCA setup on UART is completed`, `info` with `up=1` |
| fm-radio | `pro1x.test=fm pro1x.fm=radio hint.qcom_wdt.0.lifetime=360` (headphones in: the cable is the antenna; `pro1x.fm_freq=` a local station) | `tune` answers `ok`, `station` with an RSSI above -100 dBm on a real station, RDS `event ps ...` within 15 s, seeks ending on stations or `ENODATA` |
| fm-audio | `pro1x.test=fm pro1x.fm=audio hint.qcom_wdt.0.lifetime=480` | the radio run, then `qcom_apr: apr_audio_svc open on lpass`, `core: ADSP state 1 (ready)`, the framework's services; `slim: service 769 ...` and `select instance ...: result 0`, `power active: result 0` — or the evidence that the ADSP does not offer what the sound needs |
| fm-slim | `pro1x.test=fm pro1x.fm=slim hint.qcom_wdt.0.lifetime=480` (headphones in) | the chip up, tuned; the ADSP up; `qcom_lpass loaded`; `probe`: `slim: QMI ... power active: result 0`, `controller version 0x...`, `NGD before: ...`, `the manager's capability`, `satellite reported; NGD status 0x2`, `WCN3950 generic device at LA 0x.., interface device at LA 0x..`, `slave revision ...`, `probe: 0`; `slim`: `FM ports 1,2 enabled`, `connect port 1 to channel 160: acknowledged` (with `-v`), `FM stream active: 48000 Hz ...`, then the way back, `slim: 0` |
| fm-afe | `pro1x.test=fm pro1x.fm=afe hint.qcom_wdt.0.lifetime=600` (headphones in) | fm-slim, then `afe -N`: `core: ADSP ready`, `afe: port 0x4011 started`, `afe -N: 0`; `clocks`: `block 4 voted on, handle N`, `the output path's clocks on`, `clocks: 0`; `afe`: the clocks, the loopback's gain and configuration done, `port 0xb030 started`, `port 0x4011 started`, `afe: 0` — or which command the ADSP refused (`ADSP status 0x..`); then stage 4: `audio on` answers `ok (no codec path yet: ...)`, `status` with `audio=1`, `audio off` `ok`, pro1x-fmaudio's output in pro1x-fmd's log |

`pro1x.fm_debug=1` logs every frame on the UART; `pro1x.fm_speed=115200`
keeps the chip at its boot rate (if 3000000 baud fails: the QUP's core
clock and its bus votes are Linux's, which FreeBSD cannot set);
`pro1x.fm_audio_opts=` passes options to every pro1x-fmaudio of the slim
and afe runs (`-T` every NGD register access, `-c 159,160` other
channels, `-p` other ports, `-w 20` a longer wait for the chip, `-M` the
message timeout).

What a failing fm-slim says, in the order it can fail: no service 769
(the ADSP's audio PD did not come up, or offers no SLIMbus); a hang right
after `power active` (the NGD's registers not clocked: the watchdog ends
it; the last line names the step); `capability timed out` ten times
(the manager never talks to the NGD: the register message path may need
the message queues after all, then BAM); `tx ... timed out` or `error
interrupt` (the TX path); `not on the bus` (the chip does not report
present: its FM receiver must be on, `FC0E 01`; the run's status shows
`slimbus=1`); a NACK code on a user message (the manager refuses the
stream's parameters).

The chip is powered before the bus exists in these runs (the receiver
first, then the ADSP), the other way round from Android, where the bus
runs long before Bluetooth comes on. Whether the WCN3950 reports present
on a bus that appears after it is unknown, so if the first `probe` finds
it not on the bus, the run tries once more with the order reversed: a
`probe` waiting 40 s for it holds the bus up while the receiver is
switched off and on again, and every later step of the run does the
same (the stage 4 check is then left out, its `off` would end it).

## Risks

- **The clock and pins at kexec**: handled by the driver (vote, mux), but
  the GPLL0 output and the QUP core clock it relies on are left as the
  boot menu's Linux left them; the `uart` run shows the state.
- **The rails**: the RPM votes are untested (run dsp7a); the WCN3950's L9
  is not one of the Wi-Fi's. Without them the chip does not answer the
  version request (three tries, then "the controller did not come up").
- **3000000 baud**: the engine's clock at 48 MHz; the QUP's core clock
  (an RPM bus clock Linux votes through its interconnect driver) is what
  Linux left. `pro1x.fm_speed=115200` is the fallback (the download then
  takes about 5 s).
- **In-band sleep**: as Leste's hci_qca does it, but FreeBSD never lets
  the UART's clock go: no power saving while the radio runs.
- **The antenna**: the headphone cable, as on Leste; not verified.
- **The NGD without message queues**: the register path is the
  downstream driver's, not the mode bengal shipped with; whether
  `NGD_INT_STAT` shows a received message with `NGD_INT_EN` as Linux sets
  it (the downstream register mode leaves `RX_MSG_RCVD` out of its mask;
  this sets mainline's, which has it) is for fm-slim to show. BAM is the
  fallback (3a).
- **SMMU streams**: none are used on this path: no DMA on the
  application processor. A BAM fallback would need the BAM's stream ID
  (unknown; switched to bypass as `qcom_smmu.c` does for others) and a
  contiguous buffer. The codec DMA and the SLIMbus data move inside the
  LPASS, under the ADSP.
- **LPASS clocks after the kexec**: the ADSP is started by FreeBSD
  (`pro1x-dsp start adsp`), so its AFE and SLIMbus state is fresh; the
  clocks of the macros are the AFE's to give (3b's clock step). A read
  of an LPASS block whose clock is off hangs the bus: `pro1x-fmaudio`
  touches only the NGD, only after the QMI power request, and nothing of
  the macros yet.
- **Power domains**: the ADSP's LPI_CX and LPI_MX (`rpmpd`) are whatever
  the boot menu's Linux left voted; FreeBSD votes neither
  (`freebsd/dsp/README.md`). If the ADSP or the SLIMbus does not come
  up, they are next.
- **The RX port without a codec**: `afe` starts RX_CODEC_DMA_RX_0 with
  nothing behind it (3c is missing): the ADSP may refuse or stall the
  port; it is the last step of fm-afe, after `afe -N` and `clocks`.
- **/dev/apr has one user**: pro1x-q6 and pro1x-fmaudio cannot run at the
  same time.

## Files

| File | |
| --- | --- |
| `sys/dev/uart/uart_dev_qcom_geni.c` | the GENI UART class |
| `sys/dev/uart/uart_qcom_geni_calc.h` | its clock and packing arithmetic |
| `sys/modules/qcom_geni_uart/` | the module: uart(4)'s core with the class |
| `tools/pro1x-fmd/qcabt.[ch]` | H4, in-band sleep, HCI commands, the chip's bring-up (hci_qca, btqca) |
| `tools/pro1x-fmd/qcafm.[ch]` | the FM receiver (radio-qca-fm) |
| `tools/pro1x-fmd/pro1x-fmd.c` | the daemon: the tty, the rails, the firmware files, the socket, `audio` (pro1x-fmaudio as a child), `-L` |
| `tools/pro1x-fmctl/` | the command line client |
| `test/` | the host tests |
| `../dsp/sys/arm64/qualcomm/dsp/qcom_apr.c`, `../dsp/sys/modules/qcom_apr/` | `/dev/apr` |
| `../dsp/tools/pro1x-q6/` | the ADSP's core and SLIMbus services |
| `sys/arm64/qualcomm/lpass/qcom_lpass.c`, `sys/modules/qcom_lpass/` | `/dev/qcom_lpass`: the LPASS register windows as device memory |
| `tools/pro1x-fmaudio/ngd.c` | the SLIMbus NGD, messages, logical addresses, value elements, the FM stream, the WCN3950's ports |
| `tools/pro1x-fmaudio/afe.c` | the AFE over APR: ports, loopback, clocks, votes |
| `tools/pro1x-fmaudio/pro1x-fmaudio.c`, `mmio.c`, `fmaudio.h` | the steps, the QMI client, the windows, the daemon |
| `test/test_fmaudio.c` | its host test (simulated NGD, manager, chip, ADSP) |
| `test/fakeaudio.sh` | a stand-in for pro1x-fmaudio in `run.sh` |
| `test/leste-hph-trace.sh` | for 3c: a link to `../audio/test/leste-audio-trace.sh` (Linux's register sequences of the sound paths, recorded under Leste) |
| `../rootfs/usr/local/libexec/pro1x-fmtest`, `../rootfs/etc/rc.local` | the device runs |
| `../rootfs/usr/local/etc/rc.d/pro1x_fmd` | the daemon at boot (off) |
| `../../apps/pro1x-fm/` | the app, both systems |
