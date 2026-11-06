#!/usr/bin/env python3.11
# fake-wpa-supplicant.py: wpa_supplicant's control interface for wlan0
# (/var/run/wpa_supplicant/wlan0) without a wlan0, for trying
# pro1x-wifi-status in the test VM (try-wifi-status.sh): answers the
# commands it sends (STATUS, SCAN, SCAN_RESULTS, LIST_NETWORKS,
# ADD/SET/SELECT/ENABLE/REMOVE_NETWORK, SAVE_CONFIG, DISCONNECT) from a
# made-up scan, sends the events a real one sends to ATTACHed sockets
# (SCAN-RESULTS, CONNECTED, SSID-TEMP-DISABLED ... reason=WRONG_KEY for
# the password "wrong-pass", DISCONNECTED) and logs every command to
# /tmp/fakewpa.log (a psk not).
import os, socket, threading, time

DIR = "/var/run/wpa_supplicant"
PATH = DIR + "/wlan0"
LOG = open("/tmp/fakewpa.log", "a", buffering=1)
os.makedirs(DIR, exist_ok=True)
try:
    os.unlink(PATH)
except FileNotFoundError:
    pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(PATH)
os.chmod(PATH, 0o770)

attached = set()
nets = {0: {"ssid": '"Example Home"', "flags": "[CURRENT]"}}
state = {"cur": 0, "pending": None}
bss = [
    ("aa:bb:cc:00:00:01", 2437, -45, "[WPA2-PSK-CCMP][ESS]", "Example Home"),
    ("aa:bb:cc:00:00:02", 5180, -67, "[WPA2-PSK-CCMP][ESS]", "Example 5G"),
    ("aa:bb:cc:00:00:03", 2412, -80, "[ESS]", "Example Open"),
    ("aa:bb:cc:00:00:04", 2462, -72, "[WPA2-SAE-CCMP][ESS]", "Example WPA3"),
    ("aa:bb:cc:00:00:05", 2462, -60, "[WPA2-EAP-CCMP][ESS]", "Example EAP"),
]


def event(text):
    for a in list(attached):
        try:
            s.sendto(text.encode(), a)
        except OSError:
            attached.discard(a)
    LOG.write("event %s\n" % text)


def later(sec, fn):
    threading.Timer(sec, fn).start()


def bss_of(i):
    ssid = nets[i]["ssid"].strip('"')
    return next((b for b in bss if b[4] == ssid), bss[0])


def connect(i):
    state["cur"] = i
    for k, n in nets.items():
        n["flags"] = "[CURRENT]" if k == i else ""
    event("<3>CTRL-EVENT-CONNECTED - Connection to %s completed [id=%d id_str=]" % (bss_of(i)[0], i))


def status():
    i = state["cur"]
    if i is None:
        return "wpa_state=DISCONNECTED\naddress=02:00:00:00:00:01\n"
    b = bss_of(i)
    return ("bssid=%s\nfreq=%d\nssid=%s\nid=%d\nmode=station\n"
            "pairwise_cipher=CCMP\nkey_mgmt=WPA2-PSK\nwpa_state=COMPLETED\n"
            "ip_address=10.0.2.99\naddress=02:00:00:00:00:01\n" % (b[0], b[1], b[4], i))


def handle(cmd):
    w = cmd.split(" ")
    c = w[0]
    if c == "PING":
        return "PONG\n"
    if c == "ATTACH":
        return "OK\n"
    if c == "STATUS":
        return status()
    if c == "SCAN":
        later(1.5, lambda: event("<2>CTRL-EVENT-SCAN-RESULTS "))
        return "OK\n"
    if c == "SCAN_RESULTS":
        return "bssid / frequency / signal level / flags / ssid\n" + "".join(
            "%s\t%d\t%d\t%s\t%s\n" % b for b in bss)
    if c == "LIST_NETWORKS":
        return "network id / ssid / bssid / flags\n" + "".join(
            "%d\t%s\tany\t%s\n" % (k, n["ssid"].strip('"'), n["flags"]) for k, n in sorted(nets.items()))
    if c == "ADD_NETWORK":
        i = max(nets) + 1 if nets else 0
        nets[i] = {"ssid": "", "flags": "[DISABLED]"}
        return "%d\n" % i
    if c == "SET_NETWORK":
        i, k, v = int(w[1]), w[2], " ".join(w[3:])
        if i not in nets:
            return "FAIL\n"
        if k == "ssid":
            v = '"%s"' % bytes.fromhex(v).decode() if not v.startswith('"') else v
            nets[i]["ssid"] = v
        if k == "psk":
            nets[i]["psk"] = v
        return "OK\n"
    if c == "SELECT_NETWORK":
        i = int(w[1])
        if i not in nets:
            return "FAIL\n"
        n = nets[i]
        if n.get("psk") == '"wrong-pass"':
            later(1.5, lambda: event('<3>CTRL-EVENT-SSID-TEMP-DISABLED id=%d ssid=%s auth_failures=1 duration=10 reason=WRONG_KEY' % (i, n["ssid"])))
        else:
            later(1.5, lambda: connect(i))
        state["cur"] = None
        return "OK\n"
    if c in ("ENABLE_NETWORK", "SAVE_CONFIG", "REASSOCIATE", "RECONNECT"):
        return "OK\n"
    if c == "DISCONNECT":
        state["cur"] = None
        later(0.5, lambda: event("<3>CTRL-EVENT-DISCONNECTED bssid=aa:bb:cc:00:00:01 reason=3 locally_generated=1"))
        return "OK\n"
    if c == "REMOVE_NETWORK":
        nets.pop(int(w[1]), None)
        return "OK\n"
    return "UNKNOWN COMMAND\n"


while True:
    data, addr = s.recvfrom(4096)
    cmd = data.decode(errors="replace").strip()
    LOG.write("cmd %s\n" % (cmd if not cmd.startswith("SET_NETWORK") or " psk " not in cmd else cmd.split(" psk ")[0] + " psk <...>"))
    if cmd == "ATTACH":
        attached.add(addr)
    elif cmd == "DETACH":
        attached.discard(addr)
    s.sendto(handle(cmd).encode(), addr)
