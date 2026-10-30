#!/usr/bin/env python3
"""pro1x-modemd's mobile data as oFono's clients see it (freebsd/data/test).

data_run.sh starts a private bus, fakedata.py (the simulated modem with
WDS, WDA and DPM) and pro1x-modemd with a data.conf and a hook that logs;
this checks, over D-Bus and in the modem's request log:

    test_data.py CTL-SOCKET REQUEST-LOG HOOK-LOG SCENARIO

SCENARIO: main (FAKEDATA_WDA=qmapv4: QMAPv5 refused first, the DPM port
with the IPA's endpoints, a dual-stack session on mux 1, its settings, the
network ending it, a wrong APN, Stop Network on request), endpoint
(FAKEDATA_WDA_EP=1: the endpoint info after MISSING_ARGUMENT).
"""
import socket
import sys
import time

import dbus
import dbus.mainloop.glib
from gi.repository import GLib

CTL, REQLOG, HOOKLOG, SCENARIO = sys.argv[1:5]
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SystemBus()
ctx = GLib.MainContext.default()
failed = passed = 0
M = '/qrtr_0'
C1 = M + '/context1'


def check(cond, what):
    global failed, passed
    if cond:
        passed += 1
        print('ok   ' + what)
    else:
        failed += 1
        print('FAIL ' + what)


def pump(sec):
    end = time.monotonic() + sec
    while time.monotonic() < end:
        ctx.iteration(False)
        time.sleep(0.01)


def wait_for(cond, sec=15):
    end = time.monotonic() + sec
    while time.monotonic() < end:
        ctx.iteration(False)
        try:
            if cond():
                return True
        except dbus.exceptions.DBusException:
            pass
        time.sleep(0.05)
    return False


def ctl(cmd):
    s = socket.socket(socket.AF_UNIX)
    s.connect(CTL)
    s.sendall((cmd + '\n').encode())
    r = s.makefile().read()
    s.close()
    return r.strip()


def requests(prefix):
    return [l.rstrip('\n') for l in open(REQLOG) if l.startswith(prefix)]


def iface(path, name):
    return dbus.Interface(bus.get_object('org.ofono', path), name)


def props(path, name):
    return iface(path, name).GetProperties()


def call_error(fn):
    try:
        fn()
        return None
    except dbus.exceptions.DBusException as e:
        return e.get_dbus_name() + ': ' + e.get_dbus_message()


signals = []
bus.add_signal_receiver(lambda name, value, path=None: signals.append((path, str(name), value)),
                        signal_name='PropertyChanged',
                        dbus_interface='org.ofono.ConnectionContext', path_keyword='path')

check(wait_for(lambda: 'org.ofono.ConnectionManager' in
               props(M, 'org.ofono.Modem')['Interfaces'], 30),
      'Modem.Interfaces has org.ofono.ConnectionManager once the data port is set up')
opens = requests('DPM Open-Port')
check(len(opens) == 1 and 'type=4 iface=1 rx=1 tx=9' in opens[0],
      'DPM Open Port: EMBEDDED, interface 1, modem RX = IPA TX 1, modem TX = IPA RX 9')
sets = requests('WDA Set-Data-Format')
if SCENARIO == 'main':
    check(len(sets) == 2 and 'ul=9 dl=9' in sets[0] and 'ul=8 dl=8' in sets[1] and
          all('link=2' in s and 'datagrams=32 size=32768' in s for s in sets),
          'WDA: raw IP, QMAPv5 refused, then QMAPv4; 32 datagrams, 32768 bytes')
else:
    gets = requests('WDA Get-Data-Format')
    check(gets[:2] and 'endpoint=no' in gets[0] and 'endpoint=yes' in gets[1],
          'WDA Get Data Format with the endpoint info after MISSING_ARGUMENT')
    check(sets and all('endpoint=4/1' in s for s in sets),
          'WDA Set Data Format with the endpoint info (EMBEDDED, 1)')

cm = props(M, 'org.ofono.ConnectionManager')
check(cm['Attached'] and cm['Bearer'] == 'lte' and cm['Powered'],
      'ConnectionManager: Attached, Bearer lte, Powered')
ctxs = iface(M, 'org.ofono.ConnectionManager').GetContexts()
check(len(ctxs) == 1 and ctxs[0][0] == C1, 'one context, context1')
p = ctxs[0][1]
check(p['AccessPointName'] == 'internet.test' and p['Username'] == 'user1' and
      p['AuthenticationMethod'] == 'chap' and p['Protocol'] == 'dual' and
      p['Type'] == 'internet' and not p['Active'],
      'context1 from data.conf: APN, user, CHAP, dual, inactive')

if SCENARIO == 'main':
    cc = iface(C1, 'org.ofono.ConnectionContext')
    cc.SetProperty('Active', dbus.Boolean(True), timeout=60)
    p = props(C1, 'org.ofono.ConnectionContext')
    s4, s6 = p['Settings'], p['IPv6.Settings']
    check(p['Active'], 'Active after SetProperty Active true')
    check(s4.get('Interface') == 'qmap0' and s4.get('Method') == 'static' and
          s4.get('Address') == '10.64.33.7' and s4.get('Netmask') == '255.255.255.0' and
          s4.get('Gateway') == '10.64.33.1' and
          list(s4.get('DomainNameServers', [])) == ['10.74.0.1', '10.74.0.2'],
          'Settings: qmap0, static, address, netmask, gateway, DNS')
    check(s6.get('Address') == '2a02:8388:1:2::42' and s6.get('PrefixLength') == 64 and
          s6.get('Gateway') == '2a02:8388:1:2::1' and
          list(s6.get('DomainNameServers', [])) == ['2a02:8388::53', '2a02:8388::54'],
          'IPv6.Settings: address /64, gateway, DNS')
    binds = requests('WDS Bind-Mux-Data-Port')
    check(len(binds) == 2 and all('type=4 iface=1 mux=1 client=1' in b for b in binds),
          'two WDS clients bound to mux 1 (EMBEDDED, 1, tethered)')
    starts = requests('WDS Start-Network')
    check(len(starts) == 2 and 'family=4' in starts[0] and 'family=6' in starts[1] and
          all('apn=internet.test user=user1 password=set auth=2' in s for s in starts),
          'Start Network IPv4 then IPv6: APN, user, password, CHAP')
    pump(0.5)
    check(any(n == 'Active' and v for (_, n, v) in signals), 'PropertyChanged Active true')
    hook = open(HOOKLOG).read()
    check('up qmap0' in hook and 'IPV4_ADDRESS=10.64.33.7' in hook and
          'IPV4_GATEWAY=10.64.33.1' in hook and 'IPV6_PREFIXLEN=64' in hook and
          'MTU=1430' in hook, 'the hook: up qmap0 with the addresses and the MTU')
    # the network ends both sessions
    signals.clear()
    ctl('data-drop')
    check(wait_for(lambda: not props(C1, 'org.ofono.ConnectionContext')['Active']),
          'the network ended it: Active false')
    check('down qmap0' in open(HOOKLOG).read(), 'the hook: down qmap0')
    check(not props(C1, 'org.ofono.ConnectionContext')['Settings'], 'Settings empty')
    # a wrong APN
    cc.SetProperty('AccessPointName', 'wrong.apn')
    err = call_error(lambda: cc.SetProperty('Active', dbus.Boolean(True), timeout=60))
    check(err is not None and err.startswith('org.ofono.Error.Failed') and
          'call end' in err, 'a wrong APN: org.ofono.Error.Failed with the reason (%s)' % err)
    check(not props(C1, 'org.ofono.ConnectionContext')['Active'], 'not active')
    # and again, then off on request
    cc.SetProperty('AccessPointName', 'internet.test')
    cc.SetProperty('Protocol', 'ip')
    cc.SetProperty('Active', dbus.Boolean(True), timeout=60)
    check(props(C1, 'org.ofono.ConnectionContext')['Active'] and
          not props(C1, 'org.ofono.ConnectionContext')['IPv6.Settings'],
          'IPv4 only: active, no IPv6 settings')
    err = call_error(lambda: cc.SetProperty('AccessPointName', 'x'))
    check(err is not None and 'InProgress' in err, 'no APN change while active')
    cc.SetProperty('Active', dbus.Boolean(False))
    check(wait_for(lambda: len(requests('WDS Stop-Network')) == 1) and
          ctl('sessions') == 'ok 0', 'Stop Network for the session on Active false')
    # contexts come and go
    cmi = iface(M, 'org.ofono.ConnectionManager')
    p2 = cmi.AddContext('mms')
    check(p2 == M + '/context2' and len(cmi.GetContexts()) == 2, 'AddContext mms')
    cmi.RemoveContext(p2)
    check(len(cmi.GetContexts()) == 1, 'RemoveContext')

print('test_data.py %s: %d passed, %d failed' % (SCENARIO, passed, failed))
sys.exit(1 if failed else 0)
