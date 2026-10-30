#!/usr/bin/env python3
"""A PKGBUILD for one part that arch/43-packages.sh packages: its files
are those in STAGE (the part's own install root), which this tidies for
Arch Linux ARM first, as 42-meego-look.sh does for its parts:

- /bin, /sbin, /lib and every other directory that is a symlink in the
  image (Arch's merged /usr) moved to where the link points;
- libtool archives and generated caches (icon, MIME, desktop, GIO and
  GSettings caches, info's dir) left out, and EXCLUDE's globs;
- a file another package already owns (an Arch Linux ARM package, or one
  made here before) left out and reported: the other package's stays.

Version, description and licence come from the part's debian/ directory
(changelog, control, copyright) unless given; the dependencies are the
packages that own the libraries its programs and libraries link (the
image's ldconfig cache, then any library directory), the interpreters of
its scripts, qt-components where it uses com.nokia.meego, and DEPENDS.
GConf schemas are registered on install, as their packages do on Leste.

    pkgbuild.py ROOT STAGE OUTDIR  (the rest in the environment, below)

ROOT is the image's root as seen from here, STAGE and OUTDIR are paths
below it (OUTDIR gets PKGBUILD and the install script); STAGE_IN and
OUTDIR_IN are the same as seen in the image. Environment: PKGNAME, PART,
SRC (the part's source directory, below ROOT, may be empty), VERSION,
DESC, LICENSE, URL, DEPENDS, OPTDEPENDS, PROVIDES, CONFLICTS, EXCLUDE
(space-separated globs, relative to the root), LDCACHE (ldconfig -p of
the image), COMMIT.
"""
import fnmatch
import os
import re
import shutil
import subprocess
import sys

ROOT, STAGE, OUTDIR = sys.argv[1:4]
STAGE_IN = os.environ['STAGE_IN']
E = os.environ.get
NAME = E('PKGNAME')
PART = E('PART', NAME)
SRC = E('SRC', '')
SRCDIR = os.path.join(ROOT, SRC.lstrip('/')) if SRC else ''


def note(msg):
    print('     ' + msg)


# --- the files ------------------------------------------------------------------
def relfiles(top):
    out = []
    for d, dirs, files in os.walk(top):
        for f in files + [x for x in dirs if os.path.islink(os.path.join(d, x))]:
            out.append(os.path.relpath(os.path.join(d, f), top))
    return sorted(out)


def merge_into(src, dst):
    """src's contents into dst (a directory), then src removed"""
    os.makedirs(dst, exist_ok=True)
    for x in os.listdir(src):
        s, t = os.path.join(src, x), os.path.join(dst, x)
        if os.path.isdir(s) and not os.path.islink(s) and os.path.isdir(t) and not os.path.islink(t):
            merge_into(s, t)
        else:
            if os.path.lexists(t):
                if os.path.isdir(t) and not os.path.islink(t):
                    shutil.rmtree(t)
                else:
                    os.unlink(t)
            os.rename(s, t)
    os.rmdir(src)


def follow_links(stage):
    """directories that are symlinks in the image (/bin, /lib, /usr/sbin,
    /var/run ...): their contents to the links' targets"""
    changed = True
    while changed:
        changed = False
        for d, dirs, _ in os.walk(stage):
            for x in dirs:
                p = os.path.join(d, x)
                if os.path.islink(p):
                    continue
                rel = os.path.relpath(p, stage)
                img = os.path.join(ROOT, rel)
                if os.path.islink(img):
                    target = os.readlink(img)
                    if os.path.isabs(target):
                        trel = target.lstrip('/')
                    else:
                        trel = os.path.normpath(os.path.join(os.path.dirname(rel), target))
                    merge_into(p, os.path.join(stage, trel))
                    changed = True
                    break
            if changed:
                break


GENERATED = ['usr/share/info/dir', 'usr/share/applications/mimeinfo.cache',
             'usr/share/icons/*/icon-theme.cache', 'usr/share/glib-2.0/schemas/gschemas.compiled',
             'usr/lib/gio/modules/giomodule.cache', 'usr/lib/gdk-pixbuf-2.0/*/loaders.cache',
             '*.la']
MIME_KEEP = 'usr/share/mime/packages/'


def owners():
    """path (relative) -> package, from the image's pacman database"""
    m = {}
    db = os.path.join(ROOT, 'var/lib/pacman/local')
    for e in os.listdir(db):
        name = None
        try:
            with open(os.path.join(db, e, 'desc')) as f:
                lines = f.read().split('\n')
            name = lines[lines.index('%NAME%') + 1]
            with open(os.path.join(db, e, 'files')) as f:
                files = f.read().split('\n')
        except (OSError, ValueError, IndexError):
            continue
        if '%FILES%' not in files:
            continue
        for p in files[files.index('%FILES%') + 1:]:
            if not p:
                break
            if not p.endswith('/'):
                m[p] = name
    return m


def tidy(stage, own):
    follow_links(stage)
    excl = (E('EXCLUDE') or '').split()
    dropped = []
    for rel in relfiles(stage):
        p = os.path.join(stage, rel)
        why = None
        if any(fnmatch.fnmatch(rel, g) or fnmatch.fnmatch(os.path.basename(rel), g) for g in GENERATED):
            why = 'generated'
        elif rel.startswith('usr/share/mime/') and not rel.startswith(MIME_KEEP):
            why = 'generated'
        elif any(fnmatch.fnmatch(rel, g) or rel.startswith(g.rstrip('/') + '/') for g in excl):
            why = 'excluded'
        elif rel in own and own[rel] != NAME:
            why = 'owned by ' + own[rel]
        if why:
            os.unlink(p)
            if why != 'generated':
                dropped.append((rel, why))
    # empty directories left behind
    for d, dirs, files in sorted(os.walk(stage), key=lambda x: -len(x[0])):
        if d != stage and not os.listdir(d):
            os.rmdir(d)
    for rel, why in dropped[:15]:
        note('left out: /%s (%s)' % (rel, why))
    if len(dropped) > 15:
        note('left out: %d more' % (len(dropped) - 15))


# --- version, description, licence ----------------------------------------------
def changelog_version():
    try:
        with open(os.path.join(SRCDIR, 'debian/changelog'), errors='replace') as f:
            m = re.match(r'\S+ \(([^)]+)\)', f.readline())
        return m.group(1) if m else None
    except OSError:
        return None


def arch_version(v):
    v = v.split(':', 1)[-1]
    return re.sub(r'[^A-Za-z0-9.+_~]', '_', v.replace('-', '_'))


def control_desc():
    try:
        with open(os.path.join(SRCDIR, 'debian/control'), errors='replace') as f:
            text = f.read()
    except OSError:
        return None
    stanzas = [s for s in re.split(r'\n\s*\n', text) if re.search(r'^Package:', s, re.M)]
    best = None
    for s in stanzas:
        pkg = re.search(r'^Package:\s*(\S+)', s, re.M).group(1)
        d = re.search(r'^Description:\s*(.*)', s, re.M)
        if not d:
            continue
        if pkg == PART:
            return d.group(1).strip()
        if best is None and not re.search(r'-(dev|dbg|doc|l10n.*|data|common)$', pkg):
            best = d.group(1).strip()
    return best


SPDX = {'lgpl-2.1+': 'LGPL-2.1-or-later', 'lgpl-2.1': 'LGPL-2.1-only', 'lgpl-2+': 'LGPL-2.0-or-later',
        'lgpl-2': 'LGPL-2.0-only', 'lgpl-3+': 'LGPL-3.0-or-later', 'lgpl-3': 'LGPL-3.0-only',
        'gpl-2+': 'GPL-2.0-or-later', 'gpl-2': 'GPL-2.0-only', 'gpl-3+': 'GPL-3.0-or-later',
        'gpl-3': 'GPL-3.0-only', 'gpl-2.0+': 'GPL-2.0-or-later', 'gpl-3.0+': 'GPL-3.0-or-later',
        'bsd-3-clause': 'BSD-3-Clause', 'bsd-2-clause': 'BSD-2-Clause', 'mit': 'MIT',
        'expat': 'MIT', 'apache-2.0': 'Apache-2.0', 'mpl-2.0': 'MPL-2.0', 'isc': 'ISC',
        'public-domain': 'LicenseRef-public-domain'}


def license_of():
    texts = []
    for f in ('debian/copyright', 'COPYING', 'LICENSE', 'COPYING.LIB', 'LICENSE.LGPL', 'LICENSE.txt'):
        try:
            with open(os.path.join(SRCDIR, f), errors='replace') as fh:
                texts.append(fh.read(200000))
        except OSError:
            pass
    if not texts:
        return 'LicenseRef-unknown'
    m = re.search(r'^Files:\s*\*\s*$.*?^License:\s*(\S+)', texts[0], re.M | re.S)
    if m and m.group(1).lower() in SPDX:
        return SPDX[m.group(1).lower()]
    t = ' '.join(texts)
    later = re.search(r'any later version', t, re.I)
    if re.search(r'Lesser General Public License|Library General Public License', t, re.I):
        v = '2.1' if re.search(r'version 2\.1', t) else ('3.0' if re.search(r'version 3', t) else '2.0')
        return 'LGPL-%s-%s' % (v, 'or-later' if later else 'only')
    if re.search(r'GNU General Public License', t, re.I):
        v = '3.0' if re.search(r'version 3', t) and not re.search(r'version 2', t) else '2.0'
        return 'GPL-%s-%s' % (v, 'or-later' if later else 'only')
    if re.search(r'Redistribution and use in source and binary forms', t):
        return 'BSD-3-Clause' if re.search(r'Neither the name|endorse or promote', t) else 'BSD-2-Clause'
    if re.search(r'Permission is hereby granted, free of charge', t):
        return 'MIT'
    if re.search(r'Apache License', t):
        return 'Apache-2.0'
    return 'LicenseRef-unknown'


# --- dependencies -----------------------------------------------------------------
def ldcache():
    m = {}
    try:
        with open(os.path.join(ROOT, E('LDCACHE', '').lstrip('/'))) as f:
            for line in f:
                x = re.match(r'\s+(\S+) \(([^)]*)\) => (\S+)', line)
                if x and 'AArch64' in x.group(2) and x.group(1) not in m:
                    m[x.group(1)] = x.group(3).lstrip('/')
    except OSError:
        pass
    return m


def is_elf(p):
    try:
        with open(p, 'rb') as f:
            return f.read(4) == b'\x7fELF'
    except OSError:
        return False


def needed(p):
    try:
        out = subprocess.run(['readelf', '-d', p], capture_output=True, text=True).stdout
    except OSError:
        return []
    return re.findall(r'\(NEEDED\)\s+Shared library: \[([^\]]+)\]', out)


LIBDIRS = ('usr/lib/', 'opt/')


def resolve(lib, cache, own):
    path = cache.get(lib)
    if path:
        if path in own:
            return own[path]
        # the cache's path through a link (/lib): the file's own path
        rr = os.path.relpath(os.path.realpath(os.path.join(ROOT, path)), ROOT)
        if rr in own:
            return own[rr]
        d = os.path.relpath(os.path.realpath(os.path.join(ROOT, os.path.dirname(path))), ROOT)
        if os.path.join(d, lib) in own:
            return own[os.path.join(d, lib)]
    for p, o in own.items():
        if p.endswith('/' + lib) and p.startswith(LIBDIRS):
            return o
    return None


def depends(stage, own):
    cache = ldcache()
    deps, missing = [], set()
    files = relfiles(stage)
    mine = {os.path.basename(f) for f in files}

    def add(d):
        if d and d != NAME and d not in deps:
            deps.append(d)

    for rel in files:
        p = os.path.join(stage, rel)
        if os.path.islink(p) or not os.path.isfile(p):
            continue
        if is_elf(p):
            for lib in needed(p):
                if lib in mine:
                    continue
                o = resolve(lib, cache, own)
                if o:
                    add(o)
                else:
                    missing.add(lib)
        else:
            try:
                with open(p, 'rb') as f:
                    head = f.read(128)
            except OSError:
                continue
            if head.startswith(b'#!') and os.access(p, os.X_OK):
                interp = head[2:].split(b'\n')[0].strip().split()
                if not interp:
                    continue
                prog = interp[0].decode(errors='replace')
                if prog.endswith('/env') and len(interp) > 1:
                    prog = '/usr/bin/' + interp[1].decode(errors='replace')
                if os.path.basename(prog) in ('sh', 'bash'):
                    continue
                rel_i = prog.lstrip('/')
                for cand in (rel_i, 'usr/' + rel_i.split('/', 1)[-1] if rel_i.startswith('bin/') else rel_i):
                    real = os.path.relpath(os.path.realpath(os.path.join(ROOT, cand)), ROOT)
                    o = own.get(cand) or own.get(real)
                    if o:
                        add(o)
                        break
    # QML in the N9's components (in .qml files or compiled into resources)
    if NAME != 'qt-components':
        for rel in files:
            p = os.path.join(stage, rel)
            if os.path.isfile(p) and not os.path.islink(p):
                with open(p, 'rb') as f:
                    if b'com.nokia.meego' in f.read():
                        add('qt-components')
                        break
    for d in (E('DEPENDS') or '').split():
        add(d)
    for lib in sorted(missing):
        note('no package has %s (linked by %s)' % (lib, NAME))
    return deps


# --- PKGBUILD ---------------------------------------------------------------------
def q(s):
    return "'" + s.replace("'", "'\"'\"'") + "'"


def main():
    stage = os.path.join(ROOT, STAGE.lstrip('/'))
    out = os.path.join(ROOT, OUTDIR.lstrip('/'))
    own = owners()
    tidy(stage, own)
    if not relfiles(stage):
        print('     nothing left to package')
        sys.exit(3)
    v = E('VERSION') or changelog_version()
    if not v:
        print('     no version (no debian/changelog, no VERSION)')
        sys.exit(4)
    v = arch_version(v)
    desc = E('DESC') or control_desc() or PART
    lic = E('LICENSE') or license_of()
    deps = depends(stage, own)
    schemas = sorted('/' + r for r in relfiles(stage)
                     if r.endswith('.schemas') and r.startswith(('etc/gconf/schemas/', 'usr/share/gconf/schemas/')))
    if schemas and 'gconf' not in deps and NAME != 'gconf':
        deps.append('gconf')
    os.makedirs(out, exist_ok=True)
    install = ''
    if schemas:
        install = 'gconf.install'
        with open(os.path.join(out, install), 'w') as f:
            f.write('post_install() {\n'
                    '\tGCONF_CONFIG_SOURCE=$(gconftool-2 --get-default-source) \\\n'
                    '\t\tgconftool-2 --makefile-install-rule %s > /dev/null\n'
                    '}\npost_upgrade() {\n\tpost_install\n}\n' % ' '.join(schemas))
    arr = lambda xs: '(' + ' '.join(q(x) for x in xs) + ')'
    with open(os.path.join(out, 'PKGBUILD'), 'w') as f:
        f.write('# Made by arch/43-packages.sh (pro1x-maemo-leste %s) from what the\n'
                '# Arch steps built and installed into %s\n' % (E('COMMIT', 'unknown'), STAGE_IN))
        f.write('pkgname=%s\npkgver=%s\npkgrel=1\npkgdesc=%s\narch=(aarch64)\nurl=%s\n'
                % (NAME, v, q(desc), q(E('URL', ''))))
        f.write('license=(%s)\n' % q(lic))
        f.write('depends=%s\n' % arr(deps))
        for k in ('OPTDEPENDS', 'PROVIDES', 'CONFLICTS'):
            xs = (E(k) or '').split()
            if xs:
                f.write('%s=%s\n' % (k.lower(), arr(x.replace('%', ' ') for x in xs)))
        f.write('options=(!debug)\n')
        if install:
            f.write('install=%s\n' % install)
        f.write('package() {\n\tcp -a %s/. "$pkgdir/"\n}\n' % q(STAGE_IN))
    print('     %s %s (depends: %s)' % (NAME, v, ' '.join(deps) or '-'))


main()
