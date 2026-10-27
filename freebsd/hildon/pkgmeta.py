#!/usr/bin/env python3
"""FreeBSD package metadata for the parts build.sh installed into
$WORK/hildon/pkgroot/<part>: per package a directory with +MANIFEST (the
fields pkg-create(8) takes with -M), plist and root/ (the files), for
package.sh to turn into packages with FreeBSD's own pkg create.

    pkgmeta.py HILDON_DIR PACKAGESITE_YAML OUT_DIR

Name: the part's, unless FreeBSD has one of that name (then <name>-maemo,
conflicting with it); gtk becomes FreeBSD's gtk2 with Leste's libraries
over FreeBSD's package, at a higher version, since everything built here
needs Leste's GTK. Version, description and licence come from the
part's debian/ directory (or the Leste package's control file for the
data parts). Dependencies come from the ELF files' DT_NEEDED: a library
of another part, of a FreeBSD package (packagesite.yaml) or of the base
system (no dependency).
"""
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile

H, SITE, OUT = sys.argv[1:4]
ROOTS = os.path.join(H, 'pkgroot')
SRC = os.path.join(H, 'src')
SYSROOT = os.path.join(H, 'sysroot')
CACHE = os.path.join(os.path.dirname(os.path.dirname(SITE)), 'cache')
RENAME = {'gtk': 'gtk2'}
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOCAL = {
    'pro1x-hidpi': ('1.0', "Hildon at the N900's size on high-resolution screens",
                    'pro1x-theme-scale scales a Hildon theme (images, gtkrc lengths, '
                    'matchbox decorations) by one factor; pro1x-hidpi makes the '
                    "theme for the configured factor and the session's environment "
                    '(tools/pro1x-hidpi).', ['UNKNOWN']),
    'pro1x-steps': ('1.0', "The Pro1X's hardware step counter on D-Bus",
                    "pro1x-steps reads the ADSP's pedometer through libssc and "
                    'serves the count as org.pro1x.Steps on the system bus '
                    '(tools/pro1x-steps), with its rc.d script.', ['UNKNOWN']),
    'pro1x-gnss': ('1.0', "The Pro1X modem's GNSS as NMEA for gpsd",
                   "pro1x-gnss starts the modem's location service over QMI and "
                   'writes its NMEA sentences for gpsd (tools/pro1x-gnss).',
                   ['UNKNOWN']),
    'pro1x-proximity': ('1.0', "The Pro1X's proximity sensor on D-Bus, while watched",
                        "pro1x-proximity reads the ADSP's proximity sensor through "
                        'libssc while a client watches it and serves it as '
                        'org.pro1x.Proximity on the system bus (tools/pro1x-proximity), '
                        'with its rc.d script.', ['UNKNOWN']),
    'pro1x-standby': ('1.0', "The Nokia N9's stand-by screen as the X screen saver",
                      'pro1x-standby shows the time, unread notifications and the '
                      "charging state in the style of the N9's low power mode when "
                      'X blanks the screen, the rest black for the AMOLED panel '
                      "(tools/pro1x-standby); Nokia's icons and font are not "
                      'included.', ['UNKNOWN']),
}
# Parts from a release tarball (build.sh fetch_src): version, comment,
# description, licences, home page
TARBALL = {
    'ffmpeg8-libs': ('8.1.2', "FFmpeg 8.1's libraries, for FreeBSD 15.1's QtWebEngine",
                     "libavcodec, libavformat, libavutil and libswresample of FFmpeg "
                     "8.1 (LGPL parts only), which the qt5-webengine package of FreeBSD "
                     "15.1 (release_1) links; beside FFmpeg 9 of FreeBSD's quarterly "
                     "packages, without headers.", ['LGPL21+'], 'https://ffmpeg.org/'),
    'qt5-webkit': ('5.212.0.a4', 'QtWebKit with a more modern WebKit code base',
                   "QtWebKit 5.212.0-alpha4, the last release of the Qt port of WebKit, "
                   "with the patches of FreeBSD's port and Arch's: WebKit 1 (QWebView, "
                   "QtWebKitWidgets) for the Maemo apps; without WebKit 2, video, "
                   "geolocation and XSLT.", ['LGPL21'], 'https://github.com/qtwebkit/qtwebkit'),
}
# Parts from git without a debian/ directory (the DSPs' userland from
# Linux): version (git describe), comment, description, licences, and the
# licence logic if more than one
UPSTREAM = {
    'hexagonrpc': ('0.5.0.24', "FastRPC file server for the Qualcomm DSPs",
                   "hexagonrpcd serves a Hexagon DSP the files it asks for over "
                   "FastRPC: the ADSP its sensor configuration and registry. With "
                   "the sensor registry interface the SM6115's ADSP needs.",
                   ['GPLv3+']),
    'libqrtr-glib': ('1.4.0', 'QRTR sockets for GLib',
                     "libqrtr-glib, the Qualcomm IPC router (QRTR) for GLib "
                     "programs: libqmi's transport to the DSPs' QMI services.",
                     ['LGPL21+']),
    'libqmi': ('1.34.0', 'QMI protocol library and qmicli',
               "libqmi-glib and qmicli as Leste builds them (1.34), over QRTR; "
               "without udev, RMNET links, QMI over MBIM and the firmware updater.",
               ['LGPL21+', 'GPLv2+'], 'multi'),
    'libssc': ('0.4.4', 'Qualcomm Sensor Core client library',
               "libssc reads the sensors of a Qualcomm DSP (accelerometer, light, "
               "proximity, the step counter) over QMI, with Leste's two patches.",
               ['GPLv3+']),
    'qrtr': ('1.2.18', 'Qualcomm IPC router library and lookup tool',
             "libqrtr, QRTR sockets and QMI encoding for C programs, and "
             "qrtr-lookup, which lists the services of the name service.",
             ['BSD3CLAUSE']),
    'rmtfs': ('1.3', "Qualcomm modem's remote file system service",
              "rmtfs gives the modem its file systems (modemst1, modemst2, fsg, "
              "fsc) from the GPT partitions through the shared memory of "
              "qcom_rmtfs, read-only as on Leste.", ['BSD3CLAUSE']),
    'tqftpserv': ('1.3.2', 'TFTP over QRTR for the Qualcomm DSPs',
                  "tqftpserv serves the DSPs files over QRTR's TFTP: the modem "
                  "its configuration and the Wi-Fi's firmware (wlanmdsp.mbn).",
                  ['BSD3CLAUSE']),
    'pd-mapper': ('1.1', 'Qualcomm protection domain mapper',
                  "pd-mapper tells the DSPs which of them serves which protection "
                  "domain, from the firmware's .jsn files.", ['BSD3CLAUSE']),
}
MAINTAINER = 'sebastian.matkovich@gmail.com'
# What a part needs besides the libraries it links: interpreters, the
# modules it loads, the session around it
RUNTIME = {
    'hildon-desktop': ['hildon-home', 'hildon-status-menu', 'sapwood',
                       'hildon-theme-alpha', 'osso-icons-default', 'osso-sounds-ui'],
    'gpodder-leste': ['python312', 'pyotherside-qt5', 'qt5-declarative',
                      'qt5-quickcontrols', 'qt5-quickcontrols2', 'qt5-graphicaleffects'],
    'pro1x-hidpi': ['xrdb'],
    'wifi-assistant': ['curl'],
    'pro1x-steps': ['hexagonrpc', 'dbus'],
    'pro1x-proximity': ['hexagonrpc', 'dbus'],
    'pro1x-gnss': ['gpsd'],
    # the modem asks for its files and domains before its file systems
    'rmtfs': ['tqftpserv', 'pd-mapper'],
    # QtSql's SQLite driver, a package of its own on FreeBSD
    'jib': ['qt5-sqldrivers-sqlite3'],
    'dorian': ['qt5-sqldrivers-sqlite3'],
}
# Where debian/copyright says only "GPL" (any version) or nothing
LICENSES = {
    'hildon-im-vkbrenderer3': ['GPLv2+'],
    'hildon-input-method-plugins': ['GPLv2+'],
    'osso-mahjong': ['GPLv2+'],
}
# Files FreeBSD's packages own already (and what the part then needs)
EXCLUDE = {
    'osso-icons-default': ([], ['hicolor-icon-theme']),
    # FreeBSD has gtk-update-icon-cache in a package of its own; the API
    # documentation stays out; FreeBSD's libraries (2.24.33) under Leste's
    # (2.24.25 with the Hildon changes), which the links now point to
    'gtk': (['usr/local/bin/gtk-update-icon-cache', 'usr/local/share/man/man1/gtk-update-icon-cache.1*',
             'usr/local/share/gtk-doc/*', 'usr/local/lib/libgtk-x11-2.0.so.0.2400.33',
             'usr/local/lib/libgdk-x11-2.0.so.0.2400.33'], ['gtk-update-icon-cache']),
}
# Packages of this repository without a part: the Hildon session for any
# FreeBSD (freebsd/hildon/rootfs) and one to install everything
SESSION_DEPS = ['hildon-desktop', 'pro1x-hidpi', 'gconf2', 'dbus', 'xrdb', 'xset']

site = {}
provider = {}
for line in open(SITE):
    d = json.loads(line)
    site[d['name']] = d
    for so in d.get('shlibs_provided', []):
        provider.setdefault(so, []).append(d['name'])
# What the sysroot was made of: the preferred provider of a library
in_sysroot = set()
wanted = os.path.join(H, 'sysroot.list')
if os.path.exists(wanted):
    for line in open(wanted):
        m = re.match(r'(.*)-[^-]+~\d+\$\w+\.pkg$', os.path.basename(line.strip()))
        if m:
            in_sysroot.add(m.group(1))
base_libs = set()
for d in ('lib', 'usr/lib'):
    p = os.path.join(SYSROOT, d)
    if os.path.isdir(p):
        base_libs.update(f for f in os.listdir(p) if '.so' in f)


def readelf(path):
    try:
        out = subprocess.run(['llvm-readelf', '-d', path], capture_output=True,
                             text=True).stdout
    except OSError:
        return None, []
    soname = re.findall(r'\(SONAME\)\s+Library soname: \[(.*)\]', out)
    needed = re.findall(r'\(NEEDED\)\s+Shared library: \[(.*)\]', out)
    return (soname[0] if soname else None), needed


def is_elf(path):
    try:
        with open(path, 'rb') as f:
            return f.read(4) == b'\x7fELF'
    except OSError:
        return False


# FreeBSD-side changes to a part at the same upstream version (patches,
# build.sh), like a port's PORTREVISION: version_N, so that pkg upgrade
# takes the new package. Raise a part's number with each such change.
REVISION = {
    # 2026-10-27: clock-ui ran no more than its start (time zone, paths,
    # missing alarmd), apps' data moved from /usr/share to /usr/local/share
    'cityinfo': 1, 'clockd': 1, 'clock-ui': 1,
    'easylist': 1, 'flappybird': 1, 'hamsterfiler': 1, 'maepad': 1,
    'mihphoto': 1, 'osso-mahjong': 1, 'profilesx': 1, 'qshot': 1,
}


def version_of(text):
    """Debian version -> FreeBSD's: no epoch (nothing to be newer than),
    no - + ~ : (package file names stay plain, for download links)"""
    if ':' in text:
        text = text.split(':', 1)[1]
    v = re.sub(r'[^0-9A-Za-z.]', '.', text)
    return re.sub(r'\.+', '.', v).strip('.') or '0'


def stanzas(text):
    out = []
    for block in re.split(r'\n\s*\n', text):
        d, key = {}, None
        for line in block.split('\n'):
            m = re.match(r'([A-Za-z-]+):\s?(.*)', line)
            if m and not line.startswith(' '):
                key = m.group(1)
                d[key] = m.group(2)
            elif key and line.startswith(' '):
                d[key] += '\n' + line[1:]
        if d:
            out.append(d)
    return out


def describe(control, part):
    """comment and desc of the part's main binary package"""
    st = stanzas(control)
    bins = [s for s in st if 'Package' in s]
    main = None
    for s in bins:
        if s['Package'] == part:
            main = s
    if main is None:
        for s in bins:
            if not re.search(r'-(dev|dbg|doc|dbgsym|l10n.*)$', s['Package']):
                main = s
                break
    if main is None and bins:
        main = bins[0]
    desc = (main or {}).get('Description', part)
    first, _, rest = desc.partition('\n')
    rest = '\n'.join('' if l.strip() == '.' else l.strip()
                     for l in rest.split('\n')).strip()
    return first.strip() or part, rest or first.strip() or part


SPDX = {
    'GPL-2': 'GPLv2', 'GPL-2+': 'GPLv2+', 'GPL-2.0': 'GPLv2', 'GPL-2.0+': 'GPLv2+',
    'GPL-2.0-only': 'GPLv2', 'GPL-2.0-or-later': 'GPLv2+',
    'GPL-3': 'GPLv3', 'GPL-3+': 'GPLv3+', 'GPL-3.0': 'GPLv3', 'GPL-3.0+': 'GPLv3+',
    'GPL-3.0-only': 'GPLv3', 'GPL-3.0-or-later': 'GPLv3+',
    'LGPL-2': 'LGPL20', 'LGPL-2+': 'LGPL20+', 'LGPL-2.1': 'LGPL21', 'LGPL-2.1+': 'LGPL21+',
    'LGPL-2.1-only': 'LGPL21', 'LGPL-2.1-or-later': 'LGPL21+',
    'LGPL-3': 'LGPL3', 'LGPL-3+': 'LGPL3+', 'MIT': 'MIT', 'Expat': 'MIT',
    'BSD-2-clause': 'BSD2CLAUSE', 'BSD-3-clause': 'BSD3CLAUSE', 'BSD-2-Clause': 'BSD2CLAUSE',
    'BSD-3-Clause': 'BSD3CLAUSE', 'CC-BY-SA-3.0': 'CC-BY-SA-3.0',
}


def statement(t):
    """The licence a free-text notice grants: 'terms of the GNU ...'"""
    t = re.sub(r'\s+', ' ', t)
    m = re.search(r'(?:terms|conditions) of (?:the )?GNU (Lesser |Library )?General Public License', t, re.I)
    if m:
        tail = t[m.end():m.end() + 300]
        later = re.search(r'any later version', tail, re.I)
        v = re.search(r'version (\d(?:\.\d)?)', tail, re.I)
        v = v.group(1) if v else ('2.1' if m.group(1) and m.group(1).startswith('Lesser') else
                                  '2' if m.group(1) else '2')
        if m.group(1):
            name = {'2': 'LGPL20', '2.0': 'LGPL20', '2.1': 'LGPL21', '3': 'LGPL3'}.get(v, 'LGPL21')
        else:
            name = {'2': 'GPLv2', '3': 'GPLv3'}.get(v[0], 'GPLv2')
        return [name + ('+' if later else '')]
    if re.search(r'Permission is hereby granted, free of charge', t):
        return ['MIT']
    if re.search(r'Redistribution and use in source and binary forms', t):
        return ['BSD3CLAUSE' if re.search(r'neither the name', t, re.I) else 'BSD2CLAUSE']
    return []


def header(t):
    """The licence a full licence text (COPYING) is"""
    h = re.sub(r'\s+', ' ', t[:400])
    if re.search(r'LESSER GENERAL PUBLIC LICENSE Version 2\.1', h, re.I):
        return ['LGPL21']
    if re.search(r'LIBRARY GENERAL PUBLIC LICENSE Version 2', h, re.I):
        return ['LGPL20']
    if re.search(r'LESSER GENERAL PUBLIC LICENSE Version 3', h, re.I):
        return ['LGPL3']
    if re.search(r'GENERAL PUBLIC LICENSE Version 3', h, re.I):
        return ['GPLv3']
    if re.search(r'GENERAL PUBLIC LICENSE Version 2', h, re.I):
        return ['GPLv2']
    return statement(t)


def licences(copyright, copying):
    """debian/copyright's machine-readable Files: * licence, else its
    notice, else the licence text the source carries"""
    for st in stanzas(copyright):
        if st.get('Files', '').strip() == '*' and 'License' in st and \
                not st['License'].startswith('<'):    # dh_make's template
            first = st['License'].split('\n')[0].strip()
            names = [SPDX.get(x.strip(), x.strip()) for x in re.split(r'\s+or\s+|\s+and\s+', first) if x.strip()]
            if names:
                return names
    return statement(copyright) or next((header(c) for c in copying if header(c)), []) or ['UNKNOWN']


def read(path):
    try:
        return open(path, errors='replace').read()
    except OSError:
        return ''


# --- the parts -------------------------------------------------------------
parts = sorted(p for p in os.listdir(ROOTS)
               if os.path.isdir(os.path.join(ROOTS, p)))
meta = {}
for part in parts:
    root = os.path.join(ROOTS, part)
    meta_extra = {}
    name = RENAME.get(part, part)
    conflicts = []
    if name in site and part not in RENAME:
        conflicts = [name + '-*']
        name = name + '-maemo'
    src = os.path.join(SRC, part)
    url = read(root + '.url').strip()
    rev = read(root + '.rev').strip()
    if os.path.isdir(os.path.join(src, 'debian')):
        control = read(os.path.join(src, 'debian/control'))
        cl = read(os.path.join(src, 'debian/changelog'))
        m = re.match(r'\S+ \(([^)]+)\)', cl)
        version = version_of(m.group(1) if m else '0')
        comment, desc = describe(control, part)
        lic = licences(read(os.path.join(src, 'debian/copyright')),
                       [read(os.path.join(src, f)) for f in
                        ('COPYING', 'LICENSE', 'COPYING.LIB', 'LICENSE.txt')])
        www = re.sub(r'\.git$', '', url) or 'https://github.com/maemo-leste'
    elif part.startswith('leste-l10n-'):
        # Leste's *-l10n-<language> packages of one index, together
        lang = {'en': 'English', 'de': 'German'}.get(part.rsplit('-', 1)[1], part)
        rel = read(os.path.join(H, 'leste', 'Release'))
        m_ = re.search(r'^Date: \w+, (\d+) (\w+) (\d+)', rel, re.M)
        months = 'Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec'.split()
        version = ('%s.%02d.%02d' % (m_.group(3), months.index(m_.group(2)) + 1,
                                     int(m_.group(1)))) if m_ else '0'
        n = len([l for l in read(root + '.rev').splitlines() if l.strip()])
        comment = "Maemo Leste's user interface texts (%s)" % lang
        desc = ('The %s translations of the Hildon desktop and its apps: %d of '
                "Leste's *-l10n packages, as in its archive on %s." % (lang, n, version))
        lic = ['UNKNOWN']
        www = 'https://maedevu.maemo.org/leste'
        url = www
    elif os.path.exists(root + '.manifest'):
        # FreeBSD's own package of another package set (build.sh: "pkg"),
        # as FreeBSD describes it
        fm = json.loads(read(root + '.manifest'))
        version, comment, desc = fm['version'], fm['comment'], fm['desc']
        lic, www = fm.get('licenses') or ['UNKNOWN'], fm.get('www', url)
        meta_extra = {'origin': fm['origin'], 'logic': fm.get('licenselogic', 'single'),
                      'scripts': fm.get('scripts')}
    elif part in TARBALL:
        version, comment, desc, lic, www = TARBALL[part]
    elif part in UPSTREAM:
        version, comment, desc, lic = UPSTREAM[part][:4]
        if len(UPSTREAM[part]) > 4:
            meta_extra = {'logic': UPSTREAM[part][4]}
        www = re.sub(r'\.git$', '', url)
    elif part in LOCAL:
        # this repository's own (tools/)
        version, comment, desc, lic = LOCAL[part]
        www = 'https://github.com/smatkovi/pro1x-maemo-leste'
        url = www
        rev = subprocess.run(['git', '-C', REPO, 'rev-parse', 'HEAD'],
                             capture_output=True, text=True).stdout.strip()
    else:
        # a data package from Leste's archive (build.sh leste_data)
        ctl = stanzas(read(os.path.join(H, 'leste', part + '.control')))
        c = ctl[0] if ctl else {}
        version = version_of(c.get('Version', '0'))
        comment, desc = describe(read(os.path.join(H, 'leste', part + '.control')), part)
        deb = rev.split()[1] if rev.startswith('deb ') else ''
        cr = ''
        if deb:
            try:
                out = subprocess.run(
                    'bsdtar -xOf %s "data.tar.*" | bsdtar -xOf - "./usr/share/doc/%s/copyright"'
                    % (os.path.join(H, 'leste', deb), part), shell=True,
                    capture_output=True, text=True).stdout
                cr = out
            except OSError:
                pass
        lic = licences(cr, [])
        www = 'https://maedevu.maemo.org/leste'
    lic = LICENSES.get(part, lic)
    if part in REVISION:
        version += '_%d' % REVISION[part]
    meta[part] = dict(name=name, version=version, comment=comment, desc=desc,
                      licenses=lic, www=www, url=url, rev=rev, root=root,
                      conflicts=conflicts)
    meta[part].update(meta_extra)

# Leste's GTK as FreeBSD's gtk2, one step above FreeBSD's version
if 'gtk' in meta:
    g = site['gtk2']
    m = meta['gtk']
    m['version'] = g['version'].replace('_', '.1_', 1) if '_' in g['version'] \
        else g['version'] + '.1'
    m['origin'] = g['origin']
    m['comment'] = g['comment'] + " (Maemo Leste's Hildon GTK)"
    m['licenses'] = ['LGPL20']

# Libraries each part provides
ours = {}
for part, m in meta.items():
    for dp, _, fs in os.walk(m['root']):
        for f in fs:
            p = os.path.join(dp, f)
            if '.so' in f and not os.path.islink(p) and is_elf(p):
                so, _ = readelf(p)
                ours.setdefault(so or f, part)
            if '.so' in f:
                ours.setdefault(f, part)

if os.path.isdir(OUT):
    shutil.rmtree(OUT)
os.makedirs(OUT)
unresolved = {}
for part, m in meta.items():
    d = os.path.join(OUT, m['name'])
    r = os.path.join(d, 'root')
    os.makedirs(d)
    deps = {}
    extra_files = []
    if part == 'gtk':
        # FreeBSD's gtk2 under Leste's libraries
        g = site['gtk2']
        pk = [f for f in os.listdir(CACHE) if f.startswith('gtk2-%s~' % g['version'])]
        os.makedirs(r)
        with tarfile.open(os.path.join(CACHE, pk[0])) as t:
            for ti in t.getmembers():
                if ti.name.startswith('+'):
                    continue
                ti.name = ti.name.lstrip('/')
                t.extract(ti, r, filter='tar')
        for k, v in g.get('deps', {}).items():
            deps[k] = {'origin': v['origin'], 'version': v['version']}
    os.makedirs(r, exist_ok=True)
    subprocess.run(['cp', '-a', '--remove-destination', m['root'] + '/.', r + '/'],
                   check=True)
    if part.startswith('leste-l10n-'):
        # texts a part ships itself (libhildon's en_GB, ...) stay with it
        for dp, _, fs in os.walk(r):
            for f in fs:
                rel = os.path.relpath(os.path.join(dp, f), r)
                if any(os.path.lexists(os.path.join(o['root'], rel))
                       for q, o in meta.items() if q != part and not q.startswith('_')):
                    os.remove(os.path.join(dp, f))
    for x in EXCLUDE.get(part, ([], []))[0]:
        for hit in glob.glob(os.path.join(r, x)):
            if os.path.isdir(hit) and not os.path.islink(hit):
                shutil.rmtree(hit)
            else:
                os.remove(hit)
    RUNTIME.setdefault(part, []).extend(EXCLUDE.get(part, ([], []))[1])
    plist = []
    needed = set()
    provided = set()
    for dp, dns, fs in os.walk(r):
        for f in sorted(fs):
            p = os.path.join(dp, f)
            rel = '/' + os.path.relpath(p, r)
            plist.append(rel[len('/usr/local/'):] if rel.startswith('/usr/local/') else rel)
            if not os.path.islink(p) and is_elf(p):
                so, nd = readelf(p)
                needed.update(nd)
                if so:
                    provided.add(so)
    for so in sorted(needed - provided):
        if so in ours and ours[so] != part:
            o = meta[ours[so]]
            deps[o['name']] = {'origin': o.get('origin', 'maemo-leste/' + ours[so]),
                               'version': o['version']}
        elif so in ours or so in base_libs:
            # the part's own, or the base system's (libgcc_s, libc++, ...,
            # which some packages also carry: gnat, gcc)
            continue
        elif so in provider:
            cands = provider[so]
            pick = next((c for c in cands if c in in_sysroot), cands[0])
            if pick == 'gtk2' and 'gtk' in meta:
                o = meta['gtk']
                deps['gtk2'] = {'origin': o['origin'], 'version': o['version']}
            else:
                deps[pick] = {'origin': site[pick]['origin'],
                              'version': site[pick]['version']}
        else:
            unresolved.setdefault(m['name'], []).append(so)
    for r_ in RUNTIME.get(part, []):
        o = next((x for x in meta.values() if x['name'] == r_), None)
        if o is not None:
            deps[r_] = {'origin': o.get('origin', 'maemo-leste/' + r_), 'version': o['version']}
        elif r_ in site:
            deps[r_] = {'origin': site[r_]['origin'], 'version': site[r_]['version']}
        else:
            unresolved.setdefault(m['name'], []).append('runtime ' + r_)
    deps.pop(m['name'], None)
    manifest = {
        'name': m['name'],
        'origin': m.get('origin', 'maemo-leste/' + part),
        'version': m['version'],
        'comment': m['comment'][:70],
        'desc': m['desc'] + '\n\nMaemo Leste for FreeBSD (F(x)tec Pro1X port): '
                + (m['url'] or m['www']) + (' at ' + m['rev'][:12] if m['rev']
                                            and not m['rev'].startswith('deb') else ''),
        'maintainer': MAINTAINER,
        'www': m['www'],
        'prefix': '/usr/local',
        'licenselogic': 'single' if len(m['licenses']) == 1 else m.get('logic', 'dual'),
        'licenses': m['licenses'],
        'categories': ['x11'],
        'deps': deps,
        'annotations': {'source': m['url'] or m['www'], 'revision': m['rev']},
    }
    if m.get('scripts'):
        # FreeBSD's own (ldconfig, Qt's qconfig-modules.h)
        manifest['scripts'] = m['scripts']
    with open(os.path.join(d, '+MANIFEST'), 'w') as f:
        json.dump(manifest, f, indent=1)
    with open(os.path.join(d, 'plist'), 'w') as f:
        f.write('\n'.join(plist) + '\n')
    print('%-40s %-22s %-14s %d files, %d deps' % (
        m['name'], m['version'], ','.join(m['licenses']), len(plist), len(deps)))


# --- hildon-session and hildon-meta ---------------------------------------
def dep_on(n):
    o = next((x for x in meta.values() if x['name'] == n), None)
    if o is not None:
        return {'origin': o.get('origin', 'maemo-leste/' + n), 'version': o['version']}
    return {'origin': site[n]['origin'], 'version': site[n]['version']}


rev = subprocess.run(['git', '-C', REPO, 'rev-parse', 'HEAD'],
                     capture_output=True, text=True).stdout.strip()
repo_www = 'https://github.com/smatkovi/pro1x-maemo-leste'
# a step up with every commit of this repository, so that pkg upgrade
# brings hildon-meta's new apps
own_version = '1.0.' + (subprocess.run(['git', '-C', REPO, 'rev-list', '--count', 'HEAD'],
                                       capture_output=True, text=True).stdout.strip() or '0')
# what shows in a menu, the home view, the status area or the control panel
apps = sorted(x['name'] for x in meta.values()
              if any(os.path.isdir(os.path.join(x['root'], d, 'share/applications'))
                     for d in ('usr/local', 'usr')))
# and what works without an entry: the on-screen keyboard, daemons
apps = sorted(set(apps) | {x['name'] for x in meta.values() if x['name'] in (
    'hildon-input-method-plugins', 'headphoned', 'wifi-assistant', 'hildon-status-menu')})
for name, comment, desc, root, deps in (
        ('hildon-session', "Maemo Leste's Hildon session for FreeBSD",
         'hildon-session (startx hildon-session, or "startx hildon" on the Pro1X) '
         'starts hildon-desktop with the status menu and home, as Leste\'s X session '
         'does; rc.d/hildon_setup makes the GConf defaults and the theme for the '
         'HiDPI factor at boot; links for the paths Hildon has built in.',
         os.path.join(REPO, 'freebsd/hildon/rootfs'), SESSION_DEPS),
        ('hildon-meta', "Maemo Leste's Hildon desktop with all its apps on FreeBSD",
         'Installs the Hildon session and every app and plugin built for FreeBSD '
         'from Maemo Leste (and its extras), see freebsd/hildon/README.md.',
         None, ['hildon-session'] + apps)):
    d = os.path.join(OUT, name)
    r = os.path.join(d, 'root')
    os.makedirs(r)
    plist = []
    if root:
        subprocess.run(['cp', '-a', root + '/.', r + '/'], check=True)
        for dp, _, fs in os.walk(r):
            for f in sorted(fs):
                rel = '/' + os.path.relpath(os.path.join(dp, f), r)
                plist.append(rel[len('/usr/local/'):] if rel.startswith('/usr/local/') else rel)
    manifest = {
        'name': name, 'origin': 'pro1x/' + name, 'version': own_version,
        'comment': comment, 'desc': desc, 'maintainer': MAINTAINER,
        'www': repo_www, 'prefix': '/usr/local', 'licenselogic': 'single',
        'licenses': ['UNKNOWN'], 'categories': ['x11'],
        'deps': {n: dep_on(n) for n in deps},
        'annotations': {'source': repo_www, 'revision': rev},
    }
    if name == 'hildon-session':
        manifest['messages'] = [{'message':
            'Hildon needs the system D-Bus: sysrc dbus_enable=YES && service dbus start.\n'
            'Its GConf defaults and theme are made at boot (rc.d/hildon_setup), the first\n'
            'time with: service hildon_setup start. Then: startx /usr/local/bin/hildon-session\n'
            "Leste's GTK (with the Hildon changes) is this repository's gtk2: pkg install -r pro1x gtk2"}]
    with open(os.path.join(d, '+MANIFEST'), 'w') as f:
        json.dump(manifest, f, indent=1)
    with open(os.path.join(d, 'plist'), 'w') as f:
        f.write('\n'.join(plist) + ('\n' if plist else ''))
    meta['_' + name] = {'name': name, 'version': own_version, 'origin': 'pro1x/' + name,
                        'root': r}
    print('%-40s %-22s %-14s %d files, %d deps' % (name, own_version, 'UNKNOWN', len(plist), len(deps)))

for n, sos in sorted(unresolved.items()):
    print('unresolved in %s: %s' % (n, ' '.join(sorted(set(sos)))))
