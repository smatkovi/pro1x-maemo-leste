#!/usr/bin/env python3
"""The traces FreeBSD and KolibriOS leave for Linux in the bootloader's
spare splash memory, read from Linux after the next boot (as root):

    ramconsole.py [--shot FILE] [--clear]

0x5ca00000  the entry marker (PRO1X-FBSD:ENTERED, PRO1X-KOS:ENTERED)
0x5ca01000  the RAM console, up to 1 MiB, NUL-terminated
            (freebsd/patches/0003, KolibriOS's console). The reset that
            follows a run flips single bits in it (and in the pstore), some
            into a NUL: the text ends where it stops being text, not at the
            first NUL (2026-11-01: the first NUL was 207 bytes in, the run's
            console 52 KB long); a byte that is not text shows as '?'.
0x5cc00000  "PRO1X-SHOT\\n", the length (32 bits, little endian, at 12) and
            the data from 16, up to 3 MiB (freebsd/patches/0014
            /dev/pro1xshot: a gzip'd xwd or a camera tar; KolibriOS: a PPM)

Prints the marker and the console to stdout; --shot FILE writes the
screenshot's data. Only reads, unless --clear: that zeroes the marker, the
whole console and the screenshot's header afterwards, so that the next read
does not show this run (or its tail behind a shorter next run) again; do it
once the output is saved.

The region is reserved, no-map memory: read() on /dev/mem refuses it and
its uncached mapping takes only aligned accesses, so it is mapped and read
in aligned 8-byte words (as KolibriOS's tools/pro1x-read.py, whose layout
this is). PRO1X_MEM=FILE reads FILE as the region instead (PC tests).
"""
import mmap
import os
import sys

BASE = 0x5ca00000
SIZE = 0x500000
MARKER, CONSOLE, SHOT = 0x0, 0x1000, 0x200000
CONSOLE_SIZE, SHOT_SIZE = 1 << 20, 3 << 20


def words(view, off, n):
    out = bytearray()
    for i in range(off // 8, (off + n + 7) // 8):
        out += view[i].to_bytes(8, 'little')
    return bytes(out[:n])


def printable(c):
    return c in (9, 10, 13) or 32 <= c < 127


def text_of(raw, window=512):
    """The console's text: up to the last window that is mostly text, before
    a stretch that is not (a run's own NUL, then zeros or older data)."""
    end = 0
    for off in range(0, len(raw), window):
        part = raw[off:off + window]
        if sum(map(printable, part)) >= 0.9 * len(part):
            end = off + len(part)
        elif end and off - end >= 8 * window:
            break
    text = raw[:end]
    nul = text.rfind(b'\0')
    # the run's terminating NUL in the last window: the text ends there
    if nul >= 0 and nul >= end - window and not any(map(printable, text[nul + 1:nul + 64])):
        text = text[:nul]
    return bytes(c if printable(c) else 63 for c in text)


def main():
    args = sys.argv[1:]
    clear = '--clear' in args
    shot_file = args[args.index('--shot') + 1] if '--shot' in args else None
    test = os.environ.get('PRO1X_MEM')
    if test:
        fd = os.open(test, os.O_RDWR if clear else os.O_RDONLY)
        offset = 0
    else:
        fd = os.open('/dev/mem', (os.O_RDWR if clear else os.O_RDONLY) | os.O_SYNC)
        offset = BASE
    prot = mmap.PROT_READ | (mmap.PROT_WRITE if clear else 0)
    mm = mmap.mmap(fd, SIZE, mmap.MAP_SHARED, prot, offset=offset)
    view = memoryview(mm).cast('Q')

    marker = words(view, MARKER, 24).split(b'\n')[0].split(b'\0')[0]
    print('marker:', marker.decode(errors='replace') if marker.startswith(b'PRO1X-') else 'none')
    raw = words(view, CONSOLE, CONSOLE_SIZE)
    con = text_of(raw)
    if con.startswith(b'PRO1X-'):
        sys.stdout.write(con.decode(errors='replace'))
        if not con.endswith(b'\n'):
            sys.stdout.write('\n')
        if len(con) >= CONSOLE_SIZE - 1:
            print('ramconsole: the console is full (1 MiB): the end of the run is missing')
    else:
        print('no console')

    head = words(view, SHOT, 16)
    if head.startswith(b'PRO1X-SHOT\n'):
        n = int.from_bytes(head[12:16], 'little')
        print(f'screenshot: {n} bytes')
        if shot_file and 0 < n <= SHOT_SIZE - 16:
            with open(shot_file, 'wb') as f:
                f.write(words(view, SHOT + 16, n))
    else:
        print('no screenshot')

    if clear:
        for off in (MARKER, SHOT):
            for i in range(4):
                view[off // 8 + i] = 0
        for i in range(CONSOLE // 8, (CONSOLE + CONSOLE_SIZE) // 8):
            view[i] = 0
        print('cleared', file=sys.stderr)
    view.release()
    mm.close()
    os.close(fd)


if __name__ == '__main__':
    main()
