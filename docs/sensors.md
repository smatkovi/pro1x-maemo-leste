# Sensors: the ADSP's sensor core in its power-saving modes

The Pro1X's sensors (accelerometer, gyroscope, magnetometer, light,
proximity, the step counter) belong to the ADSP: Qualcomm's sensor core
(SEE) runs there, and the Linux systems and FreeBSD reach it over QRTR
(service 400) through libssc. hexagonrpcd serves the ADSP its sensor
configuration and registry (`scripts/46-sensor-stack.sh`). This page is
about how the sensor core delivers its events to the application processor
and what the repository asks of it since 2026-10-31.

## Before (libssc 0.4.4 with patches/libssc-pedometer and -sync-wait)

- Every sensor object opens its own QMI client and sends one enable
  request: streaming sensors with the **lowest** sample rate the sensor
  lists (the first positive one of its rates attribute; `ssccli --probe
  accel` shows it), on-change sensors with an empty request.
- Every request asked for **WAKEUP** delivery and **no batching**: one QMI
  indication per sample or event, so every accelerometer sample woke the
  application processor and every reader of it; the step counter sent one
  event per step (about two a second while walking), which pro1x-steps
  coalesced into a D-Bus signal every 5 s, but the CPU still woke for each.
- In a suspend the events went on coming: the ADSP's glink edge is no
  wake-up source (only the modem's is, kernel patch 0003), its interrupt
  is `IRQF_NO_SUSPEND`, so every event still brings the CPUs out of s2idle
  for a moment, and the data piles up in the FIFO until the resume.

Who switches what on (unchanged, it was already right):

| Client | Sensor | On while |
|---|---|---|
| mce (Leste, MeeGo session) via iio-sensor-proxy | accelerometer | someone listens for the orientation and the display is not off, or a call or alarm rings (`iio-accelerometer.c`'s claim policy) |
| mce via iio-sensor-proxy | light | the display is on (`iio-als.c`) |
| mce via iio-sensor-proxy | proximity | a call rings or runs, an alarm shows (`iio-proximity.c`) |
| n9-sensors (MeeGo session) | accelerometer | the display is on or dimmed (`313d828`) |
| n9-sensors | proximity | **always** (changed, below) |
| pro1x-steps | step counter | always (that is its point) |
| pro1x-proximity (FreeBSD) | proximity | a client watches it (the stand-by screen) |
| pro1x-light | light | one reading for the photo light meter, then off |

## What the sensor core offers

Its client API (`sns_client.proto`, `sns_std.proto`, `sns_std_sensor.proto`,
`sns_pedometer.proto` of Qualcomm's SEE, as Android vendor images carry them
in `/vendor/etc/sensors/proto`). libssc's own protocol files had the fields
under other names; the numbers and wire types are the same:

| libssc (`data/ssc-common.proto`) | Sensor core | Meaning |
|---|---|---|
| `SscClientRequest.uid` (1), `.msg_id` (2), `.config` (3), `.request` (4) | `sns_client_request_msg.suid`, `.msg_id`, `.susp_config`, `.request` | |
| `SscClientConfig.processor` (1) | `suspend_config.client_proc_type` | 1: the application processor |
| `SscClientConfig.suspend_mode` (2) | `suspend_config.delivery_type` | 0 WAKEUP: send when due, also into a suspend. 1 NO_WAKEUP: while the processor is suspended keep the events, send them all after the resume |
| `SscClientConfig.no_wakeup_msg_ids` (3, new) | `suspend_config.nowakeup_msg_ids` | message ids that do not wake even for a WAKEUP client |
| `SscClientRequestBody.batching` (1, was `bytes unknown`) | `sns_std_request.batching` (`batch_spec`) | none: no batching |
| `SscBatchSpec.batch_period` (1, uint32 µs) | `batch_spec.batch_period` | events wait up to this long and go out together; 0: no batching |
| `SscBatchSpec.flush_period` (2) | `batch_spec.flush_period` | at least this much of the newest events is kept; default batch period + 0.75 s |
| `SscBatchSpec.flush_only` (3) | `batch_spec.flush_only` | send only on a flush request or a full buffer |
| `SscBatchSpec.max_batch` (4) | `batch_spec.max_batch` | batch at full capacity |
| `.msg` (2), `.is_passive` (3) | `.payload`, `.is_passive` | |

Message ids: 513 enable a streaming sensor (`sns_std_sensor_config`, its
sample rate), 514 enable an on-change sensor, 10 disable, 1 attributes,
**2 flush** (empty; the sensor core sends what it holds for the client,
then **129**, the flush event), 130 error event, 768 configuration event,
1025 standard sensor event, 769 proximity event, **1028 step event** and
**775 the step count when a request comes in** (`sns_step_event`:
`step_count`, field 1, the count since the sensor was activated).

The client manager's rules (its code and the Pro1X's registry,
`sensors/registry/sns_cm` of the NixOS port's hexagonfs):

- batch periods below `min_batch_period_thrshld_ms` (10 ms here) count as
  0, periods below 1 ms are refused;
- the batch buffer is `heap_size` (1.5 MB here); for a NO_WAKEUP client in
  a suspend the **oldest recurrent events** (ids 1024-1536, samples, step
  events) are dropped when it is full, configuration events (775) are not;
- sending the enable request again for an open sensor reconfigures it:
  the client manager first sends everything it holds for the client, then
  goes on with the new batch period; the sensor stays on;
- a non-waking request from a client whose processor is awake is sent like
  a waking one, at its batch period.

The sensors themselves (registry of the NixOS port):

- **Step counter:** `sns_pedometer` (data type `pedometer`, on-change,
  from the accelerometer). It counts on the ADSP whatever the application
  processor does: a client's delivery only decides when the events are
  sent. Every event carries the whole count since activation, so batching
  and dropping lose nothing: the newest event after a resume or a flush
  has it all. There is no separate step detector in the registry.
- **Light and proximity:** the TMD2725 (`tmd2725_0.als`, `.prox`, both
  `is_dri` 1: interrupt-driven), on-change by the sensor core's API: light
  reports a significant change only (the API's example: ±10 %), proximity only
  near/far transitions, with the thresholds of its factory calibration in
  the registry. There is no client threshold in the standard request;
  libssc opens them on-change when their attributes say so.
- **Suspend:** the sensor core learns that the application processor
  suspends from bit 12 of the SMP2P entry `sleepstate`
  (its `remote_proc_state` sensor). Mainline's `sm6115.dtsi` has no such
  entry, so the sensor core takes the system as always awake and
  NO_WAKEUP behaves like WAKEUP until the kernel has patches 0038/0039
  (below). Whether the Pro1X's ADSP firmware has `remote_proc_state` at
  all cannot be seen from the PC: `ssccli --probe remote_proc_state` on
  the phone tells.

## What changed

**libssc** (`patches/libssc-wakeup-batching.patch`, after the other two;
FreeBSD's copy in `freebsd/hildon/patches/libssc/`):

- the batch spec and the delivery in every enable request:
  `ssc_sensor_set_delivery(sensor, wakeup, batch_period_us,
  flush_period_us, flush_only)` per sensor, else the defaults of
  `/etc/libssc.conf` (FreeBSD: `/usr/local/etc/libssc.conf`; `LIBSSC_CONF`
  names another, empty for none; `LIBSSC_CONTINUOUS_BATCH_MS` and
  `LIBSSC_NO_WAKEUP` override it for one process). Without the file and a
  client's own setting the requests are byte for byte those of before;
- `ssc_sensor_flush()` (completes on the flush event, fails after 5 s) and
  `ssc_sensor_reconfigure()` (sends the enable request again with the new
  delivery);
- the step counter reads 1028 and 775 (and a standard event's first
  float); flush, error and configuration events are no longer read as
  counts (each flush would have logged a warning);
- `ssccli --batch-ms MS --no-wakeup --flush-only --flush-every S` for any
  sensor, `ssccli --probe TYPE` (name, vendor, stream type, lowest rate);
- the `stream-type`, `available` and `sample-rate` properties were string
  properties holding a number; now readable.

**`/etc/libssc.conf`** (Leste, `rootfs-overlay/etc/libssc.conf`, installed
by `46-sensor-stack.sh` where there is none): streaming sensors batched
200 ms, everything except the proximity sensor non-waking. That is what
reaches mce through iio-sensor-proxy (orientation at most 200 ms later, a
wakeup for several samples), without rebuilding iio-sensor-proxy. Arch and
Kali take libssc from Leste (`arch/20`, `kali/20`); their
`/etc/libssc.conf` is the same file if copied, else they keep the old
behaviour.

**pro1x-steps** (1.1): the step counter non-waking and batched: 5 s while
the display is on (or no mce says otherwise: FreeBSD, Arch, Kali), 10
minutes while it is off. When the display comes on, the request goes out
again with 5 s: the sensor core hands over what it held, so the count
after a walk in the pocket or a suspend is there with the screen.
`GetSteps` flushes first and answers within 0.8 s (the widget's call waits
2 s). Where the sensor core has no step counter (other phones) it counts
from the accelerometer itself, batched the same way (that keeps the
ADSP's accelerometer on and counts only while the system is awake).

**n9-sensors** (MeeGo session): the proximity sensor only while the display
is on or a call rings or runs (`sig_call_state_ind`), WAKEUP, as mce's own
module; it was on all the time. The accelerometer in 200 ms batches,
non-waking, still only with the display on.

**Kernel** (patches 0038, 0039; test build `$WORK/out-pro1x.11-sensors`,
7.1.0-pro1x.11-sensors = pro1x.11 + these two, not flashed):

- 0038 `soc: qcom: smp2p-sleepstate`: bit 12 of the entry set at probe and
  after every suspend, cleared when one starts (`PM_SUSPEND_PREPARE`), as
  Android's `smp2p_sleepstate.c`; built in (`default QCOM_SMP2P`, no
  fragment change). `echo N > /sys/module/smp2p_sleepstate/parameters/report_suspend`
  keeps it set (as without the driver).
- 0039 the outbound entry `sleepstate` in the ADSP's smp2p node and the
  node for the driver, as the vendor device tree (`bengal.dtsi`) has them.
  The vendor's inbound `sleepstate_see` (the sensor core asking for a
  wakeup) is left out.

With both, NO_WAKEUP is what it says: during a suspend the step events
(and anything else non-waking) stay in the ADSP and come after the resume;
nothing from the sensor core reaches the suspended system unless a client
asked for WAKEUP (the proximity sensor in a call, where the system does
not suspend). Without them everything still works, batched; the step
counter then sends a report at most every 10 minutes into a suspend.

Not changed: FreeBSD has no sleepstate (no 0038/0039 there), so
non-waking is waking in practice; FreeBSD gets no `libssc.conf` (no
accelerometer client there). pro1x-proximity and pro1x-light need nothing
new: they already have their sensor on only while needed.

## The way back

- `/etc/libssc.conf` away (or its keys removed): every libssc client as
  before at its next open (restart iio-sensor-proxy, mce, pro1x-steps, or
  reboot).
- pro1x-steps sets its own delivery; the previous build is
  `git show f01565a:tools/pro1x-steps/pro1x-steps.c`.
- Kernel: `report_suspend=N` (above), or boot pro1x.11.

## PC tests

    bash tests/sensors/run.sh            # QRTR=1: load the PC's qrtr module for the last part

1. libssc v0.4.4 with the three patches, built against the PC's libqmi
   (as on the phone), without warnings; FreeBSD's copies of the patches
   the same as Leste's.
2. The requests and reports against the sensor core's field numbers
   (`tests/sensors/see-client.proto`, written for the tests from the
   numbers above, and `check.py`): the old request byte for byte, the
   delivery, batch spec, flush period, flush and disable requests,
   `libssc.conf` and its environment overrides, step counts from 1028,
   775 and standard events, a batched report with several events and a
   flush event.
3. pro1x-steps and n9-sensors compiled against it.
4. pro1x-steps against a fake sensor core (`fake-ssc.py`: the QMI SSC
   service on the PC's own QRTR node, discovery, attributes, a step
   counter and an accelerometer, batch periods, flushes, reconfiguring as
   the client manager does them), the real libssc and libqmi in between,
   a system bus and a fake mce of its own (`steps.py`): non-waking 5 s
   batches with the display on, 10 minutes with it off, a batch as one
   report, GetSteps with a flush (0.8 s at most, also when the flush
   answer never comes), the held steps at once when the display comes on,
   a step counter starting over, the disable at the end, the accelerometer
   where there is no step counter. It found two faults that are fixed: the
   first event of a batch went out as StepsChanged on its own (the signal
   now waits for the rest of the report), and the accelerometer's step
   detector smoothed a step's swing away.

2026-10-31: all 66 checks pass. Not tested on the PC: the real sensor
core (whether it honours the batch spec for the step counter, the
sleepstate entry) and n9-sensors' uinput side; only the phone can show
those.

## Device tests (2026-10-31)

Leste, over USB (`ssh root@172.16.42.1`), Wi-Fi up for the phone's
`git fetch` of libssc. First the new libssc and tools from the PC's HEAD
(a few minutes; hexagonrpcd, libqmi and iio-sensor-proxy stay as they
are), the MeeGo session's n9-sensors too where it is used:

    git -C $WORK/repo archive HEAD scripts/46-sensor-stack.sh patches tools/pro1x-steps \
        meego/sensors rootfs-overlay/etc/libssc.conf rootfs-overlay/etc/init.d/pro1x-steps \
        rootfs-overlay/etc/dbus-1/system.d/org.pro1x.Steps.conf |
      ssh root@172.16.42.1 'set -e; t=$(mktemp -d); tar -C $t -xf -
        PARTS="libssc tools" bash $t/scripts/46-sensor-stack.sh
        [ ! -d /usr/lib/pro1x-meego ] || gcc -O2 -Wall -o /usr/lib/pro1x-meego/n9-sensors \
          $t/meego/sensors/n9-sensors.c $(PKG_CONFIG_PATH=/usr/local/lib/aarch64-linux-gnu/pkgconfig \
          pkg-config --cflags --libs libssc) -lm
        rm -r $t'
    ssh root@172.16.42.1 reboot

The MeeGo copy the same way inside it; Arch and Kali take libssc and
pro1x-steps from Leste (`arch/20-pro1x-hardware.sh`,
`kali/20-pro1x-hardware.sh` copy them into `/usr/lib/pro1x`), so after
Leste's update those two copies again (plus `/etc/libssc.conf`).
Steps 1-5 on the running kernel; 6 needs the test kernel.

1. **What the sensor core has** (2 min):

       for t in pedometer accel ambient_light proximity remote_proc_state; do ssccli --probe $t; done

   Expected: pedometer, light and proximity on-change, the accelerometer
   streaming. `remote_proc_state` decides whether step 6 can work.

2. **Batching seen** (5 min, pro1x-steps stopped: `rc-service pro1x-steps stop`):

       ssccli -v --sensor pedometer --no-wakeup --batch-ms 5000 --timeout 60 2>&1 | grep -E "Message (1028|775)|measurement"

   Walk with the phone: the step events come in groups every 5 s
   (several `Message 1028` lines with one time stamp), not one per step.
   Then `--batch-ms 0`: one per step, as before.

3. **Flush** (2 min): `ssccli --sensor pedometer --no-wakeup --batch-ms 600000 --flush-every 20 --timeout 70`
   while walking: counts only at each "Flush complete".

4. **pro1x-steps** (5 min, `rc-service pro1x-steps start`):

       dbus-send --system --print-reply --dest=org.pro1x.Steps /org/pro1x/Steps org.pro1x.Steps.GetSteps
       dbus-monitor --system "interface='org.pro1x.Steps'" &

   20 steps: GetSteps up by about 20 at once (the flush), StepsChanged at
   most every 5 s. Display off (power key), 20 steps, display on: one
   StepsChanged within about a second with all 40.

5. **Current, step counter on vs off** (2 x 15 min, unplugged or charger
   input suspended, display off, still on the table, then in the pocket
   walking if wanted):

       rc-service pro1x-steps start; pro1x-powerlog steps-on 15
       rc-service pro1x-steps stop;  pro1x-powerlog steps-off 15

   The last line of each log is the mean current with the display off.
   Also `tools/pro1x-autosuspend/wakemeas.sh 60` both ways: with the
   counter on and the phone still, the ADSP's glink interrupts
   (`glink-smem` of the ADSP edge) should not rise above the off run;
   walking, at most one report per 10 minutes. The old pro1x-steps
   (`f01565a`) for comparison, if time: one interrupt per step.

6. **Kernel 0038/0039** (could, 20 min; flashing needs an OK, or a
   kexec start, docs/kernel-update.md):
   `OUT=$WORK/out-pro1x.11-sensors ... 65-kernel-update.sh prepare`.
   `dmesg | grep sleepstate` shows "the DSPs learn of system suspends".
   GetSteps, `pro1x-autosuspend --test 60` while walking (the phone in
   the hand), GetSteps right after the resume: the steps of the suspend
   are there; `/proc/interrupts` of the ADSP's glink edge rose by a few at
   the resume only, not during it. Way back without a boot:
   `echo N > /sys/module/smp2p_sleepstate/parameters/report_suspend`.

| Step | Result |
|---|---|
| 1 probe | |
| 2 batching | |
| 3 flush | |
| 4 pro1x-steps | |
| 5 current on / off (mA) | |
| 6 kernel | |

The power measurements of the whole system and the other open points:
[power-management.md](power-management.md).
