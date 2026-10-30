#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""trace2seq.py -- the Leste trace (test/leste-audio-trace.sh) as replay
sequences for pro1x-audiod and the FM radio's stage 3c
(freebsd/audio/README.md, "The sequences"):

    python3 -I freebsd/audio/tools/trace2seq.py TRACE [-b BASE.seq] [-o OUT.seq]
    python3 -I freebsd/audio/tools/trace2seq.py --selftest

Per scenario of the trace (hph, spk, ear, mic) the events between the
script's markers "stream on" and "stream off" are the power-up, those
after "stream off" the power-down. Each is split in two: the back end's
part (rx-up / va-up: the macros' runtime resume with their regcache sync,
the q6afe clocks, the CGCR resets, the SoundWire masters) up to the AFE
port's configuration (an 84-byte APR packet, then its 24-byte
DEVICE_START), and the route's part (hph-up, ...: the SoundWire stream,
"afe start", DAPM's sequence, the amplifiers). Down the other way: the
route's part up to the first suspend, the back end's after it.

What the trace cannot show is inserted from Linux's code: the SoundWire
masters' init and enumeration after their CGCR reset (qcom.c's writel()s
are not traced), the WCD9370's probe-time init (swr wcd-init), the LPASS
pins after the DCODEC vote (base section [pins]: lpi_gpio_write is an
iowrite32), the stream's ports from the snapshot's slave registers. Gaps
of a millisecond or more between two writes become delays (Linux's
sleeps; at most 200 ms each). Reads are kept as comments, and every
register written during a power-up is compared with the snapshot taken
during the sound in a [check-<scenario>] section (pro1x-audioctl seq
check-hph).

Not taken from the trace: regmap_bulk_write and regmap_hw_write_start
(a raw block write, e.g. a regcache sync over a bus that writes blocks,
traces no regmap_reg_write): they are counted in a note, and their values
are in the snapshot's registers dump (the check section compares them);
if a note says so, those registers need w lines by hand from the dump.

The base file (default seq/base.seq next to this directory) has the
sections only code can give (power, pins, volume); it comes first in the
output.
"""

import os
import re
import sys

# regmap device names -> sequence targets
TARGETS = [
    (re.compile(r'^a600000\.codec'), 'rxmacro'),
    (re.compile(r'^a620000\.codec'), 'txmacro'),
    (re.compile(r'^a730000\.codec'), 'vamacro'),
    (re.compile(r'^a6a9000\.clock-controller'), 'lpass_audiocc'),
    (re.compile(r'^a7e0000\.clock-controller'), 'lpasscc'),
    (re.compile(r'^sdw:.*:3$'), 'wcd'),
    (re.compile(r'^sdw:.*:4$'), 'wcdrx'),
    (re.compile(r'^\d+-0059$'), 'aw0'),
    (re.compile(r'^\d+-005a$'), 'aw1'),
]
Q6_CLOCKS = {'LPASS_CLK_ID_TX_CORE_MCLK', 'LPASS_CLK_ID_TX_CORE_NPL_MCLK',
             'LPASS_CLK_ID_RX_CORE_MCLK', 'LPASS_CLK_ID_RX_CORE_NPL_MCLK',
             'LPASS_CLK_ID_VA_CORE_MCLK', 'LPASS_CLK_ID_VA_CORE_2X_MCLK'}
VOTES = {'LPASS_HW_MACRO', 'LPASS_HW_DCODEC'}
# the macros' MCLK_FREQ (lpass-rx-macro.c, lpass-tx-macro.c; the VA
# macro's is 2 x 9.6 MHz): the q6 clocks' rate when the trace has none
DEFAULT_RATE = 19200000
# CGCR resets (lpasscc-sm6115.c): register, bit -> the master behind it
CGCR = {('lpass_audiocc', 0x98): 'swr1', ('lpasscc', 0xc100): 'swr0'}
WCD_UNIQUE = {'swr0': 3, 'swr1': 4}
PLAYBACK = ('hph', 'spk', 'ear')
MAX_DELAY_MS = 200

EV = re.compile(r'^\s*(?P<task>.+?)-(?P<pid>\d+)\s+\[(?P<cpu>\d+)\]\s+'
                r'(?:\S+\s+)?(?P<ts>\d+\.\d+):\s+(?P<ev>\w+):\s?(?P<rest>.*)$')


def target(dev):
    for rx, t in TARGETS:
        if rx.search(dev):
            return t
    return None


class Event:
    __slots__ = ('ts', 'kind', 'dev', 'tgt', 'reg', 'val', 'name', 'size',
                 'tx', 'text')

    def __init__(self, ts, kind, **kw):
        self.ts = ts
        self.kind = kind
        for k in self.__slots__[2:]:
            setattr(self, k, kw.get(k))


def parse_event(line):
    m = EV.match(line)
    if not m:
        return None
    ts = float(m.group('ts'))
    ev, rest = m.group('ev'), m.group('rest')
    if ev in ('regmap_reg_write', 'regmap_reg_read'):
        mm = re.match(r'(\S+) reg=([0-9a-f]+) val=([0-9a-f]+)', rest)
        if not mm:
            return None
        return Event(ts, 'w' if ev.endswith('write') else 'r',
                     dev=mm.group(1), tgt=target(mm.group(1)),
                     reg=int(mm.group(2), 16), val=int(mm.group(3), 16))
    if ev in ('regmap_bulk_write', 'regmap_hw_write_start'):
        mm = re.match(r'(\S+) reg=([0-9a-f]+)', rest)
        if mm:
            return Event(ts, 'bulk', dev=mm.group(1), tgt=target(mm.group(1)),
                         reg=int(mm.group(2), 16), text=ev)
        return None
    if ev == 'regmap_cache_only':
        mm = re.match(r'(\S+) flag=(\d)', rest)
        if mm:
            return Event(ts, 'cache_only', dev=mm.group(1),
                         tgt=target(mm.group(1)), val=int(mm.group(2)))
        return None
    if ev in ('clk_prepare', 'clk_unprepare'):
        return Event(ts, ev, name=rest.strip())
    if ev == 'clk_set_rate':
        mm = re.match(r'(\S+) (\d+)', rest)
        if mm:
            return Event(ts, ev, name=mm.group(1), val=int(mm.group(2)))
        return None
    if ev == 'qcom_glink_cmd_tx_data':
        mm = re.match(r'(tx|rx) remote: \S+ channel: (\S+?)\[.*chunk_size: '
                      r'(\d+)', rest)
        if mm and mm.group(2) == 'apr_audio_svc':
            return Event(ts, 'apr', tx=mm.group(1) == 'tx',
                         size=int(mm.group(3)))
        return None
    if ev == 'gpio_value':
        mm = re.match(r'(\d+)\s+set (\d+)', rest)
        if mm:
            return Event(ts, 'gpio', reg=int(mm.group(1)),
                         val=int(mm.group(2)))
        return None
    if ev == 'snd_soc_dapm_widget_power':
        mm = re.match(r'widget=(.*) val=(\d+)', rest)
        if mm:
            return Event(ts, 'dapm', name=mm.group(1), val=int(mm.group(2)))
        return None
    if ev in ('snd_soc_dapm_start', 'snd_soc_dapm_done'):
        return Event(ts, ev, text=rest)
    if ev == 'tracing_mark_write':
        mm = re.match(r'pro1x-audio: (.*)', rest)
        if mm:
            return Event(ts, 'mark', text=mm.group(1).strip())
    return None


class Scenario:
    def __init__(self, name):
        self.name = name
        self.events = []
        self.state = {}      # path -> {reg: val}
        self.clk_rates = {}
        self.recorded = None


def parse_trace(text):
    """The trace's scenarios: their events and their snapshot."""
    gpio_base = None
    scen = {}
    cur = None
    part = None
    path = None
    for line in text.splitlines():
        if line.startswith('# tlmm gpio base:'):
            v = line.split(':', 1)[1].strip()
            if v.isdigit():
                gpio_base = int(v)
            continue
        m = re.match(r'^=== scenario (\w+)$', line)
        if m:
            cur = Scenario(m.group(1))
            scen[cur.name] = cur
            part = None
            continue
        if line.startswith('--- '):
            part = line[4:]
            path = None
            continue
        if cur is None:
            continue
        if part == 'trace':
            e = parse_event(line)
            if e is not None:
                cur.events.append(e)
            continue
        if part and part.startswith('state'):
            if line.startswith('>>> '):
                path = line[4:].strip()
                cur.state.setdefault(path, {})
                continue
            if path is None:
                continue
            if path.startswith('clk_summary'):
                f = line.split()
                if len(f) >= 5 and f[4].isdigit():
                    cur.clk_rates[f[0]] = int(f[4])
                continue
            mm = (re.match(r'^\s*(?:0x)?([0-9a-f]+):\s+(?:0x)?([0-9a-f]+)$',
                           line) or
                  re.match(r'^\s*([0-9a-f]+)\t\s*([0-9a-f]+)$', line))
            if mm:
                cur.state[path][int(mm.group(1), 16)] = int(mm.group(2), 16)
    return scen, gpio_base


def gpio_pin(g, base):
    if base is not None and g >= base:
        return g - base
    for b in (512, 0):          # the usual bases, then the raw number
        if g - b in (85, 92, 98):
            return g - b
    return g


def snapshot_ports(sc, master):
    """The stream's ports and channel masks: the slave's DPn ChannelEn of
    the bank in use (nonzero in either bank), from its registers file."""
    want = WCD_UNIQUE[master]
    for path, regs in sc.state.items():
        if not path.endswith('/registers') or 'master-' not in path:
            continue
        slave = path.rsplit('/', 2)[-2]
        if not re.search(r':%d$' % want, slave) or not re.match(r'sdw:', slave):
            continue
        ports = []
        for n in range(1, 15):
            b0 = regs.get(0x20 + 0x100 * n, 0)
            b1 = regs.get(0x30 + 0x100 * n, 0)
            if b0 or b1:
                ports.append((n, b0 or b1))
        return ports
    return None


def split_at(events, pred, start=0):
    for i in range(start, len(events)):
        if pred(events[i]):
            return i
    return None


def afe_config_index(events):
    """The 84-byte port configuration followed by a 24-byte start."""
    tx = [i for i, e in enumerate(events) if e.kind == 'apr' and e.tx]
    for a, b in zip(tx, tx[1:]):
        if events[a].size == 84 and events[b].size == 24:
            return a, b
    return None, None


class Emitter:
    def __init__(self, sc, gpio_base):
        self.sc = sc
        self.gpio_base = gpio_base
        self.lines = []
        self.last_ts = None
        self.written = {}
        self.inited = set()
        self.unknown = {}

    def gap(self, e):
        if self.last_ts is not None:
            ms = (e.ts - self.last_ts) * 1000.0
            if ms >= 1.0:
                self.lines.append('delay %d' % min(MAX_DELAY_MS,
                                                     int(ms + 0.999)))
        self.last_ts = e.ts

    def rate(self, name):
        for e in self.sc.events:
            if e.kind == 'clk_set_rate' and e.name == name and e.val:
                return e.val
        return self.sc.clk_rates.get(name) or DEFAULT_RATE

    def swr_init(self, master):
        if master in self.inited:
            return
        self.inited.add(master)
        self.lines.append('swr init %s        # qcom_swrm_init (code)' % master)
        self.lines.append('swr enum %s %d' % (master, WCD_UNIQUE[master]))
        if master == 'swr0':
            self.lines.append('swr wcd-init       # wcd937x_io_init (code), '
                              'once')

    def emit(self, e, hooks=True):
        k = e.kind
        if k == 'w':
            if e.tgt is None:
                self.unknown[e.dev] = self.unknown.get(e.dev, 0) + 1
                self.lines.append('# w %s %#x %#x (no target)' % (e.dev, e.reg,
                                                                  e.val))
                return
            if hooks and e.tgt == 'wcd' and 'swr0' not in self.inited:
                self.swr_init('swr0')
            self.gap(e)
            self.lines.append('w %s %#x %#x' % (e.tgt, e.reg, e.val))
            self.written[(e.tgt, e.reg)] = e.val
            if hooks:
                m = CGCR.get((e.tgt, e.reg))
                # the reset's deassert: bit 1 cleared again
                if m is not None and not e.val & 0x2:
                    self.swr_init(m)
        elif k == 'r':
            if e.tgt is not None:
                self.lines.append('# r %s %#x = %#x' % (e.tgt, e.reg, e.val))
        elif k == 'clk_prepare':
            if e.name in Q6_CLOCKS:
                self.gap(e)
                self.lines.append('clock %s %d' % (e.name, self.rate(e.name)))
            elif e.name in VOTES:
                self.gap(e)
                self.lines.append('vote %s on' % e.name)
                if hooks and e.name == 'LPASS_HW_DCODEC':
                    self.lines.append('call pins          # LPI pins (code)')
        elif k == 'clk_unprepare':
            if e.name in Q6_CLOCKS:
                self.gap(e)
                self.lines.append('clock %s 0' % e.name)
            elif e.name in VOTES:
                self.gap(e)
                self.lines.append('vote %s off' % e.name)
        elif k == 'gpio':
            self.gap(e)
            self.lines.append('gpio %d %d' % (gpio_pin(e.reg, self.gpio_base),
                                              e.val))
        elif k == 'apr':
            if e.tx and e.size not in (52, 40):
                self.lines.append('# apr tx %d' % e.size)
        elif k == 'dapm':
            self.lines.append('# dapm %s %d' % (e.name, e.val))
        elif k == 'snd_soc_dapm_start':
            self.lines.append('# dapm start %s' % e.text)
        elif k == 'cache_only':
            if e.val:
                self.lines.append('# %s suspended' % (e.tgt or e.dev))


def phases(sc):
    """Indices of the markers: stream on, stream off, end."""
    on = off = end = None
    for i, e in enumerate(sc.events):
        if e.kind != 'mark':
            continue
        if e.text.endswith('stream on'):
            on = i
        elif 'stream off' in e.text:
            off = i
        elif e.text.startswith(sc.name + ' end'):
            end = i
    if on is None:
        on = 0
    if off is None:
        off = len(sc.events)
    if end is None:
        end = len(sc.events)
    return on, off, end


def convert(sc, gpio_base, be_done):
    """Sections of one scenario: {name: lines}, the counts, notes."""
    ev = sc.events
    on, off, end = phases(sc)
    up = ev[on + 1:off]
    down = ev[off + 1:end]
    out = {}
    notes = []
    be = 'va' if sc.name == 'mic' else 'rx'
    master = None if sc.name == 'mic' else 'swr1'
    ports = snapshot_ports(sc, master) if master else None
    cfg, start = afe_config_index(up)
    if cfg is None:
        cfg = split_at(up, lambda e: e.kind == 'snd_soc_dapm_start')
        notes.append('%s: no AFE port configuration on GLINK: split at '
                     'DAPM\'s start' % sc.name)
        start = None
    if cfg is None:
        cfg = len(up)
    # the back end's part (once per back end)
    em = Emitter(sc, gpio_base)
    for e in up[:cfg]:
        em.emit(e)
    if be not in be_done:
        out[be + '-up'] = em.lines
    written = dict(em.written)
    # the route's part: the stream, the AFE port, DAPM
    em2 = Emitter(sc, gpio_base)
    em2.inited = set(em.inited)
    em2.last_ts = em.last_ts
    for i, e in enumerate(up[cfg:], cfg):
        if start is not None and i == start:
            if ports:
                em2.lines.append('swr stream %s on %s   # the snapshot\'s '
                                 'ports' % (master, ' '.join(
                                     '%d:%#x' % p for p in ports)))
            elif master:
                notes.append('%s: no slave registers in the snapshot: no '
                             'SoundWire stream line' % sc.name)
            em2.lines.append('afe start')
            continue
        em2.emit(e)
    if start is None:
        if ports:
            em2.lines.insert(0, 'swr stream %s on %s' % (master, ' '.join(
                '%d:%#x' % p for p in ports)))
        em2.lines.insert(1 if ports else 0, 'afe start')
    out[sc.name + '-up'] = em2.lines
    written.update(em2.written)
    # down: the route's part up to the first suspend, the back end's after
    cut = split_at(down, lambda e: (e.kind == 'cache_only' and e.val) or
                   (e.kind == 'clk_unprepare' and
                    (e.name in Q6_CLOCKS or e.name in VOTES)))
    if cut is None:
        cut = len(down)
    em3 = Emitter(sc, gpio_base)
    em3.inited = {'swr0', 'swr1'}
    if ports:
        em3.lines.append('swr stream %s off %s' % (master, ' '.join(
            '%d:%#x' % p for p in ports)))
    for e in down[:cut]:
        em3.emit(e, hooks=False)
    out[sc.name + '-down'] = em3.lines
    em4 = Emitter(sc, gpio_base)
    em4.inited = {'swr0', 'swr1'}
    for e in down[cut:]:
        em4.emit(e, hooks=False)
    if be not in be_done:
        out[be + '-down'] = em4.lines
    # the check: what was written, against the snapshot
    snap = {}
    for path, regs in sc.state.items():
        t = None
        mm = re.search(r'/regmap/([^/]+)/registers$', path)
        if mm:
            t = target(mm.group(1))
        if t:
            snap.setdefault(t, {}).update(regs)
    chk = []
    for (t, reg), _ in sorted(written.items()):
        if t in snap and reg in snap[t]:
            chk.append('r %s %#x %#x' % (t, reg, snap[t][reg]))
    out['check-' + sc.name] = chk
    # hw_write_start also comes with every plain write of a bus regmap:
    # only those without a regmap_reg_write right before count
    plain = {(e.dev, e.reg) for e in up + down if e.kind == 'w'}
    raw = {}
    for e in up + down:
        if e.kind == 'bulk' and (e.dev, e.reg) not in plain:
            raw[e.dev] = raw.get(e.dev, 0) + 1
    for dev, n in sorted(raw.items()):
        notes.append('%s: %d raw/bulk write(s) to %s without their values '
                     '(see the snapshot)' % (sc.name, n, dev))
    unknown = {}
    for x in (em, em2, em3, em4):
        for dev, n in x.unknown.items():
            unknown[dev] = unknown.get(dev, 0) + n
    for dev, n in sorted(unknown.items()):
        notes.append('%s: %d write(s) to %s left out (no target)' %
                     (sc.name, n, dev))
    return out, notes


def generate(text, base_text='', source='trace'):
    scen, gpio_base = parse_trace(text)
    head = text.splitlines()[0] if text else ''
    sections = {}
    notes = []
    be_done = set()
    order = [s for s in ('hph', 'spk', 'ear', 'mic') if s in scen]
    for name in order:
        out, n = convert(scen[name], gpio_base, be_done)
        notes += n
        for k, v in out.items():
            if k not in sections:
                sections[k] = v
        be_done.add('va' if name == 'mic' else 'rx')
    lines = ['# pro1x audio sequences, made by trace2seq.py from %s' % source,
             '# %s' % head.lstrip('# '),
             '# scenarios: %s' % (', '.join(order) or 'none')]
    for n in notes:
        lines.append('# note: ' + n)
    lines.append('')
    if base_text:
        lines.append(base_text.rstrip('\n'))
        lines.append('')
    base_names = set(re.findall(r'^\[([^\]]+)\]', base_text, re.M))
    for k in sorted(sections, key=lambda s: (s.startswith('check-'), s)):
        if k in base_names:
            lines.append('# [%s] from the trace left out: the base has it' % k)
            continue
        lines.append('[%s]' % k)
        lines.extend(sections[k] or ['mark %s: nothing in the trace' % k])
        lines.append('')
    return '\n'.join(lines) + '\n', sections, notes


# --- the self test: a synthetic trace in the script's and ftrace's format ---

SAMPLE = r"""# leste-audio-trace v1: 7.1.0-pro1x.8, 2026-10-31T10:00:00+01:00, card Pro1X (0)
# scenarios: hph,mic
# tlmm gpio base: 512
=== scenario hph
--- controls on
cset HPHL Switch = 1
--- trace
            bash-900     [001] .....   100.000000: tracing_mark_write: pro1x-audio: hph start (cold)
            bash-900     [001] .....   100.500000: tracing_mark_write: pro1x-audio: hph stream on
           aplay-901     [002] .....   100.600000: clk_prepare: LPASS_HW_DCODEC
           aplay-901     [002] .....   100.600100: qcom_glink_cmd_tx_data: tx remote: lpass channel: apr_audio_svc[3/9] iid: 0 chunk_size: 32 left_size: 0 cont: 0
           aplay-901     [002] .....   100.601000: clk_prepare: LPASS_CLK_ID_RX_CORE_MCLK
           aplay-901     [002] .....   100.601100: regmap_cache_only: a600000.codec flag=0
           aplay-901     [002] .....   100.601200: regmap_reg_write: a600000.codec reg=100 val=1
           aplay-901     [002] .....   100.601300: regmap_reg_write: a600000.codec reg=108 val=1
           aplay-901     [002] .....   100.601400: regmap_reg_write: a6a9000.clock-controller reg=98 val=2
           aplay-901     [002] .....   100.601900: regmap_reg_write: a6a9000.clock-controller reg=98 val=0
           aplay-901     [002] .....   100.602000: regmap_reg_write: a620000.codec reg=8 val=1
           aplay-901     [002] .....   100.602100: regmap_reg_write: a7e0000.clock-controller reg=c100 val=2
           aplay-901     [002] .....   100.602600: regmap_reg_write: a7e0000.clock-controller reg=c100 val=0
           aplay-901     [002] .....   100.603000: regmap_reg_read: sdw:0:0:0217:010a:00:3 reg=34c0 val=1
           aplay-901     [002] .....   100.610000: regmap_reg_write: sdw:0:0:0217:010a:00:3 reg=3005 val=40
           aplay-901     [002] .....   100.611000: qcom_glink_cmd_tx_data: tx remote: lpass channel: apr_audio_svc[3/9] iid: 0 chunk_size: 84 left_size: 0 cont: 0
           aplay-901     [002] .....   100.612000: qcom_glink_cmd_tx_data: tx remote: lpass channel: apr_audio_svc[3/9] iid: 0 chunk_size: 24 left_size: 0 cont: 0
           aplay-901     [002] .....   100.613000: snd_soc_dapm_start: card=F(x)tec Pro1X event=1
           aplay-901     [002] .....   100.613100: snd_soc_dapm_widget_power: widget=HPHL PGA val=1
           aplay-901     [002] .....   100.614000: regmap_reg_write: sdw:0:0:0217:010a:00:3 reg=3009 val=80
           aplay-901     [002] .....   100.634000: regmap_reg_write: sdw:0:0:0217:010a:00:3 reg=3009 val=c0
           aplay-901     [002] .....   100.640000: gpio_value: 597 set 1
           aplay-901     [002] .....   100.640100: regmap_reg_write: 1-0059 reg=1 val=e
           aplay-901     [002] .....   100.640200: regmap_reg_write: 9999.unknown reg=1 val=1
           aplay-901     [002] .....   100.650000: qcom_glink_cmd_tx_data: tx remote: lpass channel: apr_audio_svc[3/9] iid: 0 chunk_size: 52 left_size: 0 cont: 0
            bash-900     [001] .....   103.600000: tracing_mark_write: pro1x-audio: hph stream off (exit 0)
     kworker/1:1-50      [001] .....   108.600000: regmap_reg_write: sdw:0:0:0217:010a:00:3 reg=3009 val=0
     kworker/1:1-50      [001] .....   108.601000: gpio_value: 597 set 0
     kworker/1:1-50      [001] .....   111.600000: regmap_cache_only: a600000.codec flag=1
     kworker/1:1-50      [001] .....   111.601000: clk_unprepare: LPASS_CLK_ID_RX_CORE_MCLK
     kworker/1:1-50      [001] .....   111.602000: clk_unprepare: LPASS_HW_DCODEC
            bash-900     [001] .....   112.600000: tracing_mark_write: pro1x-audio: hph end (...)
--- state (hph, 1 s into the sound)
>>> runtime pm
>>> /sys/kernel/debug/regmap/a600000.codec/registers
100: 00000001
108: 00000001
>>> /sys/kernel/debug/regmap/sdw:0:0:0217:010a:00:3/registers
3005: 40
3009: c0
>>> /sys/kernel/debug/soundwire/master-1-0/sdw:1:0:0217:010a:00:4/registers
Register  Value

DP1
  2	 0
Bank0
 120	 3
 122	 3
Bank1
 130	 0
DP2
Bank0
 220	 1
>>> clk_summary (audio)
    LPASS_CLK_ID_RX_CORE_MCLK         1       1        0        19200000    0          0     50000      Y      deviceless no_connection_id
--- controls off
=== scenario mic
--- trace
            bash-900     [001] .....   200.500000: tracing_mark_write: pro1x-audio: mic stream on
         arecord-950     [002] .....   200.600000: clk_prepare: LPASS_CLK_ID_TX_CORE_MCLK
         arecord-950     [002] .....   200.601000: regmap_reg_write: a730000.codec reg=0 val=1
         arecord-950     [002] .....   200.602000: qcom_glink_cmd_tx_data: tx remote: lpass channel: apr_audio_svc[3/9] iid: 0 chunk_size: 84 left_size: 0 cont: 0
         arecord-950     [002] .....   200.603000: qcom_glink_cmd_tx_data: tx remote: lpass channel: apr_audio_svc[3/9] iid: 0 chunk_size: 24 left_size: 0 cont: 0
         arecord-950     [002] .....   200.604000: regmap_reg_write: a730000.codec reg=400 val=10
            bash-900     [001] .....   203.600000: tracing_mark_write: pro1x-audio: mic stream off (exit 0)
     kworker/1:1-50      [001] .....   206.000000: regmap_reg_write: a730000.codec reg=400 val=0
     kworker/1:1-50      [001] .....   207.000000: clk_unprepare: LPASS_CLK_ID_TX_CORE_MCLK
            bash-900     [001] .....   208.600000: tracing_mark_write: pro1x-audio: mic end
"""


def selftest():
    import tempfile
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    base = '[power]\nrail l9 on\n\n[pins]\nmark pins\n\n[volume]\nw rxmacro 0x414 $vol_db\n'
    out, sec, notes = generate(SAMPLE, base, 'SAMPLE')
    fails = []

    def check(c, what):
        if not c:
            fails.append(what)

    rx = sec['rx-up']
    check(rx[0] == 'vote LPASS_HW_DCODEC on' and rx[1].startswith('call pins'),
          'DCODEC vote, then the pins')
    check(any(l == 'clock LPASS_CLK_ID_RX_CORE_MCLK 19200000' for l in rx),
          'the clock with the summary\'s rate')
    i_reset = rx.index('w lpass_audiocc 0x98 0x0')
    check(rx[i_reset + 1].startswith('swr init swr1') and
          rx[i_reset + 2] == 'swr enum swr1 4', 'swr1 after its CGCR reset')
    i0 = rx.index('w lpasscc 0xc100 0x0')
    check(rx[i0 + 1].startswith('swr init swr0') and
          rx[i0 + 2] == 'swr enum swr0 3' and
          rx[i0 + 3].startswith('swr wcd-init'), 'swr0 and the WCD init')
    check('# r wcd 0x34c0 = 0x1' in rx, 'reads as comments')
    check(rx[-1] == 'w wcd 0x3005 0x40', 'the back end ends before the AFE')
    check(any(l in ('delay 7', 'delay 8') for l in rx), 'a gap as a delay')
    hu = sec['hph-up']
    check(hu[0] == '# apr tx 84', 'the AFE configuration as a comment')
    check(hu[1].startswith('swr stream swr1 on 1:0x3 2:0x1') and
          hu[2] == 'afe start', 'the stream, then the AFE start: %r' % hu[:3])
    check('delay 20' in hu, 'the PA settling delay')
    check('gpio 85 1' in hu and 'w aw0 0x1 0xe' in hu, 'the amplifier')
    check(not any('chunk_size' in l or 'apr tx 52' in l for l in hu),
          'no data packets')
    check(any('9999.unknown' in n for n in notes), 'an unknown device noted')
    hd = sec['hph-down']
    check(hd[0].startswith('swr stream swr1 off') and 'w wcd 0x3009 0x0' in hd
          and 'gpio 85 0' in hd, 'the route down')
    check([l for l in sec['rx-down'] if not l.startswith(('#', 'delay'))] ==
          ['clock LPASS_CLK_ID_RX_CORE_MCLK 0', 'vote LPASS_HW_DCODEC off'],
          'the back end down: %r' % sec['rx-down'])
    chk = sec['check-hph']
    check('r rxmacro 0x100 0x1' in chk and 'r wcd 0x3009 0xc0' in chk,
          'the check against the snapshot')
    mu = sec['mic-up']
    check(mu[1] == 'afe start' and 'w vamacro 0x400 0x10' in mu and
          not any(l.startswith('swr stream') for l in mu), 'mic: no stream')
    check(sec['va-up'][0] == 'clock LPASS_CLK_ID_TX_CORE_MCLK 19200000',
          'the VA clock\'s default rate')
    check('[power]' in out and out.index('[power]') < out.index('[rx-up]'),
          'the base first')
    # the daemon's parser takes it: write it, parse it with a minimal check
    with tempfile.NamedTemporaryFile('w', suffix='.seq', delete=False) as f:
        f.write(out)
        path = f.name
    names = re.findall(r'^\[([^\]]+)\]', out, re.M)
    check(len(names) == len(set(names)), 'no section twice')
    os.unlink(path)
    if fails:
        for f in fails:
            print('FAIL trace2seq: ' + f)
        print(out)
        return 1
    print('trace2seq: done (%d sections)' % len(names))
    return 0


def main(argv):
    if len(argv) > 1 and argv[1] == '--selftest':
        return selftest()
    if len(argv) > 1 and argv[1] == '--sample':
        # the self test's trace with the real base, for the C parser
        here = os.path.dirname(os.path.abspath(__file__))
        with open(os.path.join(here, '..', 'seq', 'base.seq')) as f:
            sys.stdout.write(generate(SAMPLE, f.read(), 'SAMPLE')[0])
        return 0
    args = argv[1:]
    out_path = None
    here = os.path.dirname(os.path.abspath(__file__))
    base_path = os.path.join(here, '..', 'seq', 'base.seq')
    trace = None
    while args:
        a = args.pop(0)
        if a == '-o':
            out_path = args.pop(0)
        elif a == '-b':
            base_path = args.pop(0)
        else:
            trace = a
    if trace is None:
        print(__doc__)
        return 2
    with open(trace, errors='replace') as f:
        text = f.read()
    base = ''
    if base_path and os.path.exists(base_path):
        with open(base_path) as f:
            base = f.read()
    out, sec, notes = generate(text, base, os.path.basename(trace))
    if out_path:
        with open(out_path, 'w') as f:
            f.write(out)
    else:
        sys.stdout.write(out)
    for n in notes:
        print('note: ' + n, file=sys.stderr)
    print('%d sections: %s' % (len(sec), ' '.join(sorted(sec))),
          file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
