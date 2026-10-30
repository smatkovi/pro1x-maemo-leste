#!/usr/bin/env python3
"""A stand-in for pro1x-audiod's control socket (freebsd/audio/tools/
pro1x-audiod/main.c: one request line per connection, "ok ..." or
"error ..." back) for pro1x-callaudio's tests: it answers status, route
and voice as the daemon's formats say, and writes every request to LOG.

    fakeaudiod.py SOCKET LOG [--voice-fails] [--route spk]
"""
import argparse
import os
import socket

ap = argparse.ArgumentParser()
ap.add_argument('sock')
ap.add_argument('log')
ap.add_argument('--voice-fails', action='store_true')
ap.add_argument('--route', default='spk')
a = ap.parse_args()
route, voice = a.route, 'idle'
try:
    os.unlink(a.sock)
except FileNotFoundError:
    pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(a.sock)
s.listen(4)
log = open(a.log, 'a')
while True:
    c, _ = s.accept()
    req = c.recv(256).decode().strip()
    log.write(req + '\n')
    log.flush()
    w = req.split()
    if w == ['status']:
        ans = ('ok play=idle rec=idle route=%s rec_route=mic rx=down(0) va=down(0) '
               'tx=down(0) fm=0 voice=%s volume=80' % (route, voice))
    elif len(w) == 2 and w[0] == 'route' and w[1] in ('spk', 'hph', 'ear'):
        route = w[1]
        ans = 'ok route=%s' % route
    elif w == ['voice', 'start']:
        if a.voice_fails:
            ans = 'error EIO voice: step 2, status 0x1'
        else:
            voice = 'up'
            ans = 'ok voice=up mvm=0x1 cvp=0x2'
    elif w == ['voice', 'stop']:
        voice = 'idle'
        ans = 'ok voice=idle mvm=0 cvp=0'
    else:
        ans = 'error EINVAL %s' % req
    c.sendall((ans + '\n').encode())
    c.close()
