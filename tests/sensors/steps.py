#!/usr/bin/python3
# pro1x-steps against the fake sensor core (fake-ssc.py) on the PC: a
# system bus of its own with a fake mce (display and call state), the real
# libssc over the PC's QRTR. run.sh starts it when the qrtr module is there:
#
#   python3 -I steps.py BUILD PRO1X_STEPS LIBDIR
#
# BUILD: see_client_pb2.py and room for the bus, logs and sockets;
# PRO1X_STEPS: the binary; LIBDIR: the patched libssc's build (libssc.so).
# Checks what pro1x-steps asks of the sensor core (non-waking, 5 s batches
# with the display on, 10 minutes with it off), that a batch is one report,
# that GetSteps flushes, that the display coming on hands over what was
# held, GetSteps without a flush answer, a step counter that starts over,
# the disable at the end, and the accelerometer where there is no step
# counter.
import atexit
import json
import os
import signal
import socket
import subprocess
import sys
import time

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

build, steps_bin, libdir = sys.argv[1:4]
here = os.path.dirname(os.path.abspath(__file__))
fails = 0
procs = []  # everything started here, stopped at the end whatever happens


@atexit.register
def stop_all():
    for p in reversed(procs):
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()


def check(name, ok, detail=""):
    global fails
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else "  " + str(detail)), flush=True)
    if not ok:
        fails += 1


def pump(seconds):
    end = time.monotonic() + seconds
    ctx = GLib.MainContext.default()
    while time.monotonic() < end:
        if not ctx.iteration(False):
            time.sleep(0.01)


def wait_for(pred, timeout):
    end = time.monotonic() + timeout
    ctx = GLib.MainContext.default()
    while time.monotonic() < end:
        v = pred()
        if v:
            return v
        if not ctx.iteration(False):
            time.sleep(0.01)
    return pred()


# --- a system bus of its own --------------------------------------------
bus_dir = os.path.join(build, "bus")
os.makedirs(bus_dir, exist_ok=True)
conf = os.path.join(bus_dir, "bus.conf")
with open(conf, "w") as f:
    f.write("""<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>system</type>
  <listen>unix:path=%s/socket</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/>
    <allow own="*"/>
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
  </policy>
</busconfig>
""" % bus_dir)
daemon = subprocess.Popen(["dbus-daemon", "--config-file=" + conf, "--nofork", "--nopidfile"])
procs.append(daemon)
address = "unix:path=%s/socket" % bus_dir
for _ in range(100):
    if os.path.exists(bus_dir + "/socket"):
        break
    time.sleep(0.05)
bus = Gio.DBusConnection.new_for_address_sync(
    address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
    Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)

# --- a fake mce ------------------------------------------------------------
display = "on"
MCE_XML = """<node><interface name='com.nokia.mce.request'>
<method name='get_display_status'><arg type='s' direction='out'/></method>
<method name='get_call_state'><arg type='s' direction='out'/><arg type='s' direction='out'/></method>
</interface></node>"""


def mce_call(conn, sender, path, iface, method, params, inv):
    if method == "get_display_status":
        inv.return_value(GLib.Variant("(s)", (display,)))
    else:
        inv.return_value(GLib.Variant("(ss)", ("none", "normal")))


bus.register_object("/com/nokia/mce/request",
                    Gio.DBusNodeInfo.new_for_xml(MCE_XML).interfaces[0], mce_call, None, None)
bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
              "RequestName", GLib.Variant("(su)", ("com.nokia.mce", 4)), None,
              Gio.DBusCallFlags.NONE, -1, None)


def set_display(state):
    global display
    display = state
    bus.emit_signal(None, "/com/nokia/mce/signal", "com.nokia.mce.signal",
                    "display_status_ind", GLib.Variant("(s)", (state,)))


changed = []
bus.signal_subscribe(None, "org.pro1x.Steps", "StepsChanged", "/org/pro1x/Steps", None,
                     Gio.DBusSignalFlags.NONE,
                     lambda *a: changed.append((time.monotonic(), a[5].unpack()[0])))


def get_steps():
    t = time.monotonic()
    r = bus.call_sync("org.pro1x.Steps", "/org/pro1x/Steps", "org.pro1x.Steps", "GetSteps",
                      None, GLib.VariantType("(t)"), Gio.DBusCallFlags.NONE, 3000, None)
    return r.unpack()[0], time.monotonic() - t


# --- the fake sensor core and pro1x-steps ----------------------------------
ctl_path = os.path.join(build, "fake-ssc.ctl")
ctl = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)


def start(log_name, *args):
    log_path = os.path.join(build, log_name)
    if os.path.exists(log_path):
        os.unlink(log_path)
    fake = subprocess.Popen([sys.executable, "-I", os.path.join(here, "fake-ssc.py"),
                             build, ctl_path, log_path] + list(args))
    procs.append(fake)
    wait_for(lambda: os.path.exists(ctl_path) and os.path.exists(log_path), 5)
    env = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS=address, LD_LIBRARY_PATH=libdir,
               G_MESSAGES_DEBUG="all")
    out = open(os.path.join(build, log_name + ".steps"), "w")
    steps = subprocess.Popen([steps_bin], env=env, stdout=out, stderr=subprocess.STDOUT)
    procs.append(steps)
    return fake, steps, log_path


def entries(log_path):
    with open(log_path) as f:
        return [json.loads(line) for line in f if line.strip()]


def requests(log_path, sensor, msg_id=None):
    return [e for e in entries(log_path) if e.get("sensor") == sensor and
            (msg_id is None or e.get("msg_id") == msg_id)]


def reports(log_path, sensor):
    return [e for e in entries(log_path) if e.get("report") == sensor]


def cmd(text):
    ctl.sendto(text.encode(), ctl_path)


def stop(fake, steps):
    steps.send_signal(signal.SIGTERM)
    try:
        steps.wait(5)
    except subprocess.TimeoutExpired:
        steps.kill()
    pump(0.3)
    fake.terminate()
    fake.wait(5)


# --- with the step counter ---------------------------------------------------
fake, steps, log1 = start("fake-ssc.log")
en = wait_for(lambda: requests(log1, "pedometer", 514), 15)
check("enable request for the step counter (514)", bool(en), en)
e = en[0] if en else {}
check("non-waking (NO_WAKEUP)", e.get("delivery") == 1, e)
check("display on: batch period 5 s", e.get("batch_us") == 5000000, e)
wait_for(lambda: bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
                               "org.freedesktop.DBus", "NameHasOwner",
                               GLib.Variant("(s)", ("org.pro1x.Steps",)), None,
                               Gio.DBusCallFlags.NONE, -1, None).unpack()[0], 5)
pump(0.5)

cmd("walk 10")
pump(1.5)
before = [r for r in reports(log1, "pedometer") if 1028 in r["msg_ids"]]
check("steps held in the sensor core within the batch period", not before, before)
got = wait_for(lambda: [c for c in changed if c[1] == 10], 7)
batch = [r for r in reports(log1, "pedometer") if 1028 in r["msg_ids"]]
check("10 steps: one report with all 10 events",
      len(batch) == 1 and batch[0]["msg_ids"].count(1028) == 10, batch)
check("StepsChanged 10", bool(got), changed)

set_display("off")
off = wait_for(lambda: [r for r in requests(log1, "pedometer", 514) if r["batch_us"] == 600000000], 3)
check("display off: request again with 10 minutes", bool(off), requests(log1, "pedometer", 514))

cmd("walk 30")
pump(2)
held = [r for r in reports(log1, "pedometer") if r["msg_ids"].count(1028) == 30]
check("display off: the 30 steps stay in the sensor core", not held, held)
n, took = get_steps()
check("GetSteps flushes: 40 steps", n == 40, n)
check("GetSteps within 0.8 s", took < 0.8, took)
fl = requests(log1, "pedometer", 2)
check("a flush request (2), non-waking", len(fl) == 1 and fl[0]["delivery"] == 1, fl)
rep = [r for r in reports(log1, "pedometer") if 129 in r["msg_ids"]]
check("the flush: the held steps and the flush event in one report",
      len(rep) == 1 and rep[0]["msg_ids"].count(1028) == 30, rep)

cmd("walk 20")
pump(1.5)
mark = len(changed)
t0 = time.monotonic()
set_display("on")
on = wait_for(lambda: [c for c in changed[mark:] if c[1] == 60], 3)
check("display on: what was held comes at once (StepsChanged 60)", bool(on), changed[mark:])
if on:
    check("within a second", on[0][0] - t0 < 1.0, on[0][0] - t0)
back = [r for r in requests(log1, "pedometer", 514) if r["batch_us"] == 5000000]
check("display on: request again with 5 s", len(back) == 2, requests(log1, "pedometer", 514))

cmd("ignore-flush 1")
cmd("walk 5")
pump(0.2)
n, took = get_steps()
check("GetSteps without a flush answer: the count so far", n == 60, n)
check("GetSteps without a flush answer: after 0.8 s, not the client's 2 s",
      0.7 < took < 1.5, took)
got = wait_for(lambda: [c for c in changed if c[1] == 65], 8)
check("the batch comes anyway (StepsChanged 65)", bool(got), changed[-3:])
cmd("ignore-flush 0")

cmd("restart 3")
cmd("walk 2")
pump(6)
n, _ = get_steps()
check("a step counter that starts over: the steps so far kept (65 + 5)", n == 70, n)

stop(fake, steps)
check("stopped: the step counter disabled (10)", bool(requests(log1, "pedometer", 10)),
      requests(log1, "pedometer"))

# --- without a step counter: the accelerometer ------------------------------
changed.clear()
fake, steps, log2 = start("fake-ssc-noped.log", "--no-pedometer")
en = wait_for(lambda: requests(log2, "accel", 513), 15)
e = en[0] if en else {}
check("no step counter: the accelerometer instead (513)", bool(en), entries(log2)[-5:])
check("accelerometer: non-waking, 5 s batches",
      e.get("delivery") == 1 and e.get("batch_us") == 5000000, e)
pump(0.5)
cmd("accel-steps 12")
got = wait_for(lambda: changed and changed[-1][1] >= 10, 8)
n, _ = get_steps()
check("12 steps from the accelerometer counted (10-14)", 10 <= n <= 14, n)
stop(fake, steps)

bus.close_sync(None)
daemon.terminate()
daemon.wait(5)
print("%d failed" % fails if fails else "all passed")
sys.exit(1 if fails else 0)
