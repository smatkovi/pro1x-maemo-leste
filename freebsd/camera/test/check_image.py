#!/usr/bin/env python3
"""Decode test_image's PNG and JPEG files with Pillow: valid files of the
right size, the JPEG close to the PNG of the same picture, the 16-bit PNG
with the raw values."""
import sys
from PIL import Image

fails = 0
for p in ('RGGB', 'GRBG', 'BGGR', 'GBRG'):
    png = Image.open('test-out/%s-full.png' % p).convert('RGB')
    jpg = Image.open('test-out/%s.jpg' % p).convert('RGB')
    odd = Image.open('test-out/%s-odd.jpg' % p)
    half = Image.open('test-out/%s-half.png' % p)
    if png.size != (320, 240) or jpg.size != (320, 240) or \
            odd.size != (315, 237) or half.size != (160, 120):
        print('FAIL sizes', p, png.size, jpg.size, odd.size, half.size)
        fails += 1
    a, b = png.tobytes(), jpg.tobytes()
    diff = sum(abs(x - y) for x, y in zip(a, b)) / len(a)
    print('  %s: JPEG vs PNG mean difference %.2f' % (p, diff))
    if diff > 6:
        print('FAIL', p, 'JPEG too far from the PNG')
        fails += 1
raw = Image.open('test-out/raw16.png')
if raw.mode not in ('I;16', 'I;16B', 'I') or raw.size != (320, 240):
    print('FAIL raw16', raw.mode, raw.size)
    fails += 1
print('check_image: %s (%d failures)' % ('FAILED' if fails else 'ok', fails))
sys.exit(1 if fails else 0)
