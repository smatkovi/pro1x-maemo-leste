#!/usr/bin/env python3
"""X's screen from an fbsd-diag.sh shot: the begin-base64 block (b64encode
of the gzipped xwd) out of the text, as an xwd file and, with ImageMagick,
a PNG next to the shot.

    fbsd-diag-xwd.py SHOT.txt
"""
import base64, os, re, subprocess, sys, zlib

shot = sys.argv[1]
text = open(shot, 'rb').read()
m = re.search(rb'^begin-base64 \d+ (\S+)\n(.*?)\n====\n', text, re.S | re.M)
if m is None:
    sys.exit('no screen in this shot (X did not answer xwd?)')
out = os.path.join(os.path.dirname(os.path.abspath(shot)), 'screen.xwd')
# The shot buffer loses a bit now and then across the reset: decode what
# the stream gives up to the first damage (the top of the screen)
raw = re.sub(rb'[^A-Za-z0-9+/=]', b'', m.group(2))
data = base64.b64decode(raw[:len(raw) // 4 * 4])
d = zlib.decompressobj(16 + zlib.MAX_WBITS)
img = b''
try:
    img = d.decompress(data)
except zlib.error as e:
    print(f'gzip damaged ({e}): only the part before it', file=sys.stderr)
    for n in range(len(data), 0, -4096):
        try:
            img = zlib.decompressobj(16 + zlib.MAX_WBITS).decompress(data[:n])
            break
        except zlib.error:
            continue
open(out, 'wb').write(img)
print(out, len(img), 'bytes')
png = out[:-4] + '.png'
if subprocess.run(['magick', 'xwd:' + out, png]).returncode == 0:
    print(png)
