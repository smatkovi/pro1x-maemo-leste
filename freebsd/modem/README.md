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
| 2 | the voice of a call: the ADSP's voice path (MVM, CVP, the AFE's codec DMA ports) | `pro1x-callaudio` over `/dev/apr` | see "Stage 2" |
| 3 | GPS: the modem's GNSS (QMI LOC) into gpsd, positions for Hildon's apps | `pro1x-gnss`, gpsd, Leste's `location-daemon` | see "Stage 3" |

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
| `MessageManager`: `SendMessage`, `IncomingMessage`, `ImmediateMessage` (class 0), `MessageAdded`, `MessageRemoved` | WMS |
| `Message`: `State` (`pending`, `sent`, `failed`) | WMS |

The rest of oFono's methods answer `org.ofono.Error.NotImplemented`
(hold, swap, multiparty, call forwarding and barring, USSD, cell
broadcast, mobile data). sphone's `comm-ofono` has no DTMF (only its test
backend does); `SendTones` is there for other clients.

**Conversations** shows sphone's messages (the event log), but sending
from it needs telepathy-ring (with telepathy-glib and mission-control's
plugins), which FreeBSD's Hildon does not have yet: deferred. Since
telepathy-ring is an oFono client as well, it would need no backend
either; its use of `org.ofono.VoiceCallAgent` and `SmsHistory` (Nokia's
oFono extensions) would have to be checked then.

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
  (the durations are ours).

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
| gps | `pro1x.test=modem pro1x.modem=gps pro1x.modem_wait=600` | stage 3 |

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

Not written yet.

## Stage 3: GPS

Not written yet.
