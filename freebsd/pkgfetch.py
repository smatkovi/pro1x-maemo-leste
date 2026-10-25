#!/usr/bin/env python3
"""Fetch FreeBSD packages with their dependencies, on any host.

    pkgfetch.py [-x PATTERN]... SITE CACHE URL NAME...

SITE is the unpacked, already verified packagesite (packagesite.yaml: one
JSON object per package), URL the repository (e.g.
https://pkg.freebsd.org/FreeBSD:15:aarch64/quarterly). The packages NAME
and everything they depend on are downloaded into CACHE and checked
against the repository's checksum (BLAKE2b-512 of the file, in pkg's
legacy base32: type 2). Prints the package files, dependencies first.
-x leaves out packages matching PATTERN (fnmatch), with what only they
pull in: dependencies needed at build time or by optional parts only.
"""

import fnmatch
import hashlib
import json
import os
import sys
import urllib.parse
import urllib.request

B32 = "ybndrfg8ejkmcpqxot1uwisza345h769"


def base32_legacy(data):
    """pkg_checksum_encode_base32_legacy() of libpkg."""
    out = []
    remain = -1
    for i, c in enumerate(data):
        m = i % 5
        if m == 0:
            x = c
            remain = c >> 5
            out.append(B32[x & 31])
        elif m == 1:
            x = remain | c << 3
            out += [B32[x & 31], B32[x >> 5 & 31]]
            remain = x >> 10
        elif m == 2:
            x = remain | c << 1
            out.append(B32[x & 31])
            remain = x >> 5
        elif m == 3:
            x = remain | c << 4
            out += [B32[x & 31], B32[x >> 5 & 31]]
            remain = x >> 10 & 3
        else:
            x = remain | c << 2
            out += [B32[x & 31], B32[x >> 5 & 31]]
            remain = -1
    if remain >= 0:
        out.append(B32[remain])
    return "".join(out)


def checksum_ok(path, want):
    kind, _, value = want.partition("$")
    with open(path, "rb") as f:
        data = f.read()
    if kind == "2":
        return base32_legacy(hashlib.blake2b(data, digest_size=64).digest()) == value
    if kind == "1" or (not value and len(want) == 64):
        return hashlib.sha256(data).hexdigest() == (value or want)
    sys.exit(f"pkgfetch: unknown checksum type in {want[:12]}...")


def main():
    args = sys.argv[1:]
    prune = []
    while len(args) >= 2 and args[0] == "-x":
        prune.append(args[1])
        args = args[2:]
    if len(args) < 4:
        sys.exit(__doc__)
    site, cache, url = args[:3]
    names = args[3:]

    pkgs = {}
    with open(os.path.join(site, "packagesite.yaml")) as f:
        for line in f:
            p = json.loads(line)
            pkgs[p["name"]] = p

    order, seen = [], set()

    def visit(name, chain):
        if name in seen or any(fnmatch.fnmatch(name, p) for p in prune):
            return
        if name not in pkgs:
            sys.exit(f"pkgfetch: no package {name} (needed by {' <- '.join(chain)})")
        seen.add(name)
        for dep in sorted(pkgs[name].get("deps", {})):
            visit(dep, chain + [name])
        order.append(name)

    for n in names:
        visit(n, [])

    os.makedirs(cache, exist_ok=True)
    total = 0
    for name in order:
        p = pkgs[name]
        dest = os.path.join(cache, os.path.basename(p["repopath"]))
        if not (os.path.exists(dest) and checksum_ok(dest, p["sum"])):
            src = url.rstrip("/") + "/" + urllib.parse.quote(p["repopath"])
            with urllib.request.urlopen(src) as r, open(dest + ".part", "wb") as f:
                f.write(r.read())
            os.replace(dest + ".part", dest)
            if not checksum_ok(dest, p["sum"]):
                os.unlink(dest)
                sys.exit(f"pkgfetch: checksum mismatch for {name}")
        total += p.get("flatsize", 0)
        print(dest)
    print(f"pkgfetch: {len(order)} packages, {total // (1 << 20)} MiB installed",
          file=sys.stderr)


if __name__ == "__main__":
    main()
