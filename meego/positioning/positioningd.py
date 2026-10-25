#!/usr/bin/python3
"""positioningd for Harmattan on Leste: GPS from gpsd for QtLocation.

Harmattan's QtMobility Location (QGeoPositionInfoSource, QGeoSatelliteInfo-
Source) talks to Nokia's positioningd on the session bus, which drove the
N9's GPS chip. This replacement offers the same interface (com.nokia.
positioningd.client, as used by qt-mobility 1.2's dbuscomm_maemo.cpp) and
takes the positions from Leste's gpsd, which pro1x-gnss feeds from the
modem's GNSS engine:

  registerListener(s service, s path) -> (i client, u methods, u interval)
  configSession(i client, i command, i methods, i interval) -> i
  latestPosition(b satellite_only) -> ay
  to each client (com.nokia.qlocation.updates at its own name and path):
    positionUpdate(ay), satellitesInViewUpdate(ay), satellitesInUseUpdate(ay)

The byte arrays are QDataStream serialisations (Qt 4.7): QGeoPositionInfo,
QList<QGeoSatelliteInfo>.

gpsd, and with it the GNSS session in the modem, runs only while a client
has a session started, and stops a minute after the last one ends. Leste's
gpsd service only starts once Leste's location privacy disclaimer has been
accepted (gconf /system/nokia/location/disclaimer-accepted, asked by the
first liblocation client in Leste); until then there are no positions.

Runs as root on Leste (it starts and stops gpsd), started by the MeeGo
session (40-harmattan-session.sh) with the Harmattan tree as argument; it
joins the session bus of Harmattan's user with that user's id.
"""
import calendar
import json
import math
import os
import socket
import struct
import subprocess
import sys
import time

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

NAME = 'com.nokia.positioningd.client'
PATH = '/com/nokia/positioningd/client'
UPDATES = 'com.nokia.qlocation.updates'
SATELLITE_METHODS = 0x000000ff
CMD_START, CMD_STOP, CMD_METHODS, CMD_INTERVAL, CMD_SAT_START, CMD_SAT_STOP = 1, 2, 4, 8, 16, 32
MIN_INTERVAL_MS = 1000
LINGER_S = 60
HUID = 1000

# QGeoPositionInfo::Attribute, QGeoSatelliteInfo::Attribute
DIRECTION, GROUND_SPEED, VERTICAL_SPEED, MAGNETIC_VARIATION, H_ACCURACY, V_ACCURACY = range(6)
ELEVATION, AZIMUTH = range(2)
JULIAN_DAY_1970 = 2440588


def qdatetime(t):
    """QDateTime in UTC: QDate (julian day), QTime (ms), time spec."""
    days, secs = divmod(t, 86400)
    return struct.pack('>IIb', JULIAN_DAY_1970 + int(days), int(secs * 1000), 1)


def qhash(attrs):
    out = struct.pack('>I', len(attrs))
    for key, value in attrs.items():
        out += struct.pack('>id', key, value)
    return out


def position_info(fix):
    """QGeoPositionInfo: timestamp, coordinate (lat, lon, alt), attributes."""
    if fix is None:
        nan = float('nan')
        return struct.pack('>IIb', 0, 0xffffffff, 0) + struct.pack('>ddd', nan, nan, nan) + qhash({})
    attrs = {}
    for key, name in ((DIRECTION, 'track'), (GROUND_SPEED, 'speed'), (VERTICAL_SPEED, 'climb'),
                      (MAGNETIC_VARIATION, 'magvar'), (V_ACCURACY, 'epv')):
        if name in fix:
            attrs[key] = float(fix[name])
    if 'eph' in fix:
        attrs[H_ACCURACY] = float(fix['eph'])
    elif 'epx' in fix and 'epy' in fix:
        attrs[H_ACCURACY] = math.hypot(fix['epx'], fix['epy'])
    alt = fix.get('altMSL', fix.get('alt', float('nan'))) if fix.get('mode', 0) >= 3 else float('nan')
    return (qdatetime(fix['t']) + struct.pack('>ddd', fix['lat'], fix['lon'], alt) + qhash(attrs))


def satellite_list(sats):
    out = struct.pack('>I', len(sats))
    for s in sats:
        out += struct.pack('>ii', int(s.get('PRN', 0)), int(round(s.get('ss', -1))))
        attrs = {}
        if 'el' in s:
            attrs[ELEVATION] = float(s['el'])
        if 'az' in s:
            attrs[AZIMUTH] = float(s['az'])
        out += qhash(attrs)
    return out


class Client:
    def __init__(self, service, path):
        self.service, self.path = service, path
        self.positions = self.satellites = self.one_shot = False
        self.interval = MIN_INTERVAL_MS
        self.last_sent = 0.0


class Positioningd(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, PATH)
        self.bus = bus
        self.clients = {}
        self.next_id = 1
        self.fix = None
        self.gpsd = None
        self.gpsd_running = False   # wanted: a client has a session
        self.started_gpsd = False   # the gpsd service was started here
        self.connect_tries = 0
        self.buffer = b''
        self.stop_id = 0

    # --- the interface --------------------------------------------------------------
    @dbus.service.method(NAME, in_signature='ss', out_signature='iuu')
    def registerListener(self, service, path):
        cid = self.next_id
        self.next_id += 1
        self.clients[cid] = Client(str(service), str(path))
        return cid, SATELLITE_METHODS, MIN_INTERVAL_MS

    @dbus.service.method(NAME, in_signature='iiii', out_signature='i')
    def configSession(self, cid, command, methods, interval):
        c = self.clients.get(int(cid))
        if c is None:
            return -1
        if command & CMD_INTERVAL:
            c.interval = max(int(interval), MIN_INTERVAL_MS)
        if command & CMD_SAT_STOP and command & CMD_SAT_START:     # 48: one shot
            c.satellites = True
        elif command & CMD_SAT_START:
            c.satellites = True
        elif command & CMD_SAT_STOP:
            c.satellites = False
        if command & CMD_START and command & CMD_STOP:               # 3: one shot
            c.positions, c.one_shot = True, True
        elif command & CMD_START:
            c.positions, c.one_shot = True, False
        elif command & CMD_STOP:
            c.positions = False
        self.update_gpsd()
        return 0

    @dbus.service.method(NAME, in_signature='b', out_signature='ay', byte_arrays=True)
    def latestPosition(self, satellite_only):
        return dbus.ByteArray(position_info(self.fix))

    # --- clients -----------------------------------------------------------------------
    def send(self, c, method, data):
        try:
            obj = self.bus.get_object(c.service, c.path, introspect=False)
            obj.get_dbus_method(method, UPDATES)(dbus.ByteArray(data), ignore_reply=True)
        except dbus.DBusException:
            pass

    def forget_gone(self):
        names = set(self.bus.list_names())
        for cid in [k for k, c in self.clients.items() if c.service not in names]:
            del self.clients[cid]

    def active(self):
        return any(c.positions or c.satellites for c in self.clients.values())

    # --- gpsd --------------------------------------------------------------------------
    def update_gpsd(self):
        self.forget_gone()
        if self.active():
            if self.stop_id:
                GLib.source_remove(self.stop_id)
                self.stop_id = 0
            self.start_gpsd()
        elif self.gpsd_running and not self.stop_id:
            self.stop_id = GLib.timeout_add_seconds(LINGER_S, self.stop_gpsd)

    def start_gpsd(self):
        if not self.gpsd_running:
            self.gpsd_running = True
            r = subprocess.run(['rc-service', 'gpsd', 'start'], stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, text=True)
            self.started_gpsd = r.returncode == 0
            if not self.started_gpsd:
                print('positioningd: gpsd did not start:', r.stderr.strip(), file=sys.stderr)
            self.connect_tries = 0
            if self.gpsd is None:
                GLib.timeout_add(500, self.connect_gpsd)

    def stop_gpsd(self):
        self.stop_id = 0
        if self.active():
            return False
        if self.gpsd is not None:
            self.gpsd.close()
            self.gpsd = None
        if self.started_gpsd:
            subprocess.run(['rc-service', 'gpsd', 'stop'], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
            self.started_gpsd = False
        self.gpsd_running = False
        return False

    def connect_gpsd(self):
        if self.gpsd is not None or not self.gpsd_running:
            return False
        try:
            s = socket.create_connection(('127.0.0.1', 2947), timeout=2)
        except OSError:
            # gpsd still starting: try again for half a minute
            self.connect_tries += 1
            return self.connect_tries < 60
        s.setblocking(False)
        s.send(b'?WATCH={"enable":true,"json":true};\n')
        self.gpsd = s
        GLib.io_add_watch(s.fileno(), GLib.PRIORITY_DEFAULT, GLib.IO_IN | GLib.IO_HUP | GLib.IO_ERR,
                          self.gpsd_readable)
        return False

    def gpsd_readable(self, fd, cond):
        if self.gpsd is None:
            return False
        try:
            data = self.gpsd.recv(65536)
        except BlockingIOError:
            return True
        except OSError:
            data = b''
        if not data:
            self.gpsd.close()
            self.gpsd = None
            if self.gpsd_running:
                GLib.timeout_add_seconds(2, self.connect_gpsd)
            return False
        self.buffer += data
        while b'\n' in self.buffer:
            line, self.buffer = self.buffer.split(b'\n', 1)
            try:
                msg = json.loads(line)
            except ValueError:
                continue
            if msg.get('class') == 'TPV':
                self.on_tpv(msg)
            elif msg.get('class') == 'SKY':
                self.on_sky(msg)
        return True

    def on_tpv(self, msg):
        if msg.get('mode', 0) < 2 or 'lat' not in msg or 'lon' not in msg:
            return
        fix = dict(msg)
        try:
            fix['t'] = calendar.timegm(time.strptime(msg['time'][:19], '%Y-%m-%dT%H:%M:%S'))
        except (KeyError, ValueError):
            fix['t'] = time.time()
        self.fix = fix
        now = time.monotonic()
        data = position_info(fix)
        for c in list(self.clients.values()):
            if c.positions and (now - c.last_sent) * 1000 >= c.interval - 100:
                c.last_sent = now
                self.send(c, 'positionUpdate', data)
                if c.one_shot:
                    c.positions = c.one_shot = False
        self.update_gpsd()

    def on_sky(self, msg):
        sats = msg.get('satellites') or []
        in_view = satellite_list(sats)
        in_use = satellite_list([s for s in sats if s.get('used')])
        for c in self.clients.values():
            if c.satellites:
                self.send(c, 'satellitesInViewUpdate', in_view)
                self.send(c, 'satellitesInUseUpdate', in_use)


def main():
    harmattan = sys.argv[1] if len(sys.argv) > 1 else '/mnt/harmattan'
    with open(os.path.join(harmattan, 'var/run/pro1x-session-bus')) as f:
        address = f.read().strip()
    DBusGMainLoop(set_as_default=True)
    # The session bus admits its own user only: connect as that user,
    # keep root for starting gpsd.
    os.setegid(HUID)
    os.seteuid(HUID)
    try:
        bus = dbus.bus.BusConnection(address)
    finally:
        os.seteuid(0)
        os.setegid(0)
    name = dbus.service.BusName(NAME, bus, do_not_queue=True)
    daemon = Positioningd(bus)
    loop = GLib.MainLoop()
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, 15, loop.quit)
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, 2, loop.quit)
    loop.run()
    # do not leave the GNSS session running
    if daemon.gpsd_running:
        daemon.clients.clear()
        daemon.stop_gpsd()


if __name__ == '__main__':
    main()
