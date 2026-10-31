#!/usr/bin/python3
# A stand-in for the ADSP's sensor core on the PC, for tests/sensors/steps.py:
# the QMI SSC service (400) on the local QRTR node (the PC's qrtr module,
# its name service in the kernel), with a step counter, an accelerometer, a
# light and a proximity sensor, and the parts of the client manager that
# the power-saving delivery depends on (docs/sensors.md):
#
#   - per client and sensor one request; its batch period holds the events
#     and sends them together when it runs out (0: at once);
#   - a flush request (2) sends what is held, then the flush event (129);
#   - the enable request again (reconfiguring) sends what is held first;
#   - the step counter answers every request with the count so far (775),
#     each step is an event 1028 with the count since activation.
#
#   fake-ssc.py BUILD CTL LOG [--no-pedometer]
#
# BUILD holds see_client_pb2.py (protoc of see-client.proto). CTL is a UNIX
# datagram socket for the test's commands:
#   walk N            N steps (the step counter's count grows by N)
#   accel-steps N     N steps' worth of accelerometer samples (13 Hz)
#   ignore-flush 0|1  answer flush requests or not (a firmware that drops one)
#   restart N         the step counter starts over at N (an ADSP restart)
# LOG gets one JSON line per request (client, data type, msg_id, delivery,
# batch and flush period) and per report sent.
import json
import math
import os
import select
import socket
import struct
import sys
import time

build, ctl_path, log_path = sys.argv[1:4]
sys.path.insert(0, build)
import see_client_pb2 as see  # noqa: E402

NO_PEDOMETER = "--no-pedometer" in sys.argv[4:]

SSC_SERVICE, SSC_VERSION, SSC_INSTANCE = 400, 1, 0
QRTR_PORT_CTRL = 0xFFFFFFFE
QRTR_TYPE_NEW_SERVER = 4
QMI_REQUEST, QMI_RESPONSE, QMI_INDICATION = 0, 2, 4
SSC_CONTROL, SSC_REPORT_LARGE = 0x20, 0x22
SUID_UID = (0xABABABABABABABAB, 0xABABABABABABABAB)  # high, low

SENSORS = {
    # data type: uid (high, low), attributes
    "registry": ((0x10, 0x10), {}),
    "pedometer": ((0x20, 0x21), {"stream": 1}),
    "accel": ((0x30, 0x31), {"stream": 0, "rates": [13.0, 26.0, 52.0]}),
    "ambient_light": ((0x40, 0x41), {"stream": 1}),
    "proximity": ((0x50, 0x51), {"stream": 1}),
}
if NO_PEDOMETER:
    del SENSORS["pedometer"]
BY_UID = {uid: t for t, (uid, _) in SENSORS.items()}

log_file = open(log_path, "a", buffering=1)


def log(**kw):
    kw["t"] = round(time.monotonic(), 3)
    log_file.write(json.dumps(kw) + "\n")


# --- QMI over QRTR ------------------------------------------------------

def tlvs(msg):
    out, i = {}, 7
    while i + 3 <= len(msg):
        t, n = msg[i], struct.unpack_from("<H", msg, i + 1)[0]
        out[t] = msg[i + 3:i + 3 + n]
        i += 3 + n
    return out


def qmi(kind, txn, msg_id, fields):
    body = b"".join(struct.pack("<BH", t, len(v)) + v for t, v in fields)
    return struct.pack("<BHHH", kind, txn, msg_id, len(body)) + body


sock = socket.socket(socket.AF_QIPCRTR, socket.SOCK_DGRAM)
node, _ = sock.getsockname()


def send(data, addr):
    try:
        sock.sendto(data, addr)
    except OSError as e:  # a client that is gone
        log(send_failed=addr[1], error=str(e))

# publish the service with the kernel's name service (as libqrtr does)
sock.sendto(struct.pack("<5I", QRTR_TYPE_NEW_SERVER, SSC_SERVICE,
                        SSC_INSTANCE << 8 | SSC_VERSION, 0, 0), (node, QRTR_PORT_CTRL))

ctl = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
if os.path.exists(ctl_path):
    os.unlink(ctl_path)
ctl.bind(ctl_path)

TICKS = 19200000  # the sensor core's 19.2 MHz time stamps


def report(addr, uid, events):
    """one indication: one sensor, any number of (msg_id, payload)"""
    m = see.ClientEventMsg()
    m.suid.high, m.suid.low = uid
    for msg_id, payload in events:
        e = m.events.add()
        e.msg_id = msg_id
        e.timestamp = int(time.monotonic() * TICKS)
        e.payload = payload
    data = m.SerializeToString()
    send(qmi(QMI_INDICATION, 0, SSC_REPORT_LARGE,
             [(0x01, struct.pack("<Q", 1)), (0x02, struct.pack("<H", len(data)) + data)]), addr)
    log(report=BY_UID.get(uid, "suid"), client=addr[1], msg_ids=[e[0] for e in events])


# --- the sensors ----------------------------------------------------------

step_count = 0
ignore_flush = False
streams = {}  # (addr, uid) -> request state


def step_event(n, config=False):
    s = see.StepEvent()
    s.step_count = n
    return (775 if config else 1028), s.SerializeToString()


def accel_event(x, y, z):
    s = see.StdSensorEvent()
    s.data.extend([x, y, z])
    s.status = 3
    return 1025, s.SerializeToString()


def queue(uid, event):
    for (addr, u), st in streams.items():
        if u != uid:
            continue
        st["held"].append(event)
        if st["batch_us"] == 0:
            send_held(addr, u)
        elif st["due"] is None:
            st["due"] = time.monotonic() + st["batch_us"] / 1e6


def send_held(addr, uid, extra=()):
    st = streams[(addr, uid)]
    events = st["held"] + list(extra)
    st["held"], st["due"] = [], None
    if events:
        report(addr, uid, events)


def attributes(data_type):
    a = SENSORS[data_type][1]
    ev = see.AttrEvent()

    def attr(i, **v):
        x = ev.attributes.add()
        x.attr_id = i
        d = x.value.values.add()
        for k, val in v.items():
            setattr(d, k, val)
        return x
    attr(0, str=data_type + "-fake")
    attr(1, str="pro1x-test")
    attr(3, boolean=True)
    x = ev.attributes.add()
    x.attr_id = 6
    for r in a.get("rates", [0.0]):
        x.value.values.add().flt = r
    attr(16, sint=a.get("stream", 0))
    return ev.SerializeToString()


def handle(addr, msg):
    if len(msg) < 7 or addr[1] == QRTR_PORT_CTRL:
        return  # the name service's notices
    kind, txn, msg_id = struct.unpack_from("<BHH", msg)
    if kind != QMI_REQUEST or msg_id != SSC_CONTROL:
        return
    data = tlvs(msg).get(0x01, b"\0\0")[2:]
    # the QMI answer first, as the client manager does
    send(qmi(QMI_RESPONSE, txn, SSC_CONTROL,
             [(0x02, struct.pack("<HH", 0, 0)), (0x10, struct.pack("<I", 1))]), addr)
    r = see.ClientRequest()
    r.ParseFromString(data)
    uid = (r.suid.high, r.suid.low)
    b = r.request.batching if r.request.HasField("batching") else None
    entry = {"client": addr[1], "msg_id": r.msg_id, "delivery": r.susp_config.delivery,
             "batch_us": b.batch_period if b else 0,
             "flush_us": b.flush_period if b and b.HasField("flush_period") else 0}
    if uid == SUID_UID:
        q = see.SuidRequest()
        q.ParseFromString(r.request.payload)
        entry["discover"] = q.data_type
        log(**entry)
        ev = see.SuidEvent()
        ev.data_type = q.data_type
        if q.data_type in SENSORS:
            s = ev.suid.add()
            s.high, s.low = SENSORS[q.data_type][0]
        report(addr, SUID_UID, [(768, ev.SerializeToString())])
        return
    data_type = BY_UID.get(uid)
    entry["sensor"] = data_type
    log(**entry)
    if data_type is None:
        return
    key = (addr, uid)
    if r.msg_id == 1:
        report(addr, uid, [(128, attributes(data_type))])
    elif r.msg_id in (513, 514):
        if key in streams:
            # reconfiguring: what is held goes out first
            send_held(addr, uid)
        streams[key] = {"held": [], "due": None, "batch_us": entry["batch_us"]}
        cfg = see.PhysicalConfigEvent()
        cfg.sample_rate = 13.0
        events = [(768, cfg.SerializeToString())]
        if data_type == "pedometer":
            events.append(step_event(step_count, config=True))
        report(addr, uid, events)
    elif r.msg_id == 2:
        if key in streams and not ignore_flush:
            send_held(addr, uid, [(129, b"")])
    elif r.msg_id == 10:
        streams.pop(key, None)


def accel_steps(n):
    """13 Hz samples of a walk at about two steps a second: gravity on z,
    swinging by 3.5 m/s^2 with each step (a phone in a trouser pocket swings
    by 2-4 m/s^2), then a second standing still"""
    for i in range(n * 7):
        swing = 3.5 * math.sin(2 * math.pi * i / 7)
        queue(SENSORS["accel"][0], accel_event(0.2, 0.3, 9.81 + swing))
    for i in range(13):
        queue(SENSORS["accel"][0], accel_event(0.2, 0.3, 9.81))


def command(line):
    global step_count, ignore_flush
    words = line.split()
    log(command=line)
    if words[0] == "walk":
        for _ in range(int(words[1])):
            step_count += 1
            if "pedometer" in SENSORS:
                queue(SENSORS["pedometer"][0], step_event(step_count))
    elif words[0] == "accel-steps":
        accel_steps(int(words[1]))
    elif words[0] == "ignore-flush":
        ignore_flush = words[1] == "1"
    elif words[0] == "restart":
        step_count = int(words[1])


log(started=True, node=node, pedometer=not NO_PEDOMETER)
while True:
    due = [st["due"] for st in streams.values() if st["due"] is not None]
    timeout = max(0.0, min(due) - time.monotonic()) if due else None
    ready, _, _ = select.select([sock, ctl], [], [], timeout)
    if sock in ready:
        msg, addr = sock.recvfrom(65536)
        handle(addr, msg)
    if ctl in ready:
        command(ctl.recv(4096).decode())
    now = time.monotonic()
    for (addr, uid), st in list(streams.items()):
        if st["due"] is not None and st["due"] <= now:
            send_held(addr, uid)
