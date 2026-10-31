# tests/power-code/iphb-test.py SECONDS IPHBD_PID (with meego/iphb/iphbd running)
# Three libiphb-style clients against iphbd for DURATION s: A wakes every
# 10 s exactly (a clock), B wants 3..30 s, C 0..20 s; each asks again when
# woken. Prints the wakeups each got and the daemon's context switches.
import socket, struct, sys, time, select, os
dur = float(sys.argv[1]); pid = int(sys.argv[2])
def req(s, mn, mx):
    s.send(struct.pack('<iHHi', 0, mn, mx, os.getpid()) + b'\0' * 56)
def cs():
    t = 0
    for d in os.listdir('/proc/%d/task' % pid):
        for l in open('/proc/%d/task/%s/status' % (pid, d)):
            if l.startswith('voluntary_ctxt_switches'): t += int(l.split()[1])
    return t
spec = {'A': (10, 10), 'B': (3, 30), 'C': (0, 20)}
socks = {}
for k in spec:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect('/dev/shm/iphb'); socks[k] = s
    req(s, *spec[k])
n = {k: 0 for k in spec}; times = []
c0 = cs(); t0 = time.time()
while time.time() - t0 < dur:
    r, _, _ = select.select(list(socks.values()), [], [], 1)
    for k, s in socks.items():
        if s in r:
            s.recv(4); n[k] += 1; times.append(round(time.time() - t0)); req(s, *spec[k])
print('wakeups per client', n, 'distinct seconds with wakeups', len(set(times)), 'daemon context switches', cs() - c0)
