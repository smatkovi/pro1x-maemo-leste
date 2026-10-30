#!/usr/bin/env python3
"""pro1x-modemd's D-Bus side as oFono's clients see it (freebsd/modem/test).

run.sh starts a private bus (DBUS_SYSTEM_BUS_ADDRESS), the simulated modem
(fakemodem.py) and pro1x-modemd on it; this checks what sphone's comm-ofono
and oFono's other clients rely on, in the order a phone goes through it:

    test_ofono.py CTL-SOCKET REQUEST-LOG AUDIO-LOG SCENARIO

SCENARIO: main (the PIN from the file, then SMS and calls), wrongpin (one
attempt with a wrong PIN file, none after a restart), lowretries (no
attempt), nofile (the PIN over D-Bus).
"""
import os
import socket
import sys
import time

import dbus
import dbus.mainloop.glib
from gi.repository import GLib

CTL, REQLOG, AUDIOLOG, SCENARIO = sys.argv[1:5]
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SystemBus()
ctx = GLib.MainContext.default()
failed = passed = 0
signals = []


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


def wait_for(cond, sec=10):
    end = time.monotonic() + sec
    while time.monotonic() < end:
        ctx.iteration(False)
        v = cond()
        if v:
            return v
        time.sleep(0.01)
    return cond()


def ctl(cmd):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(CTL)
    s.sendall((cmd + '\n').encode())
    out = b''
    while True:
        b = s.recv(65536)
        if not b:
            break
        out += b
    s.close()
    return out.decode()


def requests():
    return open(REQLOG).read().splitlines()


def on_signal(*args, **kw):
    signals.append((kw['interface'], kw['member'], kw['path'], args))


bus.add_signal_receiver(on_signal, bus_name='org.ofono', interface_keyword='interface',
                        member_keyword='member', path_keyword='path')


def sigs(iface, member, path=None):
    return [s for s in signals if s[0] == iface and s[1] == member and
            (path is None or s[2] == path)]


def iface(path, name):
    return dbus.Interface(bus.get_object('org.ofono', path), name)


def props(path, name):
    return iface(path, name).GetProperties()


def dbus_error(fn, *a):
    try:
        fn(*a)
    except dbus.DBusException as e:
        return e.get_dbus_name()
    return None


# --- the modem appears -------------------------------------------------------
check(wait_for(lambda: bus.name_has_owner('org.ofono'), 15), 'org.ofono on the bus')
mgr = iface('/', 'org.ofono.Manager')
modems = mgr.GetModems()
check(len(modems) == 1 and modems[0][0] == '/qrtr_0', 'GetModems: one modem, /qrtr_0')
MP = '/qrtr_0'
m = wait_for(lambda: (lambda p: p if p.get('Manufacturer') and p.get('Revision') else None)(
    props(MP, 'org.ofono.Modem')))
check(m and m['Manufacturer'] == 'QUALCOMM INCORPORATED' and m['Model'] == 'Fake SM6115 modem' and
      m['Revision'] == 'MPSS.FAKE.1.0' and m['Powered'], 'Modem: identity, Powered')
check(wait_for(lambda: props(MP, 'org.ofono.Modem').get('Serial') == '350000000000001'),
      'Modem: Serial is the IMEI')
check(wait_for(lambda: props(MP, 'org.ofono.Modem')['Online']), 'Modem: Online at the start')


def sim():
    return props(MP, 'org.ofono.SimManager')


if SCENARIO == 'wrongpin':
    check(wait_for(lambda: [r for r in requests() if r.startswith('UIM Verify-PIN')], 10),
          'one attempt from the (wrong) PIN file')
    pump(3)
    s = sim()
    check(s['PinRequired'] == 'pin' and s['Retries']['pin'] == 2,
          'PIN still required, 2 tries left (%s %s)' % (s['PinRequired'], dict(s['Retries'])))
    n = len([r for r in requests() if r.startswith('UIM Verify-PIN')])
    check(n == 1, 'exactly one attempt (%d)' % n)
    print('passed %d failed %d' % (passed, failed))
    sys.exit(failed != 0)

if SCENARIO == 'again':
    pump(4)
    n = len([r for r in requests() if r.startswith('UIM Verify-PIN')])
    check(n == 0, 'restarted in the same boot: no attempt (%d)' % n)
    print('passed %d failed %d' % (passed, failed))
    sys.exit(failed != 0)

if SCENARIO == 'lowretries':
    pump(4)
    n = len([r for r in requests() if r.startswith('UIM Verify-PIN')])
    check(n == 0, 'fewer than 3 tries left: no attempt (%d)' % n)
    check(sim()['PinRequired'] == 'pin', 'PinRequired pin')
    print('passed %d failed %d' % (passed, failed))
    sys.exit(failed != 0)

if SCENARIO == 'nofile':
    check(wait_for(lambda: sim().get('PinRequired') == 'pin'), 'PinRequired pin without a PIN file')
    ifs = props(MP, 'org.ofono.Modem')['Interfaces']
    check('org.ofono.MessageManager' not in ifs, 'no MessageManager before the PIN')
    sm = iface(MP, 'org.ofono.SimManager')
    check(dbus_error(sm.EnterPin, 'pin', '12a4') == 'org.ofono.Error.InvalidFormat',
          'EnterPin: not digits refused')
    check(dbus_error(sm.EnterPin, 'pin', '0000') == 'org.ofono.Error.IncorrectPassword',
          'EnterPin: a wrong PIN is IncorrectPassword')
    check(wait_for(lambda: sim()['Retries'].get('pin') == 2), 'Retries pin 2 after it')
    check(dbus_error(sm.EnterPin, 'pin', '7395') is None, 'EnterPin: the right PIN')
    check(wait_for(lambda: sim().get('PinRequired') == 'none'), 'PinRequired none')
    check(wait_for(lambda: 'org.ofono.MessageManager' in props(MP, 'org.ofono.Modem')['Interfaces']),
          'MessageManager once the SIM is ready')
    print('passed %d failed %d' % (passed, failed))
    sys.exit(failed != 0)

# --- main: the PIN from the file ----------------------------------------------
check(wait_for(lambda: sim().get('PinRequired') == 'none', 10), 'SIM unlocked from the PIN file')
v = [r for r in requests() if r.startswith('UIM Verify-PIN')]
check(len(v) == 1 and 'pin_id=1' in v[0] and 'pin_len=4' in v[0], 'one Verify PIN, PIN1 (%s)' % v)
check(wait_for(lambda: sim().get('SubscriberIdentity') == '232030000000001'), 'SIM: IMSI')
check(sim().get('CardIdentifier') == '8943030000000000001', 'SIM: ICCID')
check(dict(sim()['Retries']) == {'pin': 3, 'puk': 10}, 'SIM: Retries %s' % dict(sim()['Retries']))

nr = lambda: props(MP, 'org.ofono.NetworkRegistration')
check(wait_for(lambda: nr().get('Status') == 'registered', 10), 'registered')
n = nr()
check(n.get('Name') == 'Fake Telecom' and n.get('MobileCountryCode') == '232' and
      n.get('MobileNetworkCode') == '03' and n.get('Technology') == 'lte',
      'operator, MCC/MNC, LTE (%s)' % dict(n))
check(n.get('LocationAreaCode') == 0x4321 and n.get('CellId') == 0x0abcdef,
      'TAC as LocationAreaCode, cell id')
check(wait_for(lambda: nr().get('Strength') == 61), 'Strength from -75 dBm: 61 %%')
ifs = props(MP, 'org.ofono.Modem')['Interfaces']
check(set(ifs) == {'org.ofono.SimManager', 'org.ofono.VoiceCallManager',
                   'org.ofono.MessageManager', 'org.ofono.NetworkRegistration'},
      'Interfaces: all four (%s)' % list(ifs))
r = requests()
routes = [x for x in r if x.startswith('WMS Set-Routes')]
check(routes and 'routes=0,0,1,1;0,1,1,1;0,2,0,1;0,3,1,1;0,4,1,1' in routes[0],
      'WMS routes: class 2 on the SIM, the others NV, store and notify')

# --- SMS in ------------------------------------------------------------------
signals.clear()
ctl('sms +4366412345 Grüße aus Wien {€}')
check(wait_for(lambda: sigs('org.ofono.MessageManager', 'IncomingMessage'), 5), 'IncomingMessage')
s = sigs('org.ofono.MessageManager', 'IncomingMessage')
if s:
    text, info = s[0][3]
    check(text == 'Grüße aus Wien {€}' and info['Sender'] == '+4366412345',
          'its text and sender (%r %s)' % (text, info.get('Sender')))
    lt = info['LocalSentTime']
    try:
        tt = time.strptime(lt, '%Y-%m-%dT%H:%M:%S%z')
        ok = lt.endswith('+0100')
    except ValueError:
        ok = False
    check(ok, 'LocalSentTime %s as sphone parses it (zone +01)' % lt)
check(wait_for(lambda: [x for x in requests() if x.startswith('WMS Modify-Tag') and 'tag=0' in x]),
      'the message tagged read')
signals.clear()
long_text = ''.join('abcdefghij'[i % 10] for i in range(300))
ctl('sms-long 0664111 ' + long_text)
check(wait_for(lambda: sigs('org.ofono.MessageManager', 'IncomingMessage'), 5) and
      sigs('org.ofono.MessageManager', 'IncomingMessage')[0][3][0] == long_text and
      len(sigs('org.ofono.MessageManager', 'IncomingMessage')) == 1,
      'three parts: one IncomingMessage with the whole text')
signals.clear()
ctl('sms-class0 Info Flash!')
check(wait_for(lambda: sigs('org.ofono.MessageManager', 'ImmediateMessage'), 5) and
      sigs('org.ofono.MessageManager', 'ImmediateMessage')[0][3][0] == 'Flash!',
      'class 0: ImmediateMessage')

# --- SMS out -----------------------------------------------------------------
signals.clear()
mm = iface(MP, 'org.ofono.MessageManager')
path = mm.SendMessage('+43699111', 'Hallo €uro und Grüße')
check(path.startswith(MP + '/message_'), 'SendMessage: %s' % path)
check(wait_for(lambda: sigs('org.ofono.Message', 'PropertyChanged', path)), 'Message state changed')
st = sigs('org.ofono.Message', 'PropertyChanged', path)
check(st and st[-1][3][0] == 'State' and st[-1][3][1] == 'sent', 'Message sent')
check(wait_for(lambda: sigs('org.ofono.MessageManager', 'MessageRemoved')), 'MessageRemoved at sent')
sent = [x for x in requests() if x.startswith('WMS Raw-Send')]
check(sent and "to=+43699111" in sent[-1] and "text='Hallo €uro und Grüße'" in sent[-1] and
      'format=6' in sent[-1], 'the PDU decodes to number and text (%s)' % sent[-1:])
signals.clear()
path = mm.SendMessage('0664222', 'x' * 400)
check(wait_for(lambda: sigs('org.ofono.MessageManager', 'MessageRemoved')), 'long message done')
sent = [x for x in requests() if x.startswith('WMS Raw-Send') and 'to=0664222' in x]
check(len(sent) == 3 and all('/3/' in x for x in sent), '400 characters in three parts')
check(dbus_error(mm.SendMessage, 'abc', 'x') == 'org.ofono.Error.InvalidFormat',
      'SendMessage: a bad number refused')

# --- a call out --------------------------------------------------------------
signals.clear()
vcm = iface(MP, 'org.ofono.VoiceCallManager')
cp = vcm.Dial('+43111222', '')
check(cp.startswith(MP + '/voicecall'), 'Dial: %s' % cp)
added = sigs('org.ofono.VoiceCallManager', 'CallAdded')
check(wait_for(lambda: sigs('org.ofono.VoiceCallManager', 'CallAdded')) and
      sigs('org.ofono.VoiceCallManager', 'CallAdded')[0][3][1]['State'] == 'dialing' and
      sigs('org.ofono.VoiceCallManager', 'CallAdded')[0][3][1]['LineIdentification'] == '+43111222',
      'CallAdded: dialing, the number')
check(len(sigs('org.ofono.VoiceCallManager', 'CallAdded')) == 1, 'one CallAdded')


def call_state(p):
    st = [s[3][1] for s in sigs('org.ofono.VoiceCall', 'PropertyChanged', p) if s[3][0] == 'State']
    return st[-1] if st else None


check(wait_for(lambda: call_state(cp) == 'alerting'), 'alerting')
check(wait_for(lambda: 'route ear\nvoice start' in open(AUDIOLOG).read()),
      'pro1x-callaudio: route ear, voice start with the call')
ctl('remote-answer')
check(wait_for(lambda: call_state(cp) == 'active'), 'active once answered')
check(props(cp, 'org.ofono.VoiceCall').get('StartTime'), 'StartTime')
calls = vcm.GetCalls()
check(len(calls) == 1 and calls[0][0] == cp and calls[0][1]['State'] == 'active', 'GetCalls')
vcm.SendTones('12#')
dt = [x for x in requests() if 'Continuous-DTMF' in x]
check([x.split()[1] for x in dt] == ['Start-Continuous-DTMF', 'Stop-Continuous-DTMF'] * 3 and
      [x.split('digit=')[1] for x in dt if 'digit=' in x] == ['1', '2', '#'],
      'SendTones: start and stop for 1, 2, # (%s)' % dt)
check(dbus_error(vcm.SendTones, '12x') == 'org.ofono.Error.InvalidFormat', 'SendTones: bad tone')
iface(cp, 'org.ofono.VoiceCall').Hangup()
check(wait_for(lambda: call_state(cp) == 'disconnected'), 'disconnected after Hangup')
check(wait_for(lambda: sigs('org.ofono.VoiceCallManager', 'CallRemoved', MP)), 'CallRemoved')
check(wait_for(lambda: 'voice stop' in open(AUDIOLOG).read()), 'pro1x-callaudio: voice stop after the call')
check(len(vcm.GetCalls()) == 0, 'no calls left')

# --- a call in ---------------------------------------------------------------
signals.clear()
ctl('call-in +43555666')
check(wait_for(lambda: sigs('org.ofono.VoiceCallManager', 'CallAdded')), 'incoming: CallAdded')
a = sigs('org.ofono.VoiceCallManager', 'CallAdded')
ip = a[0][3][0] if a else '/none'
check(a and a[0][3][1]['State'] == 'incoming' and a[0][3][1]['LineIdentification'] == '+43555666',
      'incoming, its number')
iface(ip, 'org.ofono.VoiceCall').Answer()
check(wait_for(lambda: call_state(ip) == 'active'), 'Answer: active')
ctl('remote-hangup')
check(wait_for(lambda: call_state(ip) == 'disconnected'), 'the far end hangs up: disconnected')
check(wait_for(lambda: sigs('org.ofono.VoiceCallManager', 'CallRemoved')), 'CallRemoved')

# HangupAll
signals.clear()
ctl('call-in +43777')
check(wait_for(lambda: sigs('org.ofono.VoiceCallManager', 'CallAdded')), 'another call in')
vcm.HangupAll()
check(wait_for(lambda: sigs('org.ofono.VoiceCallManager', 'CallRemoved')), 'HangupAll ends it')
check(dbus_error(vcm.Dial, '12ab', '') == 'org.ofono.Error.InvalidFormat', 'Dial: bad number')

# --- offline -----------------------------------------------------------------
signals.clear()
iface(MP, 'org.ofono.Modem').SetProperty('Online', dbus.Boolean(False))
check(wait_for(lambda: not props(MP, 'org.ofono.Modem')['Online']), 'Online false')
check(wait_for(lambda: 'org.ofono.NetworkRegistration' not in
               props(MP, 'org.ofono.Modem')['Interfaces']), 'no NetworkRegistration offline')
iface(MP, 'org.ofono.Modem').SetProperty('Online', dbus.Boolean(True))
check(wait_for(lambda: nr().get('Status') == 'registered', 10), 'online again: registered')

print('passed %d failed %d' % (passed, failed))
sys.exit(failed != 0)
