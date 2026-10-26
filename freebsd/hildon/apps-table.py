#!/usr/bin/env python3
"""The table of packages in freebsd/hildon/README.md, between its
<!-- packages --> markers, from the metadata pkgmeta.py made and the state
of each in status.txt ("name|state", the state as tried in the VM).

    apps-table.py PKGMETA_DIR
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
META = sys.argv[1]
status = {}
for line in open(os.path.join(HERE, 'status.txt')):
    line = line.strip()
    if line and not line.startswith('#'):
        n, _, st = line.partition('|')
        status[n.strip()] = st.strip()

rows = []
for n in sorted(os.listdir(META)):
    f = os.path.join(META, n, '+MANIFEST')
    if not os.path.exists(f):
        continue
    m = json.load(open(f))
    src = m.get('annotations', {}).get('source', m['www'])
    rev = m.get('annotations', {}).get('revision', '')
    rev = rev[:12] if re.fullmatch(r'[0-9a-f]{40}', rev) else ''
    name = re.sub(r'\.git$', '', src).rstrip('/').split('/')[-1]
    link = '[%s](%s)' % (name, re.sub(r'\.git$', '', src)) if src.startswith('http') else src
    rows.append('| %s | %s | %s | %s | %s | %s |' % (
        m['name'], m['version'], ', '.join(m['licenses']), link,
        ('`%s`' % rev) if rev else '', status.get(m['name'], 'builds')))
table = '\n'.join(['| Package | Version | Licence | Source | Revision | State |',
                   '| --- | --- | --- | --- | --- | --- |'] + rows)
readme = os.path.join(HERE, 'README.md')
s = open(readme).read()
s = re.sub(r'(<!-- packages -->\n).*?(<!-- /packages -->)',
           lambda mm: mm.group(1) + table + '\n' + mm.group(2), s, flags=re.S)
open(readme, 'w').write(s)
print('%d packages' % len(rows))
