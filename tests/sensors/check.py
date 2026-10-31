#!/usr/bin/python3
# Checks the patched libssc's requests and report parsing against the
# sensor core's field numbers (see-client.proto): run.sh builds the encode
# helper and the Python module of see-client.proto, then
#
#   python3 -I check.py BUILD_DIR
#
# BUILD_DIR holds see_client_pb2.py and the encode binary. Prints one line
# per check, exits 1 if one failed.
import os
import struct
import subprocess
import sys
import tempfile

build = sys.argv[1]
sys.path.insert(0, build)
import see_client_pb2 as see  # noqa: E402

ENCODE = os.path.join(build, "encode")
UID_HIGH = 0x1122334455667788
UID_LOW = 0x99AABBCCDDEEFF00
fails = 0


def check(name, ok, detail=""):
    global fails
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else "  " + detail))
    if not ok:
        fails += 1


def run(args, stdin=None, env=None):
    e = dict(os.environ)
    e.update(env or {})
    return subprocess.run([ENCODE] + args, input=stdin, capture_output=True,
                          text=True, env=e, check=True).stdout


# --- requests ------------------------------------------------------------
reqs = {}
for line in run(["requests"]).splitlines():
    name, hexdata = line.split()
    reqs[name] = bytes.fromhex(hexdata)


def decode(name):
    m = see.ClientRequest()
    m.ParseFromString(reqs[name])
    return m


def rate_payload(hz):
    c = see.StdSensorConfig()
    c.sample_rate = hz
    return c.SerializeToString()


# The request without a delivery must be the one libssc sent before the
# patch: built here with the sensor core's numbers, only the fields set
# that libssc set (no batch spec, no is_passive).
legacy = see.ClientRequest()
legacy.suid.low = UID_LOW
legacy.suid.high = UID_HIGH
legacy.msg_id = 513
legacy.susp_config.client_proc_type = 1
legacy.susp_config.delivery = 0
legacy.request.payload = rate_payload(13.0)
check("no delivery: same bytes as before the patch",
      reqs["legacy-null"] == legacy.SerializeToString(),
      reqs["legacy-null"].hex() + " != " + legacy.SerializeToString().hex())
check("default delivery: same bytes as no delivery",
      reqs["legacy-default"] == reqs["legacy-null"])

m = decode("legacy-null")
check("suid low/high in their fields", m.suid.low == UID_LOW and m.suid.high == UID_HIGH)
check("enable request: msg_id 513", m.msg_id == 513)
check("processor: APSS (1)", m.susp_config.client_proc_type == 1)
check("no batch spec without batching", not m.request.HasField("batching"))
c = see.StdSensorConfig()
c.ParseFromString(m.request.payload)
check("payload: sample_rate 13 Hz", abs(c.sample_rate - 13.0) < 1e-6)

m = decode("accel-batched")
check("accelerometer: NO_WAKEUP (1)", m.susp_config.delivery == 1)
check("accelerometer: batch period 200000 us",
      m.request.HasField("batching") and m.request.batching.batch_period == 200000)
check("accelerometer: no flush period (the core's default)",
      not m.request.batching.HasField("flush_period"))
check("accelerometer: not flush-only", not m.request.batching.flush_only)
check("accelerometer: payload kept", m.request.payload == rate_payload(13.0))
check("accelerometer: active request (no is_passive)", not m.request.HasField("is_passive"))

m = decode("pedometer")
check("pedometer: on-change enable msg_id 514", m.msg_id == 514)
check("pedometer: NO_WAKEUP", m.susp_config.delivery == 1)
check("pedometer: batch period 5 s", m.request.batching.batch_period == 5000000)
check("pedometer: no payload", not m.request.HasField("payload"))

m = decode("pedometer-idle")
b = m.request.batching
check("idle: batch 600 s, flush period 900 s, flush only",
      b.batch_period == 600000000 and b.flush_period == 900000000 and b.flush_only)

m = decode("flush-period-raised")
check("flush period below the batch period raised to it",
      m.request.batching.flush_period == m.request.batching.batch_period == 400000)

m = decode("proximity")
check("proximity: WAKEUP (0), no batch spec",
      m.susp_config.delivery == 0 and not m.request.HasField("batching"))

m = decode("flush")
check("flush request: msg_id 2, no payload, same delivery",
      m.msg_id == 2 and not m.request.HasField("payload") and m.susp_config.delivery == 1)

m = decode("disable")
check("disable request: msg_id 10, WAKEUP, nothing else",
      m.msg_id == 10 and m.susp_config.delivery == 0 and
      not m.request.HasField("batching") and not m.request.HasField("payload"))

# --- libssc.conf ---------------------------------------------------------
types = ["accel:1", "gyro:1", "proximity:0", "ambient_light:0", "pedometer:0"]


def defaults(env):
    out = {}
    for line in run(["defaults"] + types, env=env).splitlines():
        t, w, b = line.split()
        out[t] = (int(w), int(b))
    return out


with tempfile.TemporaryDirectory() as tmp:
    conf = os.path.join(tmp, "libssc.conf")
    with open(conf, "w") as f:
        f.write("[delivery]\ncontinuous-batch-ms=200\n"
                "no-wakeup=accel;mag;gyro;rotv;ambient_light;pedometer\n")
    d = defaults({"LIBSSC_CONF": conf})
    check("conf: streaming accelerometer batched 200 ms, no wakeup", d["accel"] == (0, 200000))
    check("conf: gyroscope batched, no wakeup", d["gyro"] == (0, 200000))
    check("conf: proximity wakes, not batched", d["proximity"] == (1, 0))
    check("conf: light (on-change) not batched, no wakeup", d["ambient_light"] == (0, 0))
    check("conf: pedometer no wakeup", d["pedometer"] == (0, 0))
    d = defaults({"LIBSSC_CONF": ""})
    check("no conf: everything as before (wakeup, unbatched)",
          all(v == (1, 0) for v in d.values()), str(d))
    d = defaults({"LIBSSC_CONF": conf, "LIBSSC_CONTINUOUS_BATCH_MS": "0",
                  "LIBSSC_NO_WAKEUP": "pedometer"})
    check("environment overrides the conf",
          d["accel"] == (1, 0) and d["pedometer"] == (0, 0), str(d))
    with open(conf, "w") as f:
        f.write("not a key file\n")
    d = defaults({"LIBSSC_CONF": conf})
    check("unreadable conf: as before", all(v == (1, 0) for v in d.values()), str(d))

# --- reports -------------------------------------------------------------


def step(n):
    s = see.StepEvent()
    s.step_count = n
    return s.SerializeToString()


std = see.StdSensorEvent()
std.data.append(56.0)
std.status = 3
cases = [step(0), step(1234), step(2**31 + 5), std.SerializeToString(), b"", b"\xff"]
out = run(["count"], stdin="".join(c.hex() + "\n" for c in cases)).split()
check("step event counts", out[:3] == ["0", "1234", str(2**31 + 5)], str(out))
check("standard sensor event: count as its first float", out[3] == "56", str(out))
check("empty and broken reports rejected", out[4:] == ["fail", "fail"], str(out))

# one report with what a batch or a flush brings: steps, the flush event
ev = see.ClientEventMsg()
ev.suid.low = UID_LOW
ev.suid.high = UID_HIGH
for msg_id, payload in [(1028, step(10)), (1028, step(11)), (775, step(11)),
                        (1028, step(12)), (129, b"")]:
    e = ev.events.add()
    e.msg_id = msg_id
    e.timestamp = 19200000
    e.payload = payload
out = run(["events"], stdin=ev.SerializeToString().hex() + "\n").split()
check("batched report: sensor uid", out[1:3] == ["%016x" % UID_HIGH, "%016x" % UID_LOW], str(out))
check("batched report: every event in order, the newest count last",
      out[3:] == ["1028:10", "1028:11", "775:11", "1028:12", "129:-"], str(out))

# sample rates the sensor core sends as float: the wire type is fixed32
check("sample_rate on the wire as a float (fixed32)",
      rate_payload(13.0) == b"\x0d" + struct.pack("<f", 13.0))

print("%d failed" % fails if fails else "all passed")
sys.exit(1 if fails else 0)
