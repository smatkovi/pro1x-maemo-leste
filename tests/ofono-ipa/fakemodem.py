#!/usr/bin/python3
"""A simulated QRTR modem for the test of oFono's IPA data path (PC only).

Registers DMS, UIM, WDS, NAS, WDA and DPM on the PC's local QRTR node and
answers what oFono's qrtrqmi driver sends while it enables an embedded
modem: DMS Get Operating Mode (low power), DPM Open Port, WDA Get/Set Data
Format. Every request is logged with its message id and TLVs in hex, so the
test can check the encoding. Requests it does not know (UIM, NAS, ...) get
no answer, as oFono does not need them for this.

    fakemodem.py SCENARIO

    v5       the modem takes QMAPv5
    qmap     Set Data Format succeeds, but Get reports QMAP (5) whatever was
             asked: oFono has to step down to QMAP
    refuse   QMAPv5 and QMAPv4 refused with an error, QMAP taken
    missing  Get without the endpoint info TLV -> MISSING_ARGUMENT (and Set
             without it too): oFono has to ask again with it
    nodpm    no DPM service registered
    silent   DPM never answers (oFono's 20 s timeout)
    nowda    WDA refuses every protocol

It ends after 120 s without a request, or on SIGTERM.
"""
import select
import socket
import struct
import sys
import time

QRTR_PORT_CTRL = 0xFFFFFFFE
QRTR_TYPE_NEW_SERVER = 4
SCENARIOS = ("v5", "qmap", "refuse", "missing", "nodpm", "silent", "nowda")

scenario = sys.argv[1] if len(sys.argv) > 1 else "v5"
if scenario not in SCENARIOS:
    sys.exit("unknown scenario %s (%s)" % (scenario, ", ".join(SCENARIOS)))

services = {2: "DMS", 11: "UIM", 1: "WDS", 3: "NAS", 26: "WDA", 47: "DPM"}
if scenario == "nodpm":
    del services[47]

socks = {}
for svc in services:
    s = socket.socket(socket.AF_QIPCRTR, socket.SOCK_DGRAM)
    node = s.getsockname()[0] or 1
    # version 1, instance 0; the kernel's name service takes the sender's
    # own node and port for a local server
    s.sendto(struct.pack("<5I", QRTR_TYPE_NEW_SERVER, svc, 1, 0, 0),
             (node, QRTR_PORT_CTRL))
    socks[s] = svc
print("registered", services, flush=True)

wda = {"llp": 1, "ul": 0, "dl": 0}


def tlvs(buf):
    out = {}
    i = 0
    while i + 3 <= len(buf):
        t, ln = struct.unpack_from("<BH", buf, i)
        out[t] = buf[i + 3:i + 3 + ln]
        i += 3 + ln
    return out


def reply(s, addr, txn, msg, ok=True, err=0, extra=b""):
    # QMI response: TLV 0x02 = result (u16) and error (u16)
    body = struct.pack("<BHHH", 2, 4, 0 if ok else 1, err) + extra
    s.sendto(struct.pack("<BHHH", 2, txn, msg, len(body)) + body, addr)


def u32(t, v):
    return struct.pack("<BHI", t, 4, v)


def u8(t, v):
    return struct.pack("<BHB", t, 1, v)


while True:
    r, _, _ = select.select(list(socks), [], [], 120)
    if not r:
        print("idle 120 s, end", flush=True)
        break
    for s in r:
        data, addr = s.recvfrom(4096)
        svc = socks[s]
        if len(data) < 7:
            continue
        typ, txn, msg, ln = struct.unpack_from("<BHHH", data)
        t = tlvs(data[7:7 + ln])
        print("%s %s msg 0x%04x txn %d tlvs %s" % (
            time.strftime("%T"), services[svc], msg, txn,
            {("0x%02x" % k): v.hex() for k, v in sorted(t.items())}),
            flush=True)
        if svc == 2 and msg == 0x002D:          # DMS Get Operating Mode
            reply(s, addr, txn, msg, extra=u8(0x01, 1))   # low power
        elif svc == 2 and msg == 0x002E:        # DMS Set Operating Mode
            reply(s, addr, txn, msg)
        elif svc == 47 and msg == 0x0020:       # DPM Open Port
            if scenario == "silent":
                print("  (no answer)", flush=True)
                continue
            reply(s, addr, txn, msg)
        elif svc == 26 and msg == 0x0021:       # WDA Get Data Format
            if scenario == "missing" and 0x10 not in t:
                reply(s, addr, txn, msg, ok=False, err=0x11)
                continue
            reply(s, addr, txn, msg, extra=u32(0x11, wda["llp"]) +
                  u32(0x12, wda["ul"]) + u32(0x13, wda["dl"]))
        elif svc == 26 and msg == 0x0020:       # WDA Set Data Format
            want = struct.unpack("<I", t[0x12])[0]
            if scenario == "nowda" or (scenario == "refuse" and want != 5):
                reply(s, addr, txn, msg, ok=False, err=0x30)
                continue
            if scenario == "missing" and 0x17 not in t:
                reply(s, addr, txn, msg, ok=False, err=0x11)
                continue
            wda["llp"] = struct.unpack("<I", t[0x11])[0]
            wda["ul"] = wda["dl"] = want if scenario != "qmap" else 5
            reply(s, addr, txn, msg)
