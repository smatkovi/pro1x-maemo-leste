#!/usr/bin/env python3
"""freebsd/modem/test's simulated modem with the services of mobile data.

fakemodem.py's Modem (DMS, UIM, NAS, ... over QMUX on a pty) extended,
without changing it, by WDS, WDA and DPM as libqmi's data files define
them (libqmi's data/qmi-service-{wds,wda,dpm}.json: message ids, TLV ids,
layouts: Start Network 0x20, Stop Network 0x21, Packet Service Status
0x22, Get Current Settings 0x2D, Set IP Family 0x4D, Bind Mux Data Port
0xA2; WDA Set/Get Data Format 0x20/0x21; DPM Open Port 0x20).

The environment chooses how the modem behaves:
    FAKEDATA_WDA=qmapv5|qmapv4|qmap  the one aggregation protocol it takes
    FAKEDATA_WDA_EP=1                Get Data Format wants the endpoint info
Control commands (besides fakemodem's): "data-drop" (the network ends every
session: a Packet Service Status indication per session).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                '..', '..', 'modem', 'test'))
import fakemodem as fm  # noqa: E402

SVC_WDS, SVC_WDA, SVC_DPM = 0x01, 0x1A, 0x2F
fm.SVC_NAMES.update({SVC_WDS: 'WDS', SVC_WDA: 'WDA', SVC_DPM: 'DPM'})
ERR_CALL_FAILED, ERR_MISSING_ARGUMENT, ERR_INVALID_ARG = 14, 17, 48
PROTOS = {'qmap': 5, 'qmapv4': 8, 'qmapv5': 9}
LINK_RAW_IP = 2
tlv, result = fm.tlv, fm.result


def u32(b):
    return struct.unpack('<I', b[:4])[0]


def ip4(s):
    a, b, c, d = (int(x) for x in s.split('.'))
    return struct.pack('<I', a << 24 | b << 16 | c << 8 | d)


def ip6(s, prefix=None):
    import ipaddress
    b = ipaddress.IPv6Address(s).packed      # 8 guint16, network order
    return b if prefix is None else b + bytes([prefix])


class DataModem(fm.Modem):
    def __init__(self, a, log):
        super().__init__(a, log)
        self.wda_takes = PROTOS[os.environ.get('FAKEDATA_WDA', 'qmapv4')]
        self.wda_ep = os.environ.get('FAKEDATA_WDA_EP') == '1'
        self.fmt = (1, 0, 0)            # 802.3, no aggregation
        self.bound = {}                 # WDS cid -> mux id
        self.family = {}                # WDS cid -> 4 or 6
        self.sessions = {}              # WDS cid -> (handle, family)
        self.next_handle = 0x81000001

    def fmt_tlvs(self):
        link, ul, dl = self.fmt
        return (tlv(0x10, bytes([0])) + tlv(0x11, struct.pack('<I', link)) +
                tlv(0x12, struct.pack('<I', ul)) + tlv(0x13, struct.pack('<I', dl)) +
                tlv(0x15, struct.pack('<I', 32)) + tlv(0x16, struct.pack('<I', 32768)))

    def handle(self, svc, cid, txn, msg, tl):
        if svc not in (SVC_WDS, SVC_WDA, SVC_DPM):
            return super().handle(svc, cid, txn, msg, tl)
        r, out, name, kw = result(), b'', 'msg-0x%04x' % msg, {}
        if svc == SVC_DPM and msg == 0x0020:
            name = 'Open-Port'
            hw = tl.get(0x11, b'\0')
            n = hw[0]
            kw['ports'] = n
            if n:
                t, ifn, rx, tx = struct.unpack_from('<IIII', hw, 1)
                kw.update(type=t, iface=ifn, rx=rx, tx=tx)
        elif svc == SVC_WDA and msg == 0x0021:
            name = 'Get-Data-Format'
            kw['endpoint'] = 'yes' if 0x10 in tl else 'no'
            if self.wda_ep and 0x10 not in tl:
                r = result(ERR_MISSING_ARGUMENT)
            else:
                out = self.fmt_tlvs()
        elif svc == SVC_WDA and msg == 0x0020:
            name = 'Set-Data-Format'
            link, ul, dl = u32(tl[0x11]), u32(tl[0x12]), u32(tl[0x13])
            kw.update(link=link, ul=ul, dl=dl, datagrams=u32(tl.get(0x15, b'\0' * 4)),
                      size=u32(tl.get(0x16, b'\0' * 4)),
                      endpoint='%u/%u' % struct.unpack('<II', tl[0x17]) if 0x17 in tl else 'no')
            if self.wda_ep and 0x17 not in tl:
                r = result(ERR_MISSING_ARGUMENT)
            elif ul != self.wda_takes or dl != self.wda_takes:
                r = result(ERR_INVALID_ARG)
            else:
                self.fmt = (link, ul, dl)
                out = self.fmt_tlvs()
        elif svc == SVC_WDS and msg == 0x00A2:
            name = 'Bind-Mux-Data-Port'
            t, ifn = struct.unpack('<II', tl[0x10])
            mux = tl[0x11][0]
            kw.update(type=t, iface=ifn, mux=mux, client=u32(tl.get(0x13, b'\xff' * 4)))
            self.bound[cid] = mux
        elif svc == SVC_WDS and msg == 0x004D:
            name = 'Set-IP-Family'
            self.family[cid] = tl[0x01][0]
            kw['family'] = tl[0x01][0]
        elif svc == SVC_WDS and msg == 0x0020:
            name = 'Start-Network'
            apn = tl.get(0x14, b'').decode()
            fam = tl[0x19][0] if 0x19 in tl else 4
            kw.update(apn=apn, user=tl.get(0x17, b'').decode(),
                      password='set' if 0x18 in tl else 'none',
                      auth=tl[0x16][0] if 0x16 in tl else '-', family=fam,
                      mux=self.bound.get(cid, 'unbound'))
            if cid not in self.bound:
                r = result(ERR_INVALID_ARG)
            elif apn == 'wrong.apn':
                # 3GPP's reason 27, "missing or unknown APN"
                r = (result(ERR_CALL_FAILED) + tlv(0x10, struct.pack('<H', 1)) +
                     tlv(0x11, struct.pack('<Hh', 6, 27)))
            else:
                h = self.next_handle
                self.next_handle += 1
                self.sessions[cid] = (h, fam)
                out = tlv(0x01, struct.pack('<I', h))
                kw['handle'] = '%#x' % h
        elif svc == SVC_WDS and msg == 0x002D:
            name = 'Get-Current-Settings'
            fam = self.sessions.get(cid, (0, self.family.get(cid, 4)))[1]
            kw.update(requested='%#x' % u32(tl.get(0x10, b'\0' * 4)), family=fam)
            if fam == 4:
                out = (tlv(0x15, ip4('10.74.0.1')) + tlv(0x16, ip4('10.74.0.2')) +
                       tlv(0x1E, ip4('10.64.33.7')) + tlv(0x20, ip4('10.64.33.1')) +
                       tlv(0x21, ip4('255.255.255.0')) + tlv(0x29, struct.pack('<I', 1430)) +
                       tlv(0x2B, bytes([4])))
            else:
                out = (tlv(0x25, ip6('2a02:8388:1:2::42', 64)) +
                       tlv(0x26, ip6('2a02:8388:1:2::1', 64)) +
                       tlv(0x27, ip6('2a02:8388::53')) + tlv(0x28, ip6('2a02:8388::54')) +
                       tlv(0x29, struct.pack('<I', 1430)) + tlv(0x2B, bytes([6])))
        elif svc == SVC_WDS and msg == 0x0021:
            name = 'Stop-Network'
            h = u32(tl[0x01])
            kw['handle'] = '%#x' % h
            if self.sessions.get(cid, (None,))[0] == h:
                del self.sessions[cid]
            else:
                r = result(ERR_INVALID_ARG)
        self.record(svc, name, **kw)
        self.frame(svc, cid, msg, r + out, txn=txn)

    def command(self, line):
        if line == 'data-drop':
            for cid, (h, fam) in list(self.sessions.items()):
                # disconnected, no reconfiguration; reason 2 (generic: release normal)
                self.frame(SVC_WDS, cid, 0x0022, tlv(0x01, bytes([1, 0])) +
                           tlv(0x10, struct.pack('<H', 2)) + tlv(0x12, bytes([fam])),
                           indication=True)
                del self.sessions[cid]
            return 'ok'
        if line == 'sessions':
            return 'ok %d' % len(self.sessions)
        return super().command(line)


if __name__ == '__main__':
    fm.Modem = DataModem
    sys.exit(fm.main())
