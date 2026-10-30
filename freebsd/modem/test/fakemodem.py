#!/usr/bin/env python3
"""A simulated QMI modem for pro1x-modemd's tests (freebsd/modem/test).

It speaks QMUX on a pty, as a USB modem's cdc-wdm port would: libqmi's
QMUX endpoint opens the pty's other end (pro1x-modemd -d PTY), and every
message goes through libqmi's own encoders and decoders on that side. On
this side the messages are written from libqmi's data files
(data/qmi-service-{ctl,dms,uim,nas,wms,voice}.json, libqmi 1.34: the
message ids, the TLV ids and their layouts; strings that are a whole TLV
have no length prefix, strings inside a sequence a guint8 one), not from
pro1x-modemd's code. The SMS PDUs it delivers and decodes are built here
from 3GPP TS 23.040/23.038, independently of pro1x-modemd's sms.c.

    fakemodem.py --pty-link PATH --ctl SOCKET --log FILE [--pin PIN]
                 [--retries N] [--no-sim] [--online]

PATH becomes a symlink to the pty's slave. SOCKET takes one command per
connection and answers "ok ..." or "error ...":

    sms NUMBER TEXT         a message arrives (stored in NV, event report)
    sms-long NUMBER TEXT    the same as three concatenated parts
    sms-class0 NUMBER TEXT  a class 0 ("flash") message
    call-in NUMBER          a call comes in
    remote-answer           the far end answers the outgoing call
    remote-hangup           the far end ends every call
    log                     the requests so far, one per line
    state                   the simulated modem's state

The log file has one line per request: "SERVICE Message-Name key=value...".
"""
import argparse
import os
import selectors
import socket
import struct
import sys
import time
import tty

# --- QMI framing (libqmi's qmi-message.c: struct qmux_header,
# control_header, service_header, tlv) ---------------------------------
QMUX_MARKER = 0x01
SVC_CTL, SVC_DMS, SVC_NAS, SVC_WMS, SVC_VOICE, SVC_UIM = 0, 2, 3, 5, 9, 11
SVC_NAMES = {0: 'CTL', 2: 'DMS', 3: 'NAS', 5: 'WMS', 9: 'VOICE', 11: 'UIM'}
# QMI_PROTOCOL_ERROR_* (qmi-errors.h)
ERR_INTERNAL, ERR_INVALID_ID, ERR_INCORRECT_PIN, ERR_NO_EFFECT = 3, 41, 12, 26
ERR_NOT_SUPPORTED, ERR_INVALID_ARG, ERR_PIN_BLOCKED = 94, 48, 35


def tlv(t, v):
    return struct.pack('<BH', t, len(v)) + v


def result(err=0):
    return tlv(0x02, struct.pack('<HH', 1 if err else 0, err))


def pstr(s):
    """a string inside a sequence: its guint8 length first"""
    b = s.encode()
    return bytes([len(b)]) + b


def parse_tlvs(b):
    out, i = {}, 0
    while i + 3 <= len(b):
        t, n = struct.unpack_from('<BH', b, i)
        out[t] = b[i + 3:i + 3 + n]
        i += 3 + n
    return out


# --- SMS PDUs (23.040 9.2.2.1 SMS-DELIVER, 9.2.2.2 SMS-SUBMIT; 23.038
# 6.2.1 the default alphabet, the extension table) ----------------------
GSM7 = ('@£$¥èéùìòÇ\nØø\rÅåΔ_ΦΓΛΩΠΨΣΘΞ\x1bÆæßÉ !"#¤%&\'()*+,-./0123456789:;<=>?'
        '¡ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÑÜ§¿abcdefghijklmnopqrstuvwxyzäöñüà')
GSM7EXT = {'\f': 0x0a, '^': 0x14, '{': 0x28, '}': 0x29, '\\': 0x2f, '[': 0x3c,
           '~': 0x3d, ']': 0x3e, '|': 0x40, '€': 0x65}
assert len(GSM7) == 128


def septets_of(text):
    out = []
    for ch in text:
        if ch in GSM7EXT:
            out += [0x1b, GSM7EXT[ch]]
        elif ch in GSM7 and ch != '\x1b':
            out.append(GSM7.index(ch))
        else:
            return None
    return out


def pack7(septets, fill_bits=0):
    bits, nbits, out = 0, fill_bits, bytearray()
    for s in septets:
        bits |= s << nbits
        nbits += 7
        while nbits >= 8:
            out.append(bits & 0xff)
            bits >>= 8
            nbits -= 8
    if nbits:
        out.append(bits & 0xff)
    return bytes(out)


def unpack7(data, count, skip_bits=0):
    out = []
    for i in range(count):
        bit = skip_bits + 7 * i
        o, sh = bit // 8, bit % 8
        v = data[o] >> sh if o < len(data) else 0
        if sh > 1 and o + 1 < len(data):
            v |= data[o + 1] << (8 - sh)
        out.append(v & 0x7f)
    text, i = '', 0
    rev = {v: k for k, v in GSM7EXT.items()}
    while i < len(out):
        if out[i] == 0x1b and i + 1 < len(out):
            text += rev.get(out[i + 1], ' ')
            i += 2
        else:
            text += GSM7[out[i]]
            i += 1
    return text


def address(number):
    if not number.lstrip('+').isdigit():
        # alphanumeric (type of number 101): its 7-bit letters, the length
        # in semi-octets (23.040 9.1.2.5)
        sep = septets_of(number)
        return bytes([(7 * len(sep) + 3) // 4, 0xd0]) + pack7(sep)
    intl = number.startswith('+')
    digits = number.lstrip('+')
    b = bytearray([len(digits), 0x91 if intl else 0x81])
    d = digits + ('F' if len(digits) % 2 else '')
    for i in range(0, len(d), 2):
        b.append(int(d[i + 1], 16) << 4 | int(d[i], 16))
    return bytes(b)


def bcd2(v):
    return (v % 10) << 4 | (v // 10)


def scts(t, tz_quarters):
    tm = time.gmtime(t + tz_quarters * 15 * 60)
    z = abs(tz_quarters)
    zb = (z % 10) << 4 | (z // 10) | (0x08 if tz_quarters < 0 else 0)
    return bytes([bcd2(tm.tm_year % 100), bcd2(tm.tm_mon), bcd2(tm.tm_mday),
                  bcd2(tm.tm_hour), bcd2(tm.tm_min), bcd2(tm.tm_sec), zb])


def deliver_pdus(number, text, t, tz_quarters=4, parts=1, ref=42, dcs_class0=False):
    """SMS-DELIVER PDUs with the SMSC address in front (as WMS stores them)"""
    sep = septets_of(text)
    gsm = sep is not None
    smsc = bytes([0x07, 0x91]) + bytes.fromhex('3446000010F0')   # +436400000100
    chunks = []
    if parts == 1:
        chunks = [text]
    else:
        n = -(-len(text) // parts)
        chunks = [text[i:i + n] for i in range(0, len(text), n)]
    out = []
    for i, ch in enumerate(chunks):
        first = 0x04 | (0x40 if len(chunks) > 1 else 0)   # DELIVER, no more messages
        dcs = (0x00 if gsm else 0x08) | (0x10 if dcs_class0 else 0)
        udh = bytes([5, 0, 3, ref, len(chunks), i + 1]) if len(chunks) > 1 else b''
        if gsm:
            sep = septets_of(ch)
            # the header's septets, its fill bits included, then the text
            hs = (len(udh) * 8 + 6) // 7
            body = udh + pack7(sep, hs * 7 - len(udh) * 8)
            udl = hs + len(sep)
        else:
            body = udh + ch.encode('utf-16-be')
            udl = len(body)
        tpdu = (bytes([first]) + address(number) + bytes([0x00, dcs]) + scts(t, tz_quarters) +
                bytes([udl]) + body)
        out.append(smsc + tpdu)
    return out


def decode_submit(pdu):
    """number and text of an SMS-SUBMIT (the default SMSC in front)"""
    i = 1 + pdu[0]
    first = pdu[i]
    assert first & 3 == 1, 'not an SMS-SUBMIT'
    udhi = bool(first & 0x40)
    vpf = (first >> 3) & 3
    i += 2                                # first octet, message reference
    nd, ton = pdu[i], pdu[i + 1]
    digits = ''
    for b in pdu[i + 2:i + 2 + (nd + 1) // 2]:
        digits += '%x%x' % (b & 0xf, b >> 4)
    number = ('+' if ton == 0x91 else '') + digits[:nd]
    i += 2 + (nd + 1) // 2
    dcs = pdu[i + 1]
    i += 2 + (1 if vpf == 2 else 7 if vpf else 0)
    udl = pdu[i]
    ud = pdu[i + 1:]
    hdr = 1 + ud[0] if udhi else 0
    concat = None
    if udhi and ud[1] == 0 and ud[2] == 3:
        concat = (ud[3], ud[4], ud[5])
    if dcs & 0x0c == 0x08:
        text = ud[hdr:udl].decode('utf-16-be')
    else:
        skip = (hdr * 8 + 6) // 7
        text = unpack7(ud, udl - skip, skip * 7)
    return number, text, concat


# --- the simulated modem -----------------------------------------------
# enum values: libqmi's qmi-enums-*.h
CALL_ORIGINATION, CALL_INCOMING, CALL_CONVERSATION, CALL_ALERTING, CALL_END = 1, 2, 3, 5, 9
DIR_MO, DIR_MT = 1, 2
MODE_ONLINE, MODE_LOW_POWER = 0, 1
PIN_ENABLED_NOT_VERIFIED, PIN_ENABLED_VERIFIED, PIN_DISABLED, PIN_BLOCKED = 1, 2, 3, 4
APP_PIN_REQUIRED, APP_PUK_REQUIRED, APP_READY = 2, 3, 7
REG_NOT, REG_YES, REG_SEARCHING = 0, 1, 2
RADIO_LTE = 8


class Modem:
    def __init__(self, a, log):
        self.log = log
        self.pin = a.pin
        self.sim = not a.no_sim
        self.pin_state = PIN_ENABLED_NOT_VERIFIED if a.pin else PIN_DISABLED
        self.retries = a.retries
        self.puk_retries = 10
        self.mode = MODE_ONLINE if a.online else MODE_LOW_POWER
        self.cids = {}             # (service, cid) -> True
        self.next_cid = 1
        self.calls = {}            # id -> dict(state, dir, number)
        self.next_call = 1
        self.nv = {}               # index -> (tag, pdu)
        self.next_index = 0
        self.timers = []           # (when, fn)
        self.out = []              # frames to write
        self.requests = []
        self.events_mask = {}

    # framing
    def frame(self, svc, cid, msg, payload, indication=False, txn=0):
        if svc == SVC_CTL:
            hdr = struct.pack('<BBHH', 0x02 if indication else 0x01, txn, msg, len(payload))
        else:
            hdr = struct.pack('<BHHH', 0x04 if indication else 0x02, txn, msg, len(payload))
        body = struct.pack('<BBB', 0x80, svc, cid) + hdr + payload
        self.out.append(bytes([QMUX_MARKER]) + struct.pack('<H', len(body) + 2) + body)

    def indicate(self, svc, msg, payload):
        for (s, cid) in self.cids:
            if s == svc:
                self.frame(svc, cid, msg, payload, indication=True)

    def later(self, sec, fn):
        self.timers.append((time.monotonic() + sec, fn))

    def record(self, svc, name, **kw):
        line = SVC_NAMES[svc] + ' ' + name + ''.join(' %s=%s' % (k, v) for k, v in kw.items())
        self.requests.append(line)
        self.log.write(line + '\n')
        self.log.flush()

    # state as TLVs
    def sim_ready(self):
        return self.sim and self.pin_state in (PIN_ENABLED_VERIFIED, PIN_DISABLED)

    def card_status_tlv(self):
        if not self.sim:
            cards = bytes([1]) + bytes([0, 0, 0, 0, 0, 0])    # absent, no apps
            return tlv(0x10, struct.pack('<HHHH', 0xffff, 0xffff, 0xffff, 0xffff) + cards)
        app_state = (APP_READY if self.sim_ready() else
                     APP_PUK_REQUIRED if self.pin_state == PIN_BLOCKED else APP_PIN_REQUIRED)
        aid = bytes.fromhex('a0000000871002ff49ff0589')
        app = (bytes([2, app_state, 0, 0, 0, 0, len(aid)]) + aid +
               bytes([0, self.pin_state, self.retries, self.puk_retries, PIN_DISABLED, 3, 10]))
        card = bytes([1, 0, 0, 0, 0, 1]) + app                 # present, one application
        # GW primary: slot 0, application 0
        return tlv(0x10, struct.pack('<HHHH', 0x0000, 0xffff, 0xffff, 0xffff) + bytes([1]) + card)

    def registered(self):
        return self.mode == MODE_ONLINE and self.sim_ready()

    def serving_tlvs(self, indication=False):
        """Get Serving System's output, or its indication's: the same
        fields, but from 0x1C on under other TLV ids (LAC, CID, TAC: 0x1C,
        0x1D, 0x24 in the response, 0x1D, 0x1E, 0x25 in the indication)"""
        if self.registered():
            ss = bytes([REG_YES, 1, 1, 2, 1, RADIO_LTE])
            lac, cid, tac = (0x1D, 0x1E, 0x25) if indication else (0x1C, 0x1D, 0x24)
            return (tlv(0x01, ss) + tlv(0x10, bytes([0x01])) +   # roaming indicator: off (home)
                    tlv(0x12, struct.pack('<HH', 232, 3) + pstr('Fake Telecom')) +
                    tlv(lac, struct.pack('<H', 0x1234)) + tlv(cid, struct.pack('<I', 0x0abcdef)) +
                    tlv(tac, struct.pack('<H', 0x4321)))
        st = REG_SEARCHING if self.mode == MODE_ONLINE else REG_NOT
        return tlv(0x01, bytes([st, 0, 0, 0, 0]))

    def serving_changed(self):
        self.indicate(SVC_NAS, 0x0024, self.serving_tlvs(indication=True))
        if self.registered():
            t = time.gmtime()
            self.indicate(SVC_NAS, 0x004C, tlv(0x01, struct.pack('<HBBBBBB', t.tm_year, t.tm_mon,
                          t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, t.tm_wday)) +
                          tlv(0x10, struct.pack('<b', 4)))

    def calls_tlvs(self, info_id, nums_id):
        live = sorted(self.calls.items())
        info = bytes([len(live)])
        nums = bytes([len(live)])
        for cid, c in live:
            info += bytes([cid, c['state'], 0, c['dir'], 4, 0, 0])   # voice, LTE
            nums += bytes([cid, 0]) + pstr(c['number'])
        return tlv(info_id, info) + tlv(nums_id, nums)

    def calls_changed(self):
        self.indicate(SVC_VOICE, 0x002E, self.calls_tlvs(0x01, 0x10))
        for cid in [k for k, c in self.calls.items() if c['state'] == CALL_END]:
            del self.calls[cid]

    def store_sms(self, pdus):
        for p in pdus:
            idx = self.next_index
            self.next_index += 1
            self.nv[idx] = (1, p)                      # MT_NOT_READ
            if self.events_mask.get('mt'):
                self.indicate(SVC_WMS, 0x0001, tlv(0x10, struct.pack('<BI', 1, idx)) +
                              tlv(0x12, bytes([1])))

    # requests
    def handle(self, svc, cid, txn, msg, tl):
        r = result()
        if svc == SVC_CTL:
            if msg == 0x0022:                         # Allocate CID
                s = tl[0x01][0]
                c = self.next_cid
                self.next_cid += 1
                self.cids[(s, c)] = True
                self.record(svc, 'Allocate-CID', service=SVC_NAMES.get(s, s), cid=c)
                self.frame(0, 0, msg, result() + tlv(0x01, bytes([s, c])), txn=txn)
            elif msg == 0x0023:                       # Release CID
                s, c = tl[0x01][0], tl[0x01][1]
                self.cids.pop((s, c), None)
                self.record(svc, 'Release-CID', service=SVC_NAMES.get(s, s), cid=c)
                self.frame(0, 0, msg, result() + tlv(0x01, bytes([s, c])), txn=txn)
            else:
                self.record(svc, 'msg-0x%04x' % msg)
                self.frame(0, 0, msg, result(ERR_NOT_SUPPORTED), txn=txn)
            return
        out = b''
        name = 'msg-0x%04x' % msg
        post = None
        if svc == SVC_DMS:
            if msg == 0x0021:
                name, out = 'Get-Manufacturer', tlv(0x01, b'QUALCOMM INCORPORATED')
            elif msg == 0x0022:
                name, out = 'Get-Model', tlv(0x01, b'Fake SM6115 modem')
            elif msg == 0x0023:
                name, out = 'Get-Revision', tlv(0x01, b'MPSS.FAKE.1.0')
            elif msg == 0x0025:
                name, out = 'Get-IDs', tlv(0x11, b'350000000000001')
            elif msg == 0x002D:
                name, out = 'Get-Operating-Mode', tlv(0x01, bytes([self.mode]))
            elif msg == 0x002E:
                name = 'Set-Operating-Mode'
                self.mode = tl[0x01][0]
                post = lambda: self.later(0.2, self.serving_changed)
            elif msg == 0x0043:
                name = 'UIM-Get-IMSI'
                out = tlv(0x01, b'232030000000001') if self.sim_ready() else b''
                if not self.sim_ready():
                    r = result(ERR_INTERNAL)
            elif msg == 0x003C:
                name, out = 'UIM-Get-ICCID', tlv(0x01, b'8943030000000000001')
            else:
                r = result(ERR_NOT_SUPPORTED)
        elif svc == SVC_UIM:
            if msg == 0x002E:
                name = 'Register-Events'
                out = tlv(0x10, tl[0x01])
            elif msg == 0x002F:
                name, out = 'Get-Card-Status', self.card_status_tlv()
            elif msg in (0x0026, 0x0027):
                info = tl[0x02]
                pin_id = info[0]
                if msg == 0x0026:
                    name = 'Verify-PIN'
                    pin = info[2:2 + info[1]].decode()
                    self.record(svc, name, pin_id=pin_id, pin_len=len(pin))
                    ok = pin == self.pin and self.pin_state == PIN_ENABLED_NOT_VERIFIED
                    if ok:
                        self.pin_state = PIN_ENABLED_VERIFIED
                        self.retries = 3
                    elif self.pin_state == PIN_ENABLED_NOT_VERIFIED:
                        self.retries -= 1
                        if self.retries == 0:
                            self.pin_state = PIN_BLOCKED
                else:
                    name = 'Unblock-PIN'
                    n = info[1]
                    puk = info[2:2 + n].decode()
                    newpin = info[3 + n:3 + n + info[2 + n]].decode()
                    self.record(svc, name, pin_id=pin_id)
                    ok = puk == '12345678' and self.pin_state == PIN_BLOCKED
                    if ok:
                        self.pin, self.pin_state, self.retries = newpin, PIN_ENABLED_VERIFIED, 3
                    else:
                        self.puk_retries -= 1
                out = tlv(0x10, bytes([self.retries, self.puk_retries]))
                if not ok:
                    r = result(ERR_INCORRECT_PIN if self.pin_state != PIN_BLOCKED or msg == 0x0027
                               else ERR_PIN_BLOCKED)
                post = lambda: self.later(0.1, self.sim_changed)
                self.frame(svc, cid, msg, r + out, txn=txn)
                if post:
                    post()
                return
            else:
                r = result(ERR_NOT_SUPPORTED)
        elif svc == SVC_NAS:
            if msg == 0x0003:
                name = 'Register-Indications'
            elif msg == 0x0024:
                name, out = 'Get-Serving-System', self.serving_tlvs()
            elif msg == 0x0020:
                name = 'Get-Signal-Strength'
                if self.registered():
                    out = tlv(0x01, struct.pack('<bb', -75, RADIO_LTE))
                else:
                    r = result(ERR_NO_EFFECT)
            else:
                r = result(ERR_NOT_SUPPORTED)
        elif svc == SVC_WMS:
            if msg == 0x0032:
                n = struct.unpack_from('<H', tl[0x01])[0]
                routes = [tuple(tl[0x01][2 + 4 * i:6 + 4 * i]) for i in range(n)]
                name = 'Set-Routes'
                self.record(svc, name, routes=';'.join('%d,%d,%d,%d' % x for x in routes))
                self.frame(svc, cid, msg, r, txn=txn)
                return
            elif msg == 0x0001:
                name = 'Set-Event-Report'
                self.events_mask['mt'] = bool(tl.get(0x10, b'\0')[0])
            elif msg == 0x0031:
                storage, tag = tl[0x01][0], tl.get(0x11, b'\xff')[0]
                name = 'List-Messages'
                self.record(svc, name, storage=storage, tag=tag)
                items = [(i, t) for i, (t, _) in sorted(self.nv.items())
                         if storage == 1 and (tag == 0xff or t == tag)]
                if storage == 0 and not self.sim_ready():
                    self.frame(svc, cid, msg, result(ERR_INTERNAL), txn=txn)
                    return
                body = struct.pack('<I', len(items)) + b''.join(struct.pack('<IB', i, t)
                                                               for i, t in items)
                self.frame(svc, cid, msg, r + tlv(0x01, body), txn=txn)
                return
            elif msg == 0x0022:
                storage, idx = struct.unpack_from('<BI', tl[0x01])
                name = 'Raw-Read'
                self.record(svc, name, storage=storage, index=idx)
                if storage != 1 or idx not in self.nv:
                    self.frame(svc, cid, msg, result(ERR_INVALID_ARG), txn=txn)
                    return
                t, p = self.nv[idx]
                self.frame(svc, cid, msg, r + tlv(0x01, struct.pack('<BBH', t, 6, len(p)) + p),
                           txn=txn)
                return
            elif msg == 0x0023:
                storage, idx, t = struct.unpack_from('<BIB', tl[0x01])
                name = 'Modify-Tag'
                self.record(svc, name, storage=storage, index=idx, tag=t)
                if idx in self.nv:
                    self.nv[idx] = (t, self.nv[idx][1])
                self.frame(svc, cid, msg, r, txn=txn)
                return
            elif msg == 0x0020:
                fmt, n = struct.unpack_from('<BH', tl[0x01])
                pdu = tl[0x01][3:3 + n]
                number, text, concat = decode_submit(pdu)
                name = 'Raw-Send'
                self.record(svc, name, format=fmt, to=number, text=repr(text),
                            concat=concat and '%d/%d/%d' % concat)
                self.frame(svc, cid, msg, r + tlv(0x01, struct.pack('<H', 7)), txn=txn)
                return
            else:
                r = result(ERR_NOT_SUPPORTED)
        elif svc == SVC_VOICE:
            if msg == 0x0003:
                name = 'Indication-Register'
            elif msg == 0x002F:
                name, out = 'Get-All-Call-Info', self.calls_tlvs(0x10, 0x11)
            elif msg == 0x0020:
                number = tl[0x01].decode()
                cidn = self.next_call
                self.next_call += 1
                self.calls[cidn] = dict(state=CALL_ORIGINATION, dir=DIR_MO, number=number)
                name = 'Dial-Call'
                self.record(svc, name, number=number)
                # the call's first notification before the answer, as modems do
                self.calls_changed()
                self.frame(svc, cid, msg, r + tlv(0x10, bytes([cidn])), txn=txn)
                self.later(0.3, lambda: self.set_call(cidn, CALL_ALERTING))
                return
            elif msg in (0x0021, 0x0022):
                cidn = tl[0x01][0]
                name = 'End-Call' if msg == 0x0021 else 'Answer-Call'
                self.record(svc, name, call=cidn)
                if cidn not in self.calls:
                    self.frame(svc, cid, msg, result(ERR_INVALID_ID), txn=txn)
                    return
                self.frame(svc, cid, msg, r + tlv(0x10, bytes([cidn])), txn=txn)
                self.later(0.1, lambda: self.set_call(cidn, CALL_END if msg == 0x0021
                                                      else CALL_CONVERSATION))
                return
            elif msg in (0x0029, 0x002A):
                d = tl[0x01]
                name = 'Start-Continuous-DTMF' if msg == 0x0029 else 'Stop-Continuous-DTMF'
                kw = dict(call=d[0])
                if msg == 0x0029:
                    kw['digit'] = chr(d[1])
                self.record(svc, name, **kw)
                self.frame(svc, cid, msg, r + tlv(0x10, bytes([d[0]])), txn=txn)
                return
            else:
                r = result(ERR_NOT_SUPPORTED)
        self.record(svc, name)
        self.frame(svc, cid, msg, r + out, txn=txn)
        if post:
            post()

    def sim_changed(self):
        self.indicate(SVC_UIM, 0x0032, self.card_status_tlv())
        self.later(0.2, self.serving_changed)

    def set_call(self, cidn, state):
        if cidn in self.calls:
            self.calls[cidn]['state'] = state
            self.calls_changed()

    # control commands
    def command(self, line):
        w = line.split(' ', 2)
        cmd = w[0]
        if cmd in ('sms', 'sms-long', 'sms-class0'):
            parts = 3 if cmd == 'sms-long' else 1
            self.store_sms(deliver_pdus(w[1], w[2], int(time.time()), parts=parts,
                                        dcs_class0=cmd == 'sms-class0'))
            return 'ok'
        if cmd == 'call-in':
            cidn = self.next_call
            self.next_call += 1
            self.calls[cidn] = dict(state=CALL_INCOMING, dir=DIR_MT, number=w[1])
            self.calls_changed()
            return 'ok %d' % cidn
        if cmd == 'remote-answer':
            for cidn, c in self.calls.items():
                if c['dir'] == DIR_MO and c['state'] in (CALL_ORIGINATION, CALL_ALERTING):
                    c['state'] = CALL_CONVERSATION
            self.calls_changed()
            return 'ok'
        if cmd == 'remote-hangup':
            for c in self.calls.values():
                c['state'] = CALL_END
            self.calls_changed()
            return 'ok'
        if cmd == 'log':
            return 'ok\n' + '\n'.join(self.requests)
        if cmd == 'state':
            return 'ok mode=%d pin_state=%d retries=%d calls=%d nv=%d' % (
                self.mode, self.pin_state, self.retries, len(self.calls), len(self.nv))
        return 'error unknown command'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pty-link', required=True)
    ap.add_argument('--ctl', required=True)
    ap.add_argument('--log', required=True)
    ap.add_argument('--pin')
    ap.add_argument('--retries', type=int, default=3)
    ap.add_argument('--no-sim', action='store_true')
    ap.add_argument('--online', action='store_true')
    a = ap.parse_args()

    master, slave = os.openpty()
    tty.setraw(slave)              # a byte stream: no echo, no line discipline
    name = os.ttyname(slave)
    os.set_blocking(master, False)
    try:
        os.unlink(a.pty_link)
    except FileNotFoundError:
        pass
    os.symlink(name, a.pty_link)
    try:
        os.unlink(a.ctl)
    except FileNotFoundError:
        pass
    ctl = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    ctl.bind(a.ctl)
    ctl.listen(4)
    log = open(a.log, 'w')
    m = Modem(a, log)
    sel = selectors.DefaultSelector()
    sel.register(master, selectors.EVENT_READ, 'pty')
    sel.register(ctl, selectors.EVENT_READ, 'ctl')
    buf = b''
    print('fakemodem: %s' % name, flush=True)
    while True:
        now = time.monotonic()
        due = [t for t in m.timers if t[0] <= now]
        m.timers = [t for t in m.timers if t[0] > now]
        for _, fn in due:
            fn()
        while m.out:
            os.write(master, m.out.pop(0))
        timeout = min([t[0] for t in m.timers], default=now + 0.5) - time.monotonic()
        for key, _ in sel.select(max(timeout, 0.01)):
            if key.data == 'pty':
                try:
                    buf += os.read(master, 65536)
                except BlockingIOError:
                    continue
                except OSError:
                    # the other side closed (pro1x-modemd ended): wait for the next
                    time.sleep(0.05)
                    continue
                while len(buf) >= 3:
                    if buf[0] != QMUX_MARKER:
                        buf = buf[1:]
                        continue
                    n = struct.unpack_from('<H', buf, 1)[0]
                    if len(buf) < 1 + n:
                        break
                    f, buf = buf[1:1 + n], buf[1 + n:]
                    _, _, svc, cid = struct.unpack_from('<HBBB', f, 0)
                    if svc == SVC_CTL:
                        _, txn, msg, tlen = struct.unpack_from('<BBHH', f, 5)
                        tl = parse_tlvs(f[11:11 + tlen])
                    else:
                        _, txn, msg, tlen = struct.unpack_from('<BHHH', f, 5)
                        tl = parse_tlvs(f[12:12 + tlen])
                    m.handle(svc, cid, txn, msg, tl)
            else:
                c, _ = ctl.accept()
                line = c.makefile().readline().strip()
                c.sendall((m.command(line) + '\n').encode())
                c.close()


if __name__ == '__main__':
    sys.exit(main())
