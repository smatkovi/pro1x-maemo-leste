# Calls, SMS and GPS under FreeBSD

The Pro1X's modem (the SM6115's MPSS, a Hexagon DSP running Qualcomm's
firmware) offers its telephony as QMI services over QRTR, the IPC router
`freebsd/dsp` brings up (`freebsd/dsp/README.md`, run dsp6: rmtfs,
tqftpserv, pd-mapper, `pro1x-dsp start modem`). Leste talks to it with
oFono's QMI driver; FreeBSD has no oFono (it is built on Linux's ELL,
udev and netlink), so this directory has a daemon of its own that speaks
QMI to the modem and oFono's D-Bus API to the applications:

| Stage | What | Here | State |
| --- | --- | --- | --- |
| 1 | the modem's calls, SMS, SIM and network as oFono's D-Bus API | `pro1x-modemd` (libqmi over QRTR) | built for FreeBSD/arm64; host tested against a simulated modem; device runs prepared |
| 2 | the voice of a call: the ADSP's voice session and the codec's earpiece and microphone | `pro1x-callaudio`, through pro1x-audiod (freebsd/audio, the one user of `/dev/apr`) | built; host tested against a stand-in for pro1x-audiod, sphone's Speaker button and DTMF keypad in the VM; needs the audio stack on the phone |
| 3 | GPS: the modem's GNSS (QMI LOC) into gpsd, positions for Hildon's apps | `pro1x-gnss`, gpsd, `rc.d/pro1x_gps`, Leste's `location-daemon`, `location-ui` and `location-control` (new parts), liblocation patched | the chain runs in the VM on a simulated modem's LOC, up to liblocation's client, with location-ui's dialogs and location-control's applet; device run prepared |
| 4 | mobile data: DPM, WDA, WDS sessions over the IPA, oFono's ConnectionManager and ConnectionContext | `pro1x-modemd/data.c`, `pro1x-data-ifup`, `pro1x-data`; the kernel side is `freebsd/data` (qcom_ipa) | built for FreeBSD/arm64; host tested against a simulated modem (`freebsd/data/test/data_run.sh`); device run prepared (`freebsd/data/README.md`) |
| 5 | SMS in Conversations, as on Leste: telepathy-ring (oFono's Telepathy connection manager) with its Mission Control account, the history in rtcom-eventlogger | `telepathy-ring` (new part), FreeBSD's telepathy-glib and mission-control, `hildon-session`, sphone for calls only | runs in the VM against pro1x-modemd and a simulated modem: SMS in (notification, overview, chat, Events page) and out (below) |

Nothing here has run on the phone (unreachable until 2026-10-31). No call
was made and no SMS sent: the tests that dial or send need the user and a
number they give (below).

## The choice: oFono's API, not backends in the apps

The apps are Leste's: sphone (the dialler, with its own messages UI) talks
to oFono through its module `comm-ofono`; Conversations reads the event
log (rtcom-eventlogger, where sphone's `store-rtcom` logs every call and
message) and sends through Telepathy's `ring`, which is oFono's client
too. So `pro1x-modemd` offers a subset of oFono's D-Bus API (ofono 2.18,
`doc/*-api.txt`: names, signatures and values are oFono's), and sphone
runs unchanged: what `comm-ofono` uses is all there:

| oFono | pro1x-modemd |
| --- | --- |
| `org.ofono` on the system bus, `Manager.GetModems`, `ModemAdded` | one modem, `/qrtr_0` |
| `Modem`: `Powered`, `Online` (read and set), `Manufacturer`, `Model`, `Revision`, `Serial` (IMEI), `Interfaces`, `Features`, `Type` | DMS |
| `SimManager`: `Present`, `PinRequired`, `LockedPins`, `Retries`, `SubscriberIdentity`, `CardIdentifier`; `EnterPin`, `ResetPin` | UIM, DMS |
| `NetworkRegistration`: `Status`, `Name`, `MobileCountryCode`, `MobileNetworkCode`, `Technology`, `LocationAreaCode`, `CellId`, `Strength` | NAS |
| `VoiceCallManager`: `Dial`, `HangupAll`, `SendTones`, `GetCalls`, `CallAdded`, `CallRemoved`, `EmergencyNumbers` | VOICE |
| `VoiceCall`: `Answer`, `Hangup`, `State`, `LineIdentification`, `StartTime`, `Emergency`, `PropertyChanged` | VOICE |
| `MessageManager`: `SendMessage`, `GetMessages`, `IncomingMessage`, `ImmediateMessage` (class 0), `MessageAdded`, `MessageRemoved` | WMS |
| `Message`: `State` (`pending`, `sent`, `failed`); its path unique, `message_` and 40 hex digits as oFono's UUIDs | WMS |

The rest of oFono's methods answer `org.ofono.Error.NotImplemented`
(hold, swap, multiparty, call forwarding and barring, USSD, cell
broadcast). Mobile data (ConnectionManager, ConnectionContext) is stage 4,
`freebsd/data/README.md`. sphone's `comm-ofono` had no DTMF (only its test
backend did): `freebsd/hildon/patches/sphone/0001` gives it the DTMF
button and sends each key of its keypad as `SendTones` (sphone 3). Its
Speaker button reaches the call's voice through `0002`, sphone's new
module `route-pro1x` (stage 2).

**Conversations** (stage 5, below) gets the SMS through telepathy-ring,
which is an oFono client as well: the same subset of oFono's API serves it,
with no backend of its own. sphone keeps the calls and leaves the messages
to Conversations, as on Leste.

**The event log on FreeBSD** (`freebsd/hildon/patches/rtcom-eventlogger/
0001`): rtcom-eventlogger built its INSERT with SQLite's printf and C99's
`PRIiMAX`, which is `"li"` with glibc but `"ji"` on FreeBSD; SQLite's
printf has no `j`, so every new event failed ("can't compile SQL statement
... incomplete input") and no call or message was ever logged. The two
statements take `long long` now; found with sphone in the VM.

**sphone's call window** asks for portrait, for which hildon-desktop
rotates the CRTC; the VM's X server and the phone's scfb cannot, and the
picture went black (Clutter's scale 0). The package sets sphone's
`LandscapeCalls=1` (`extra/usr/local/share/sphone/sphone.ini.d`).

## Stage 1: pro1x-modemd

`pro1x-modemd/`: C on GLib, libqmi-glib 1.34 (Leste's version, built for
FreeBSD with its QRTR transport in `freebsd/hildon`, libqrtr-glib on
`qrtr_sock.c`'s family 45). Every QMI message is libqmi's generated code
(its `data/qmi-service-*.json`); what is asked of the modem, and in which
order, follows KolibriOS for ARM's `kernel/modem/phone.c`
(github.com/smatkovi/kolibrios-arm, `docs/pro1x-modem.md`: the same
services over raw QMI, written for this phone):

- **DMS**: manufacturer, model, revision, IMEI (`Get IDs`); the operating
  mode, set to online at the start (`-n` leaves it), `Modem.Online`
  switches between online and low power; the SIM's IMSI and ICCID once it
  is ready (`UIM Get IMSI`, `UIM Get ICCID`).
- **UIM**: `Register Events` (card status), `Get Card Status` and its
  indication: the primary GW application's PIN1 (or UPIN) state and tries
  give `PinRequired` (`pin`, `puk`, `none`) and `Retries`; `Verify PIN`
  and `Unblock PIN` (session: primary GW provisioning).
- **NAS**: `Register Indications` (serving system, network time), `Get
  Serving System` and its indication: registration (`registered`,
  `roaming` by the roaming indicator, `searching`, `denied`), the
  operator (current PLMN: MCC, MNC with its PCS digit, name), the
  technology (LTE, UMTS, GSM from the radio interfaces), the LAC (the TAC
  on LTE) and cell id; `Get Signal Strength` every 30 s (`-s`) as
  `Strength`, 27.007's +CSQ range (-113 to -51 dBm) as 0-100 %; the
  network's time is logged against the system's.
- **WMS**: `Set Routes` as KolibriOS: class 2 on the SIM, classes 0, 1,
  3 and none in the modem (NV), store and notify; `Set Event Report` (new
  MT message); at the start `List Messages` (unread, NV and SIM), then
  each message (`Raw Read`) on its indication; a delivered message is
  tagged read (`Modify Tag`), not deleted, so nothing is lost when no
  client listened. Concatenated messages are put together (by sender,
  reference and count; after 10 minutes what came is delivered).
  `IncomingMessage` carries `Sender`, `LocalSentTime` (the sender's time
  and zone, `%Y-%m-%dT%H:%M:%S%z` as sphone parses it) and `SentTime`.
  Sending: SMS-SUBMIT PDUs (GSM 7-bit with its extension table, UCS-2
  otherwise, up to six concatenated parts) with `Raw Send` (GSM/WCDMA
  point to point); the message object goes `pending` to `sent` or
  `failed` (with the RP and TP causes in the log) and away, as oFono's.
  The PDU codec is KolibriOS's `sms.c` (`sms.c` here, with the time
  stamp's zone and the class kept).
- **VOICE**: `Indication Register` (call notifications, supplementary
  services), `Get All Call Info` at the start and `All Call Status`
  indications: a call per modem call id, its state (origination: dialing;
  alerting: alerting out, incoming in; conversation: active; hold:
  held; waiting; end: disconnected, then `CallRemoved` as oFono does),
  the number (when presentation is allowed), emergency calls. `Dial Call`
  (the number in oFono's format `[+][0-9*#]{1,80}`; libqmi's Dial Call
  has no CLIR TLV, so `hide_callerid` stays the network's), `Answer
  Call`, `End Call`; DTMF as ModemManager's QMI calls send it: `Start
  Continuous DTMF`, 250 ms, `Stop Continuous DTMF`, 100 ms, per tone
  (the durations are ours); tones asked for while others are still
  being sent (sphone sends one `SendTones` per key) join the queue, so
  the modem gets one tone at a time, and each call is answered once its
  tones are out.

A service the modem does not offer yet is asked again every 5 s for two
minutes (the modem announces its services one by one); a modem that goes
away (QRTR's BYE: restarted, crashed) ends the daemon, and daemon(8)
starts it again (`rc.d/pro1x_modemd`).

### The SIM PIN

FreeBSD starts the modem itself (dsp6): unlike KolibriOS, which takes
over a modem Leste had unlocked, the SIM is locked at every start. The
PIN comes from a file the user writes on userdata, from Leste:

    install -m 600 /dev/null /host/freebsd/sim-pin      # as root on Leste
    echo 1234 > /host/freebsd/sim-pin                   # the user's PIN

(`/dev/pro1x-host/freebsd/sim-pin` under FreeBSD, `-p`; never part of an
image, never written by any script here.) pro1x-modemd tries it **once
per boot** (a mark in `/var/run`, which starts empty, `-P`; daemon(8)'s
restarts do not try again) and **only while the card has at least 3
tries left** (`-r`), so that a wrong PIN in the file cannot block the SIM
over unattended reboots. The PIN is never logged (the tests check that
it is in no log). Without the file, or after a failed attempt,
`PinRequired` is `pin` and `SimManager.EnterPin` takes it; a blocked PIN
needs `ResetPin` with the PUK.

### Built

`freebsd/hildon/build.sh pro1x-modem` (`modem_part`): pro1x-modemd (and
pro1x-callaudio, stage 2) for FreeBSD/arm64 against the sysroot's libqmi
1.34, package `pro1x-modem` with `rc.d/pro1x_modemd` and the D-Bus policy
(`extra/`: root owns `org.ofono`, everyone may call it). In `rc.conf`:

    pro1x_modemd_enable="YES"                         # after pro1x_modem
    pro1x_modemd_flags="-A /usr/local/sbin/pro1x-callaudio"

(off until the device runs pass, as the modem itself).

### Tests on the PC

    make -C freebsd/modem/test

builds pro1x-modemd for the PC (the host's libqmi) under ASan and UBSan
and runs:

- `test_sms`: KolibriOS's SMS test (the classic example PDU, UCS-2,
  an alphanumeric sender, round trips through SUBMIT with the extension
  table, Euro sign and surrogates, 400 characters in three parts with an
  escape across the border, 80 UCS-2 characters in two), and the zone
  (+01, -05), class 0 and 1, a status report refused.
- `run.sh`: pro1x-modemd against `fakemodem.py`, a simulated modem that
  speaks QMUX on a pty (libqmi's QMUX endpoint takes a pty like a cdc-wdm
  port, so every message goes through libqmi's own encoders and
  decoders). The modem side is written from libqmi's data files (message
  and TLV ids, layouts, the string prefix rules), its SMS PDUs from 23.040
  and 23.038, independently of `sms.c` (its alphanumeric address for
  "Info" is byte for byte KolibriOS's test vector). The checks
  (`test_ofono.py`, dbus-python on a private bus given as
  `DBUS_SYSTEM_BUS_ADDRESS`, which sphone's GDBus honours too) go through
  what a phone goes through:
  - `main`: the modem's identity, online from low power, the PIN from
    the PIN file (exactly one `Verify PIN`, PIN1, 4 digits), IMSI, ICCID,
    retries; registered on LTE with operator, MCC/MNC, TAC, cell, signal
    61 % from -75 dBm; the four interfaces; WMS routes as above; an SMS
    in with umlauts, braces and the Euro sign (text, sender,
    `LocalSentTime` parsed as sphone does, the message tagged read), three
    parts as one message, class 0 as `ImmediateMessage`; SMS out (the PDU
    the modem got decodes to the number and text; 400 characters in three
    parts; a bad number refused); a call out (`Dial`, `CallAdded`
    dialing, alerting, the call audio program started, answered by the far
    end: active with `StartTime`, `GetCalls`, `SendTones` as start/stop
    pairs for each tone, `Hangup`: disconnected, `CallRemoved`, the call
    audio program stopped); a call in (incoming with its number,
    `Answer`, the far end hangs up); `HangupAll`; offline and online
    again;
  - `wrongpin`: a wrong PIN in the file: one attempt, 2 tries left, the
    PIN still required; `again`: restarted in the same boot, no
    attempt; `lowretries`: 2 tries left, no attempt; `nofile`: the PIN
    over D-Bus, a malformed one refused, a wrong one `IncorrectPassword`,
    the right one: the SIM ready and `MessageManager` there;
  - in every scenario: no sanitizer report, the daemon ends cleanly on
    SIGTERM, the PIN in no log.

**sphone in the FreeBSD VM** (`test/vm-sphone.sh`, freebsd/vm): the
FreeBSD build of pro1x-modemd (libqmi 1.34 for FreeBSD, QMUX on a pty to
`fakemodem.py` under the VM's Python) on the VM's system bus with the
package's D-Bus policy, Leste's sphone unchanged in Hildon on Xvfb
(2160x1080), driven with xdotool: sphone finds the modem ("comm-ofono:
Using modem: /qrtr_0"); a call comes in: its window with the number and
Answer/Hangup, Answer clicked (`VOICE Answer-Call`, active), Hangup
clicked (`End-Call`, disconnected); an SMS comes in with umlauts, braces
and the Euro sign: sphone's notification shows sender and text; the
dialler opened with a number (`sphone -c dialer-open -n ...`), Call
clicked: `Dial-Call`, dialing, alerting, active once the far end
answers, Hangup; a new SMS (`sphone -c sms-new -n ...`) typed and sent:
`Raw-Send` with the PDU decoding to the number and text, the message
`sent`. With the event log's patch all of it is in rtcom-eventlogger
(local uid `sphone/ofono`: the SMS in and out, the missed call, the call
out). Since pro1x-modem 1.1 sphone is set for calls only, as on Leste
(the SMS are Conversations', stage 5); this run's SMS steps were with its
messages modules. Doing the same in the VM again:

    tar -C <dir with pro1x-modemd, lib/, fakemodem.py, vm-sphone.sh,
        pro1x-modemd.conf> -czf - . | vm 'mkdir -p /root/modemtest &&
        tar -xzf - -C /root/modemtest'
    vm sh /root/modemtest/vm-sphone.sh start      # then ctl, click, shot, stop

With sphone 3 (its patches 0001, 0002) and pro1x-callaudio per call
against a stand-in for pro1x-audiod (`vm-sphone.sh` takes the newer
modules, the package's module list and the stand-in when they come
along): the call window has the DTMF button
(`~/ps/pro1x-screens/bsd-sphone-active.png`), its keypad
(`bsd-sphone-dtmf.png`); 1, 2 and # tapped 0.2 s apart: three
`SendTones`, at the modem `Start`/`Stop Continuous DTMF` for 1, 2, #
one after the other. Speaker: `route spk` at pro1x-audiod, the button
turns into Handset (`bsd-sphone-speaker.png`); Handset: `route ear`;
Hangup: `voice stop` and the route of before, the pid file gone.
Answering no longer passes the ring tone's loudspeaker route on: the
call starts on the earpiece.

What the simulated modem cannot tell is how the Pro1X's modem answers:
which services it announces when, whether its firmware supports DMS's
UIM messages, the event report's TLVs on this firmware. The device runs
are for that.

## Device runs (prepared)

FreeBSD on the phone has no USB and no network: each run is a reboot into
FreeBSD with `pro1x.test=modem` in `/host/freebsd/cmdline`; rc.local runs
`/usr/local/libexec/pro1x-modemtest`, which writes to the console (the RAM
console, read by Leste after the next boot, `fbsdtest.sh`) and reboots;
the watchdog resets a hang. Each run builds on dsp6 (the modem started),
which has never run: everything below sits behind it.

The user's files on userdata (from Leste, as root; none of them written by
anything here):

| File | For | Contents |
| --- | --- | --- |
| `/host/freebsd/sim-pin` | the SIM's PIN, mode 600 | the PIN's digits |
| `/host/freebsd/modem-test.conf` | what the runs may do | `to=+43...` (one SMS sent to it), `dial=+43...` (dialled, ended after 30 s), `answer=1` (a call that comes in is answered, ended after 20 s) |

Without `modem-test.conf` the runs only receive.

| Run | cmdline | Expected on the RAM console |
| --- | --- | --- |
| modem | `pro1x.test=modem` | dsp6's lines (`start: 0`, modem running, `net.qrtr.servers` with 2 DMS, 3 NAS, 5 WMS, 9 VOICE, 11 UIM, 16 LOC), `qmicli --dms-get-revision` answering; `qrtr-lookup` the same services through libqrtr's lookup (the first time libqrtr-glib's lookup runs on `qrtr_sock.c`); `pro1x-modemd: qrtr: node 0 with N services`, `qmi: DMS client 1` ... `VOICE client 5`, `dms: operating mode online`, `sim: present`, `sim: PIN required: pin (tries 3, ...)`, `sim: PIN from the PIN file`, `sim: PIN (PIN file) accepted`, `sim: ready`, `nas: registered on lte`, `nas: operator ...`; `after N s: registration "registered"`; the properties of Modem, SimManager (`PinRequired "none"`), NetworkRegistration (`Strength`) |
| sms | `pro1x.test=modem pro1x.modem=sms pro1x.modem_wait=300` | modem's lines, then for each message the user sends to the phone in the window an `IncomingMessage` with its text, `Sender` and `LocalSentTime`, and `sms: from ...` in the log; with `to=`: `sms: to ..., N part(s)`, `part 1 of 1 ... sent (reference N)` (or the RP/TP causes) |
| call | `pro1x.test=modem pro1x.modem=call pro1x.modem_wait=300` | modem's lines, then for a call to the phone `CallAdded` with `"incoming"` and the caller's number, `call 1 (/qrtr_0/voicecall01): from ..., incoming`; with `answer=1`: `voice: answer call 1`, `incoming -> active`, `callaudio: ... started` (stage 2), 20 s later `voice: end call 1`, `disconnected`, `callaudio: the last call is over`; with `dial=`: `voice: dial ...`, `dialing`, `alerting`, `active` once answered |
| gps | `pro1x.test=modem pro1x.modem=gps pro1x.modem_wait=600` | stage 3 (below) |

Risks, in the order they would show:

- **dsp6** (the modem's start) has never run; its own risks are in
  `freebsd/dsp/README.md`.
- **libqrtr-glib on FreeBSD's QRTR**: its lookup (NEW_LOOKUP to the
  control port, NEW_SERVER answers) has only been tested against Linux.
  `qrtr-lookup` (linux-msm's libqrtr) and `qmicli -d qrtr://0` in the
  same run tell whether the name service or libqrtr-glib is at fault.
- **The SIM**: no SIM: `sim: absent`, no registration (the run still
  shows DMS, UIM, NAS). A SIM whose PIN is off: `PIN required: none` at
  once. The PIN file's PIN wrong: one try used (two left), reported, not
  repeated; the user then fixes the file.
- **Firmware differences**: newer modem firmware drops DMS's UIM messages
  (no IMSI/ICCID: logged, nothing else depends on them) or answers `List
  Messages` on an empty SIM storage with an error (logged).
- **Registration on LTE only**: the modem may register on LTE without
  voice (no IMS here): calls then fall back to CSFB, if the network has
  it; the run shows the call's states or `voice: dial: ...` with the
  modem's error.
- **SMS over IMS**: some networks deliver SMS on LTE only over IMS, which
  is not set up (no IMS stack); then no SMS arrives on LTE.

## Stage 2: call audio

A call's voice does not pass the application processor: the modem hands
it to the ADSP, which runs it through its voice processing (MVM, CVP) to
the codec DMA ports, and from there through the codec to the earpiece, and
from the microphone back (KolibriOS's `docs/pro1x-audio.md`). What the
application processor does is set this up: the codec's paths, the ADSP's
ports, and for each call a voice session.

**KolibriOS's way does not carry over.** `pro1x-kexec.sh -a` keeps what
a running Leste left: the ADSP with its AFE ports started and the codec
powered by two streams held open across the kexec, Linux's APR channel
adopted. FreeBSD is kexec'd from the boot menu's initramfs, where no sound
card exists (no ADSP driver, no UCM, no PulseAudio: nothing to leave
running); and it is not kexec'd from a running Leste instead, since the
menu rebinds UFS with an identity mapping before the jump
(`initramfs/local-top/pro1x-loop-root`), which a Leste running from UFS
could not do under itself. And FreeBSD starts the ADSP itself (PAS, `pro1x-dsp start
adsp`, dsp3), which begins with no ports, clocks or codec set up. So on
FreeBSD the whole path is set up from scratch: the AFE's votes, clocks and
codec DMA ports, the codec (RX macro, SoundWire, WCD9370's EAR; VA macro
and DMIC3), then the voice session.

**Who does it.** `/dev/apr` (`qcom_apr.ko`) takes one process, and on
FreeBSD that is `pro1x-audiod` (freebsd/audio, the audio stack's daemon:
ASM, ADM, the AFE, the codec's replay sequences from the Leste trace,
SoundWire). Its `voice start` is the session as Linux's q6voice and
KolibriOS run it (MVM `CREATE_PASSIVE_CONTROL_SESSION` "11C05000", dual
control, CVP `CREATE_FULL_CONTROL_SESSION_V2` with tx `VA_CODEC_DMA_TX_0`
0xB021 and rx `RX_CODEC_DMA_RX_0` 0xB030, enable, attach, `START_VOICE`;
the way back in reverse), then the two back ends with the codec's paths of
the current route; `voice stop` takes it down. So `pro1x-callaudio` is a
client of pro1x-audiod's control socket (`/var/run/pro1x-audio.sock`, one
request per connection, as `pro1x-audioctl`):

| When | pro1x-callaudio asks |
| --- | --- |
| a call exists (pro1x-modemd starts it: dialling, ringing either way, active, held, as Linux's `pro1x-voice-call`) | `status` (the route of before), `route ear`, `voice start`; then "callaudio: ready" |
| SIGUSR1, SIGUSR2 | `route spk` (the loudspeaker), `route ear` (the earpiece again); sphone's Speaker and Handset buttons send them through its module `route-pro1x` (`hildon/patches/sphone/0002`), which finds pro1x-callaudio in `/var/run/pro1x-callaudio.pid` (`-p`, written once the voice is up); the call's voice does not pass PulseAudio here, so route-pulseaudio has nothing to switch |
| the last call is over (SIGTERM from pro1x-modemd) | `voice stop`, the route of before |

Without pro1x-audiod (none within 5 s) or when `voice start` fails it
says so ("the call has no sound") and exits 1; pro1x-modemd then does not
start it again for these calls. The codec's earpiece and microphone paths
and the `route ear` they hang on are the audio stack's (freebsd/audio,
from its Leste trace of 2026-10-31: `ear`, `mic`).

Tests on the PC (`make -C freebsd/modem/test`): `test_callaudio.sh` runs
pro1x-callaudio against `fakeaudiod.py` (pro1x-audiod's answers as its
`main.c` writes them): the requests of a call in order (status, route
ear, voice start, route spk on SIGUSR1, nothing on a second one, route
ear on SIGUSR2, voice stop, the route of before; the pid file while the
voice is up), a refused `voice start` (exit 1, the route restored), no
daemon (exit 1). Each request goes out in one write (pro1x-audiod reads
once and closes: a second write got SIGPIPE in the VM). In `run.sh` pro1x-modemd runs the real pro1x-callaudio per call
against the same stand-in: `route ear`, `voice start` while the call
exists, `voice stop` after it.

What the device run adds (`pro1x.modem=call`): the ADSP started
(`pro1x-dsp start adsp`, unless running) and pro1x-audiod
(`rc.d/pro1x_audiod`, when freebsd/audio has one), `pro1x-audioctl
status`, then the call with pro1x-callaudio (`callaudio: ready (ok
voice=up mvm=... cvp=...)` in pro1x-modemd's log), pro1x-audiod's status
during the call, `callaudio: down` after it. The risks: the ADSP and the
modem started by FreeBSD both (dsp3 and dsp6 have not run); the passive
MVM session needs the modem's own voice session to exist (Linux's
pro1x-voice-call found it to work only when started while a call exists,
which is when pro1x-modemd starts it); the codec's paths from the trace.
A call without sound still connects (stage 1).

## Stage 3: GPS

As on Leste (`README.md`: "engine runs, positions arrive"), the modem's
GNSS engine is QMI LOC over QRTR, and `tools/pro1x-gnss` turns its
position reports into NMEA for gpsd (the modem sends none itself: GGA and
RMC are built from each fix). For Hildon's location apps (CloudGPS, GPS
Recorder, Maep, Fahrplan, ...) the chain on Leste is liblocation, which
listens to `location-daemon` on the system bus, which reads gpsd. On
FreeBSD:

| Piece | Leste | FreeBSD |
| --- | --- | --- |
| the engine to NMEA | `pro1x-gnss` (libqmi over QRTR) | the same, built for FreeBSD (`hildon/build.sh pro1x-gnss`); `-d` takes a QMUX file for the tests |
| gpsd | Debian's, `/etc/conf.d/gpsd`: `udp://127.0.0.1:29470`, pro1x-gnss started and stopped with it | FreeBSD's package (3.25); `rc.d/pro1x_gps` (package pro1x-modem): gpsd on `udp://127.0.0.1:29470` and pro1x-gnss under daemon(8), stopped together (SIGTERM: LOC Stop) |
| on demand | location-daemon runs `sudo /etc/init.d/gpsd start` (and never stops it) | location-daemon runs `/usr/local/libexec/location-daemon-gpsd start` (→ `service pro1x_gps onestart`), as root through D-Bus activation, and `... stop` (→ `onestop`, not with `pro1x_gps_enable`) when its clients are gone (patch 0002) |
| location-daemon | Leste's | Leste's at its 0.8 commit, new part `location-daemon` (patch 0001: the lock in `/var/run`, the start hook, the service file below /usr/local as root; 0002: the gpsd it started stopped again; D-Bus policy in `hildon/extra/location-daemon`) |
| the dialogs | location-ui, activated on the system bus through maemo-invoker as the user | location-ui (new part), run by `hildon-session` for the session's life (`--stay`, patch 0002: a system bus activation would start it without the session's display, so no service file); Leste's fixes (`patches/fixes/location-ui.patch`, as 0001): its unanswered dialogs, and the answer to "close", which passed the response by value and aborted location-ui (libdbus' NULL check) whenever a dialog was answered with OK (found here, fixed for both) |
| the settings | location-control, the control panel's Location applet | the same (new part): GPS and network positioning on or off; its GConf schema (the defaults: GPS on, network off, the device `(com.nokia.Location, las)`, the disclaimer not accepted) loaded at boot by `rc.d/hildon_setup` |
| liblocation | Leste's | 0003: the lock in `/var/run`; 0004: when no location-ui runs at all (no session, an unattended run) the dialog's yes is taken, with a message; without location-control's schema the internal receiver; without MCE the device mode normal; 0005: the calls of location-ui's "display" and "close" lacked dbus-glib's terminator of the output arguments, so every dialog request failed on FreeBSD ("Too few arguments in reply"); 0006: `get_selected_method()` had its test the wrong way round, so the "enable positioning" dialog's yes ended in "User didn't enable requested methods" (0005 and 0006 are in Leste's code too, which has them in `patches/fixes/liblocation.patch` since 2026-10-30) |

The positions only start when an app asks (liblocation's start): the
engine runs only while a client holds liblocation's lock. location-daemon
ends 15 s after the last client let go of it; Leste's never stopped the
gpsd it had started, so gpsd, pro1x-gnss and the modem's GNSS engine ran
until the next boot after the first app that asked (found in the energy
audit of 2026-10-31; Leste's `/etc/conf.d/gpsd` says "starts and stops",
but location-daemon has no stop). Patch 0002 stops it again through the
hook's `stop` (`service pro1x_gps onestop`: SIGTERM, pro1x-gnss sends LOC
Stop); a gpsd that ran before location-daemon came is left alone.
`pro1x_gps_enable="YES"` starts it at boot instead and keeps it running
(the hook's `stop` does nothing then). pro1x-gnss's own idle stop (`-g`,
default 30 s: the LOC session ends when gpsd has closed its UDP source,
for gpsd without `-n` on Leste, Kali and Arch) does not come into play:
`rc.d/pro1x_gps` runs gpsd with `-n`, which keeps the source open for as
long as it runs, and passes no `-x` (daemon(8)'s `-r` would start an
exited pro1x-gnss again); the chain ends with gpsd, through the hook. Only
a gpsd that dies alone leaves pro1x-gnss behind, its session stopped
after 30 s and a probe every 5 s, until `service pro1x_gps onestop`. While pro1x-gnss runs,
`pro1x-autosleep` does not suspend (the positions would wake it every
second; `freebsd/pm/README.md`, stage 6).

**Tested in the VM** (`test/vm-gps.sh`): liblocation's own example
(`examples/basic.c`: start, the first fix, stop) under a session bus for
GConf, against the VM's system bus: D-Bus activates location-daemon (its
policy and service file), which finds no gpsd and runs the start hook;
gpsd (FreeBSD's) and pro1x-gnss come up, pro1x-gnss on `fakemodem.py`'s
QMI LOC (Register Events, Set NMEA Types with its indication, Start;
then a Position Report a second: 48.2082 N, 16.3738 E, 171 m, 1.5 m/s,
DOP, five satellites, the UTC time, written from libqmi's
`qmi-service-loc.json`); gpsd's TPV has mode 3 with that position;
location-daemon sends `Running`, `FixStatusChanged 3` and
`PositionChanged 48.2082 16.3738 171`; the example prints `lat: 48.208200`,
`lon: 16.373800` and stops. With patch 0002 (2026-10-31, the VM's run
of the test as it is now): location-daemon ended 11 s after the example,
ran the hook's `stop`, pro1x-gnss sent `LOC Stop session=1` and released
its client, gpsd and pro1x-gnss were gone; before, both ran on.

**The dialogs and the applet in the VM** (`test/vm-location.sh`, the
same chain in a Hildon session, location-ui run as hildon-session runs
it, the schema's defaults installed as hildon_setup does): liblocation's
example asks; location-ui shows the disclaimer
(`~/ps/pro1x-screens/bsd-location-disclaimer.png`), Accept: GConf's
`disclaimer-accepted` true, the position arrives. With GPS and network
off: "Enable positioning?" with GPS and Network positioning
(`bsd-location-enable.png`), GPS ticked (`bsd-location-enable-gps.png`),
Done: `gps-disabled` false, the position arrives. Settings, Connectivity,
Location: the applet with GPS enabled (`bsd-location-applet.png`). The
dialogs' Done and the applet's Save show the text IDs `wdgt_bd_done` and
`wdgt_bd_save` in English: the stage's `en_GB/LC_MESSAGES/hildon-libs.mo`
is libhildon's own catalog, not Leste's (the packages exclude it, the
stage does not); not ours to fix here.

On the PC:
pro1x-gnss against the same simulated LOC (`-d`) writes GGA and RMC with
the position (4812.4920 N, 01622.4280 E, 2.92 knots) and sends LOC Stop
on SIGTERM.

The device run (`pro1x.test=modem pro1x.modem=gps pro1x.modem_wait=600`,
outdoors or at a window; the window is the time to the first fix): the
modem's lines, then the lock held as a client would, location-daemon
activated with liblocation's call (or started by hand if activation fails),
`gpsd on udp://127.0.0.1:29470, pro1x-gnss`, `pro1x-gnss: session 1
started`, `engine on`; with a fix gpspipe's GGA/RMC lines with a position
and a `"class":"TPV"` with `"mode":3`, `PositionChanged` signals; without
one (indoors) `position in-progress` reports and no NMEA. Risks: dsp6;
QMI LOC on this firmware needs nothing else from the apps processor on
Leste (positions arrive there), but its first fix without assistance
data (no network for XTRA) can take many minutes; the engine's time comes
from the network or the satellites.

## Stage 5: SMS in Conversations

On Leste (`hildon-connectivity-rtcom`) the messages are Conversations':
it is a Telepathy client (Telepathy Qt 6), and the SMS come and go through
**telepathy-ring**, Telepathy's connection manager for oFono, under
**Mission Control** (Debian's telepathy-mission-control-5 5.16.6, on
telepathy-glib 0.24). The cellular account `ring/tel/account0`
("Cellular", enabled, connected automatically, `always_dispatch`) is not
made by any setup tool: telepathy-ring's own Mission Control plugin
(`mcp-account-manager-ring`, a read-only account storage) gives it. Leste
starts `conversations --background` with the X session (its xsession
file, under dsmetool), which asks Mission Control for the accounts and so
brings up ring's connection; incoming messages reach Conversations through
Mission Control's dispatch, and Conversations logs every message itself in
rtcom-eventlogger (service SMS, local uid `ring/tel/account0`). sphone
does the calls only there (leste-config: `Features=calls`, without the
messages UI), so that no message is logged and announced twice.

| Piece | Leste | FreeBSD |
| --- | --- | --- |
| telepathy-glib, Mission Control | Debian's 0.24.1 (Leste's build), 5.16.6 | FreeBSD's packages, 0.24.2 and 5.16.6 (plugins from `/usr/local/lib/mission-control-plugins.0`), already in the root image |
| telepathy-ring | maemo-leste-upstream-forks, 2.3.24.0 | the same (new part, `220b1053ef08`); patches: 0001 GLib's random UUID for the incoming messages' tokens (libuuid is not FreeBSD's), 0002 SMS only (`-DRING_SMS_ONLY`, below) |
| the account | ring's Mission Control plugin | the same plugin, in the package |
| Conversations at the session's start | xsession, `post 20` | `hildon-session` (`respawn conversations --background`) |
| sphone | `Features=calls`, comm-voicecallmanager | `Features=calls`, comm-ofono, without ui-messages-gtk and ui-message-threads-gtk (`extra/.../50-pro1x-freebsd.ini`, package pro1x-modem) |

**SMS only** (`hildon/patches/telepathy-ring/0002`): Leste's calls go
through ring too (voicecall-qt5), which FreeBSD's Hildon has not; with no
client for ring's call channels Mission Control would close them, which
for ring is a hangup of every incoming call (and of the ones sphone dials).
So ring leaves the VoiceCallManager alone ("ignoring interface"), and the
calls stay with sphone on pro1x-modemd as before.

**What ring asks of pro1x-modemd**: `GetModems` (the modem's properties:
powered and online picks it), `SimManager.GetProperties`
(`SubscriberIdentity`), `MessageManager.GetProperties`
(`UseDeliveryReports`, `ServiceCenterAddress`) and `GetMessages` (the
messages still being sent) when it connects, then `IncomingMessage` and
`ImmediateMessage` (`Sender`, `SentTime`), `SendMessage` and the
message's `PropertyChanged` (`State` `sent`: Telepathy's delivery report
"accepted", `failed`: a failed one), `MessageRemoved`. All of it was
there for sphone. It reads neither NetworkRegistration nor
RadioSettings, and nothing in ring sets `UseDeliveryReports`, so the
network's status reports (oFono's `SmsHistory.StatusReport`) are never
asked for; pro1x-modemd does not offer them. Changed in pro1x-modemd
(pro1x-modem 1.1): ring takes a message's object path as its token, which
Conversations keeps in the event log to find the message a delivery
report is for; the paths counted from `message_01` with every start and
would have matched old messages after a reboot. They are `message_` and
40 random hex digits now, as oFono's UUIDs (`test_ofono.py` checks the
form and that two messages differ). ring subscribes to a message's
`PropertyChanged` only once `SendMessage` has answered; a network takes a
second or more for the message, so that is in time (the simulated modem
answers Raw Send after 1.5 s in the VM, `fakemodem.py --send-delay`; at
once in `run.sh`).

**Conversations' delivery reports** (`hildon/patches/conversations/0001`):
an SMS sent stayed "pending" in the event log for ever. Conversations kept
the sent message's token from the message's header, which
telepathy-glib's message mixin never fills (it passes the token as
`MessageSent`'s argument only): no `message-token` header, and every
report was "the original message for this delivery report was not found".
It takes the sent token now. Leste's Conversations has the same code.
**Its launcher** (`0002`): `conversations` starts `conversations_qml` and
waits; Linux kills the child with the launcher (`PR_SET_PDEATHSIG`), on
FreeBSD it outlived it. hildon-session ends the background instance by
ending the launcher, and X stays for the next desktop (pro1x-desktop): the
old instance would have stayed on the dead session bus, and the next
session's launcher only woken it over its socket, every 2 s, with no SMS
received. FreeBSD's `procctl(PROC_PDEATHSIG_CTL)` does the same now.

**Tried in the VM** (`test/vm-conversations.sh`, freebsd/vm; FreeBSD's
telepathy-mission-control installed, telepathy-ring's files below
/usr/local as its package has them, the Conversations of 0001):
pro1x-modemd's FreeBSD build on the simulated modem, this repository's
`hildon-session`, which started `conversations --background`; Mission
Control (D-Bus activated) listed `ring/tel/account0` "Cellular",
connected (`ConnectionStatus` 0), ring asked `GetModems`,
`SimManager.GetProperties`, `MessageManager.GetProperties` and
`GetMessages` and nothing of the VoiceCallManager. An SMS in with umlauts,
braces and the Euro sign: Conversations' notification
(`~/ps/pro1x-screens/bsd-conversations-notification.png`), one event in
rtcom-eventlogger (service SMS, `ring/tel/account0`, unread; none from
sphone); the Events page of the MeeGo-like look lists it under
Notifications (`bsd-conversations-events.png`), a tap opens Conversations
(`bsd-conversations-from-events.png`) and marks it read; the overview
(`bsd-conversations-overview.png`), the chat
(`bsd-conversations-chat.png`). A reply typed and sent with the Send
button (`bsd-conversations-sent.png`): `SendMessage` from ring, at the
modem `Raw-Send` to the sender with the text, the event outgoing with
the message's path as `message-token` and its pending flag cleared once
the modem had sent it; refused by the network (`fail-sends 1`): the
event's permanent error flag. The launcher ended (as hildon-session ends
it): `conversations_qml` gone with it at once, both started again by the
session's respawn, the next SMS received and logged. A call coming in
while ring is connected:
sphone's call window (`bsd-conversations-call-in.png`), the call
untouched until the far end hung up. Doing it again:

    tar -C <dir with pro1x-modemd, lib/ (libqmi-glib, libqrtr-glib,
        librtcom-eventlogger with its 0001), fakemodem.py, pro1x-modemd.conf,
        hildon-session, 50-pro1x-freebsd.ini, modules/, vm-conversations.sh>
        -czf - . | vm 'mkdir -p /root/convtest && tar -xzf - -C /root/convtest'
    vm sh /root/convtest/vm-conversations.sh start   # then accounts, ctl, click,
                                                     # type, shot, db, events, stop

**On the phone** (not run yet; after the headless runs above, as the
user, with a SIM and a second phone): pro1x_modem and pro1x_modemd on in
`rc.conf`, the SIM PIN file, Hildon started. In an X Terminal:

    dbus-send --session --print-reply --dest=org.freedesktop.Telepathy.AccountManager \
        /org/freedesktop/Telepathy/Account/ring/tel/account0 \
        org.freedesktop.DBus.Properties.Get \
        string:org.freedesktop.Telepathy.Account string:ConnectionStatus

answers `uint32 0` once the SIM is ready (1: connecting, 2: no modem).
An SMS from the other phone: the notification, Conversations, the Events
page; a reply from Conversations' chat reaches the other phone, and
pro1x-modemd's log has `sms: to ..., part 1 of 1 ... sent`. A call to the
phone still rings in sphone. What the VM could not tell: the real
modem's timing between `SendMessage` and its answer, and Mission Control
and ring on the phone's memory beside Hildon.
