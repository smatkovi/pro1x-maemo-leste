#!/usr/bin/env python3
"""Write the makefs specification of a root tree unpacked as a normal user.

    mkspec.py ROOT OUT SPEC...

Every path under ROOT gets one line in OUT (an mtree specification, parents
before children), with its attributes from the first SPEC that lists it
(mtree files: the release's, the packages', the overlay's), or else owned
by root with the mode the unpacked file has. Paths are compared decoded,
written encoded as mtree(5) does (\\ooo for unusual bytes).
"""

import os
import re
import stat
import sys

SAFE = set(b"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
           b"./_-+,:@%=~^!()[]{}'$&;")
OCTAL = re.compile(rb"\\([0-7]{3})")


def decode(name):
    return OCTAL.sub(lambda m: bytes([int(m.group(1), 8)]), name)


def encode(raw):
    return b"".join(bytes([c]) if c in SAFE and c != 0x5c else b"\\%03o" % c
                    for c in raw)


def read_spec(path, attrs):
    with open(path, "rb") as f:
        for line in f:
            line = line.rstrip(b"\n")
            if not line or line.startswith(b"#") or line.startswith(b"/"):
                continue
            name, _, rest = line.partition(b" ")
            raw = decode(name)
            if not raw.startswith(b"./"):
                raw = b"./" + raw.lstrip(b"/")
            raw = raw.rstrip(b"/") if raw != b"./" else b"."
            if raw == b"./":
                raw = b"."
            # The first source wins; keywords without type/time stay.
            if raw not in attrs:
                kw = [k for k in rest.split() if not k.startswith(b"time=")
                      and not k.startswith(b"size=")]
                attrs[raw] = kw


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    root = sys.argv[1].encode()
    out = sys.argv[2]
    attrs = {}
    for spec in sys.argv[3:]:
        read_spec(spec, attrs)

    lines = []

    def entry(rel, full):
        st = os.lstat(full)
        kw = attrs.get(rel)
        if kw is None:
            if stat.S_ISDIR(st.st_mode):
                t = b"dir"
            elif stat.S_ISLNK(st.st_mode):
                t = b"link"
            else:
                t = b"file"
            kw = [b"type=" + t, b"uid=0", b"gid=0",
                  b"mode=%o" % stat.S_IMODE(st.st_mode)]
            if t == b"link":
                kw.append(b"link=" + encode(os.readlink(full)))
        lines.append(encode(rel) + b" " + b" ".join(kw))

    entry(b".", root)
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        rel_dir = b"." + dirpath[len(root):]
        for n in sorted(filenames) + [d for d in dirnames
                                      if os.path.islink(os.path.join(dirpath, d))]:
            entry(rel_dir + b"/" + n, os.path.join(dirpath, n))
        for d in list(dirnames):
            if os.path.islink(os.path.join(dirpath, d)):
                dirnames.remove(d)
                continue
            entry(rel_dir + b"/" + d, os.path.join(dirpath, d))

    with open(out, "wb") as f:
        f.write(b"#mtree\n")
        f.write(b"\n".join(lines) + b"\n")
    print(f"mkspec: {len(lines)} entries", file=sys.stderr)


if __name__ == "__main__":
    main()
