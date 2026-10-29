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
| 3 | FM sound: SLIMbus, the ADSP's AFE | `qcom_apr.ko` (`/dev/apr`), `pro1x-q6` (in `freebsd/dsp`) | only the first bricks; see "Stage 3" |
| 4 | the app on FreeBSD's Hildon | `apps/pro1x-fm` with a pro1x-fmd backend, part `pro1x-fm` of `hildon/build.sh` | built; its backend host tested; no sound |

Nothing here has run on the phone (unreachable until 2026-10-31). What
can be checked on the PC is checked: `make -C freebsd/fm/test` (below).

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
    watch                       "event tune|ps|rt|on|off ..." lines from then on

`pro1x-fmctl` sends one request and prints the answer (`pro1x-fmctl tune
99.9`, `pro1x-fmctl -t 15 watch`). `rc.d/pro1x_fmd` starts the daemon
(off in `rc.conf` until the device tests pass); it loads `qcom_dsp` and
`qcom_geni_uart`.

## Stage 3: the sound

On Android the receiver's audio goes WCN3950 SLIMbus ports → LPASS
SLIMbus → the ADSP's AFE port `SLIMBUS_8_TX` → a loopback inside the ADSP
to the headphones (`docs/fm-radio.md`). Leste's kernel 7.1.0-pro1x.6 has
it as patches 0018-0021, built but not booted: nobody has heard the
Pro1X's FM receiver under Linux either yet. On FreeBSD, written now:

- **`qcom_apr.ko`** (`freebsd/dsp/sys/arm64/qualcomm/dsp/qcom_apr.c`): the
  ADSP's APR channel (`apr_audio_svc` on its GLINK edge) as `/dev/apr`,
  whole packets in and out, for one process; `sysctl hw.qcom_apr`. This
  is how every audio service of the ADSP (q6core, q6afe, q6asm, q6adm) is
  reached.
- **`pro1x-q6`** (`freebsd/dsp/tools/pro1x-q6`): `core` asks the ADSP's
  core service what Linux's `q6core` asks at boot (ready?, the framework
  and service versions); `slim` asks the ADSP's SLIMbus service (QMI 769
  over QRTR) what Linux's `qcom-ngd-ctrl` asks first: select controller
  instance 0 with the ADSP as master, power the bus up (and down again).
  This is the FreeBSD side of the Linux test plan's step 0 ("does the ADSP
  offer the SLIMbus service at all?"). `send` sends any APR command.

Still missing, in the order the sound needs them:

1. **The SLIMbus NGD controller** (Linux's `qcom-ngd-ctrl.c`, some 1700
   lines): after the QMI steps above, its message queues to the ADSP's
   bus manager run through a **BAM DMA** engine (`slimbam` at `0xa584000`,
   channels 3 and 4), for which FreeBSD has no driver; whether that DMA
   needs an SMMU stream is unknown (the downstream device tree has none;
   the Linux run of pro1x.6 will log a blocked stream ID if it does).
2. **The SLIMbus core**: the WCN3950's two devices (`ifd@0,0`,
   `codec@1,0`, `slim217,220`) reported present, their logical addresses,
   the stream messages (connect the ports, define and activate channels
   160 and 161).
3. **The WCN3950's ports**: the register writes of Leste's patch 0019
   (from downstream `btfm_slim`) for FM ports 1 and 2.
4. **The AFE port**: `SLIMBUS_8_TX` (0x4011) configured (48 kHz, 16 bit,
   two channels on 160/161) and started over APR, which `/dev/apr` can now
   carry.
5. **An output**: FreeBSD has no sound on the Pro1X at all. Hearing the
   radio needs either the ADSP's loopback to `RX_CODEC_DMA_RX_0` and the
   whole headphone path behind it (the LPASS RX macro, its clocks through
   the ADSP's AFE, the SoundWire master, the WCD937x codec on SoundWire,
   none of which FreeBSD has), or the capture into the host (q6asm with
   its shared memory mapped for the ADSP) and still that same output path
   for playback.

That is the size of a FreeBSD audio stack for the SM6115 (Linux's is tens
of thousands of lines of ASoC, q6dsp and SoundWire code), on hardware that
cannot be debugged except by unattended runs. The honest state: FreeBSD
tunes, seeks and shows RDS once stages 1 and 2 pass on the phone; FM sound
on FreeBSD is not in reach without that stack, and the first useful
evidence (does the ADSP answer APR and offer the SLIMbus service under
FreeBSD?) comes from the `audio` run below. Linux is the place to get FM
sound working first (pro1x.6); its results decide the SLIMbus details
FreeBSD would copy.

## Stage 4: the app

`apps/pro1x-fm` keeps its V4L2 code unchanged under `__linux__`; on
FreeBSD the same requests (on, tune, seek, mute, status, station name,
radio text) go to pro1x-fmd's socket, and the window says that there is
no FM sound on FreeBSD yet. `hildon/build.sh pro1x-fm` builds it (with
its German texts and menu entry); the daemon itself comes with `build.sh`
(the DSP tools, `/usr/local/sbin`).

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
- `run.sh`: pro1x-fmd on a pty against `fakechip.c`, a simulated WCN3950
  written from the protocol (not from qcabt.c) that checks the host's
  side: the power pulses, the rampatch byte for byte, the NVM as patch
  0013 patches it, no packet while the host's side sleeps. Then through
  the socket: on, tune, RDS, station, seek up (and around the band),
  seek down, mute, mono, the events, idle sleep both ways and waking
  again, off and on again; a run without any station (`ENODATA`); a run
  at 115200 baud throughout; the app's FreeBSD backend (`test_app.c`:
  the app's file with libhildon stubbed); the UART loopback test against
  an echo.

Built for FreeBSD/arm64: both modules (in the kernel's object tree, every
symbol resolved by the PRO1X kernel and `qcom_dsp.ko`), pro1x-fmd,
pro1x-fmctl, pro1x-q6, the app (`hildon/build.sh pro1x-fm`).

## Tests on the phone

Unattended, as the DSP runs: `pro1x.test=fm pro1x.fm=STEP` in
`/host/freebsd/cmdline`, `rootfs/usr/local/libexec/pro1x-fmtest` reports
to the console (the RAM console keeps it for Leste) and reboots;
`hint.qcom_wdt.0.lifetime` ends a hang. It needs a root image built after
this directory came (`rc.local`'s `fm` test); after that, a newer
`pro1x-fmtest`, `pro1x-fmd`, `pro1x-fmctl` and `pro1x-q6` in
`/host/freebsd/fm/`, the modules in `/host/freebsd/modules/` and the
firmware in `/host/freebsd/firmware/qca/` are taken first, so later fixes
need no new image. `$WORK/fm-test/` has them laid out as
`/host/freebsd/` wants them, with the command lines:

| Run | cmdline | Expected |
| --- | --- | --- |
| fm-uart | `pro1x.test=fm pro1x.fm=uart hint.qcom_wdt.0.lifetime=180` | `uart0: clock: CBCR ... (on)` (or `voted on: ... on`), `pins (CTL before>after): 8:...>0x... 9:... 10:... 11:...` with function 1, `QUP v.., protocol 2, FIFOs n/n words`, `/dev/cuau0`; `loopback 115200 baud: 4096 of 4096 bytes sent, 4096 back, 0 differ ...: ok` and the same at 3000000 |
| fm-bt | `pro1x.test=fm pro1x.fm=bt hint.qcom_wdt.0.lifetime=240` | `rail l9 on: voted` (and l16, l17, l23), `QCA SOC Version :0x40070120`, `QCA ROM Version :0x00000102`, `UART at 3000000 baud`, both files downloaded, `QCA setup on UART is completed`, `info` with `up=1` |
| fm-radio | `pro1x.test=fm pro1x.fm=radio hint.qcom_wdt.0.lifetime=360` (headphones in: the cable is the antenna; `pro1x.fm_freq=` a local station) | `tune` answers `ok`, `station` with an RSSI above -100 dBm on a real station, RDS `event ps ...` within 15 s, seeks ending on stations or `ENODATA` |
| fm-audio | `pro1x.test=fm pro1x.fm=audio hint.qcom_wdt.0.lifetime=480` | the radio run, then `qcom_apr: apr_audio_svc open on lpass`, `core: ADSP state 1 (ready)`, the framework's services; `slim: service 769 ...` and `select instance ...: result 0`, `power active: result 0` — or the evidence that the ADSP does not offer what the sound needs |

`pro1x.fm_debug=1` logs every frame on the UART; `pro1x.fm_speed=115200`
keeps the chip at its boot rate (if 3000000 baud fails: the QUP's core
clock and its bus votes are Linux's, which FreeBSD cannot set).

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
- **The sound**: see Stage 3.

## Files

| File | |
| --- | --- |
| `sys/dev/uart/uart_dev_qcom_geni.c` | the GENI UART class |
| `sys/dev/uart/uart_qcom_geni_calc.h` | its clock and packing arithmetic |
| `sys/modules/qcom_geni_uart/` | the module: uart(4)'s core with the class |
| `tools/pro1x-fmd/qcabt.[ch]` | H4, in-band sleep, HCI commands, the chip's bring-up (hci_qca, btqca) |
| `tools/pro1x-fmd/qcafm.[ch]` | the FM receiver (radio-qca-fm) |
| `tools/pro1x-fmd/pro1x-fmd.c` | the daemon: the tty, the rails, the firmware files, the socket, `-L` |
| `tools/pro1x-fmctl/` | the command line client |
| `test/` | the host tests |
| `../dsp/sys/arm64/qualcomm/dsp/qcom_apr.c`, `../dsp/sys/modules/qcom_apr/` | `/dev/apr` |
| `../dsp/tools/pro1x-q6/` | the ADSP's core and SLIMbus services |
| `../rootfs/usr/local/libexec/pro1x-fmtest`, `../rootfs/etc/rc.local` | the device runs |
| `../rootfs/usr/local/etc/rc.d/pro1x_fmd` | the daemon at boot (off) |
| `../../apps/pro1x-fm/` | the app, both systems |
