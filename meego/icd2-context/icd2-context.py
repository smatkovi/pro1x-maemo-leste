#!/usr/bin/python3
"""icd2-context for Harmattan on Leste: Internet.* from Leste's icd2.

Harmattan's status bar shows the WLAN icon (sysuid's PhoneNetworkType-
StatusIndicator) when contextkit's Internet.NetworkType is "WLAN",
Internet.NetworkState is not "disconnected" and System.WlanEnabled (mce's
radio states, mce/radiostates.c) is set. On the N9 Harmattan's own icd2
offered Internet.* as com.nokia.icd2.context on the system bus (the
provider file /usr/share/contextkit/providers/com.nokia.icd2.context in the
Harmattan tree); Leste's icd2 speaks the same com.nokia.icd2 API but
has no context provider, so nothing in the status bar showed a connection.

This provider follows Leste's icd2 (state_req when icd2 is there, then its
state_sig for every change) and offers the provider file's keys with
contextkit's D-Bus protocol, as libcontextprovider does:
org.maemo.contextkit.Property at /org/maemo/contextkit/Internet/<Key>,
Subscribe() and Get() -> (av value, t timestamp), Unsubscribe(),
ValueChanged(av, t); no value is an empty array, the timestamp is
CLOCK_MONOTONIC in nanoseconds.

  Internet.NetworkState  connected, connecting or disconnected
  Internet.NetworkType   WLAN (icd2's WLAN_INFRA, WLAN_ADHOC), GPRS, ...
  Internet.NetworkName   the WLAN's SSID (iw), else icd2's network id
  Internet.NetworkID     icd2's network id (the IAP)

SignalStrength, ServiceName, TrafficIn and TrafficOut have no value: the
status bar does not use them, and the status menu asks icd2 itself.

Runs as root on Leste, started by the MeeGo session
(40-harmattan-session.sh); the system bus's policy for the name is
pro1x-meego-icd2-context.conf.
"""
import subprocess
import sys
import time

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

NAME = 'com.nokia.icd2.context'
PROPERTY_IF = 'org.maemo.contextkit.Property'
ICD = 'com.nokia.icd2'
ICD_PATH = '/com/nokia/icd2'
KEYS = ('NetworkName', 'NetworkState', 'NetworkType', 'NetworkID',
        'SignalStrength', 'ServiceName', 'TrafficOut', 'TrafficIn')
WLAN_IF = 'wlan0'

# enum icd_connection_state (icd_dbus_api.h)
DISCONNECTED, CONNECTING, CONNECTED, DISCONNECTING = range(4)
ADDRESS_ACQUIRED = 15


def log(msg):
    print('icd2-context: ' + msg, file=sys.stderr, flush=True)


class Property(dbus.service.Object):
    def __init__(self, bus, key):
        super().__init__(bus, '/org/maemo/contextkit/Internet/' + key)
        self.value = None
        self.stamp = time.monotonic_ns()

    def values(self):
        return dbus.Array([] if self.value is None else [self.value], signature='v')

    @dbus.service.method(PROPERTY_IF, out_signature='avt')
    def Subscribe(self):
        return self.values(), dbus.UInt64(self.stamp)

    @dbus.service.method(PROPERTY_IF)
    def Unsubscribe(self):
        pass

    @dbus.service.method(PROPERTY_IF, out_signature='avt')
    def Get(self):
        return self.values(), dbus.UInt64(self.stamp)

    @dbus.service.signal(PROPERTY_IF, signature='avt')
    def ValueChanged(self, values, stamp):
        pass

    def set(self, value):
        if value == self.value:
            return
        self.value = value
        self.stamp = time.monotonic_ns()
        self.ValueChanged(self.values(), dbus.UInt64(self.stamp))


def ssid():
    for iw in ('/usr/sbin/iw', '/sbin/iw', '/usr/bin/iw'):
        try:
            out = subprocess.run([iw, 'dev', WLAN_IF, 'link'], capture_output=True,
                                 text=True, timeout=5).stdout
        except (OSError, subprocess.SubprocessError):
            continue
        for line in out.splitlines():
            if line.strip().startswith('SSID: '):
                return line.strip()[6:]
        return None
    return None


class Bridge:
    def __init__(self, bus):
        self.bus = bus
        self.props = {k: Property(bus, k) for k in KEYS}
        # (network type, attributes, id) -> 'connecting' or 'connected'
        self.conns = {}
        bus.add_signal_receiver(self.on_state, 'state_sig', ICD, ICD, ICD_PATH)
        bus.watch_name_owner(ICD, self.on_icd)

    def on_icd(self, owner):
        self.conns.clear()
        if not owner:
            self.publish()
            return
        self.bus.call_async(ICD, ICD_PATH, ICD, 'state_req', '', (),
                            self.on_state_reply, self.on_error)

    def on_state_reply(self, count):
        # with connections their state_sig follow
        if not count:
            self.publish()

    def on_error(self, err):
        log('state_req: %s' % err)
        self.publish()

    def on_state(self, *args):
        if len(args) < 8:
            return          # network search start/stop: (type, state)
        # the network id (an IAP's gconf name) comes with its NUL
        key = (str(args[3]), int(args[4]), bytes(args[5]).rstrip(b'\0'))
        state = int(args[7])
        if state == CONNECTED:
            self.conns[key] = 'connected'
        elif state in (DISCONNECTED, DISCONNECTING):
            self.conns.pop(key, None)
        elif state in (CONNECTING, ADDRESS_ACQUIRED):
            if self.conns.get(key) != 'connected':
                self.conns[key] = 'connecting'
        else:
            return          # limited connectivity: no change
        self.publish()

    def publish(self):
        best = None
        for (ntype, _attrs, nid), state in self.conns.items():
            rank = (state == 'connected', ntype.startswith('WLAN'))
            if best is None or rank > best[0]:
                best = (rank, ntype, nid, state)
        if best is None:
            values = {'NetworkState': 'disconnected', 'NetworkType': None,
                      'NetworkName': None, 'NetworkID': None}
        else:
            _rank, ntype, nid, state = best
            wlan = ntype.startswith('WLAN')
            nid = nid.decode(errors='replace') or None
            name = ssid() if wlan and state == 'connected' else None
            values = {'NetworkState': state, 'NetworkType': 'WLAN' if wlan else ntype,
                      'NetworkName': name or nid, 'NetworkID': nid}
        for key, value in values.items():
            self.props[key].set(value)


def main():
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    bridge = Bridge(bus)
    name = dbus.service.BusName(NAME, bus, do_not_queue=True)
    GLib.MainLoop().run()
    del bridge, name


if __name__ == '__main__':
    main()
