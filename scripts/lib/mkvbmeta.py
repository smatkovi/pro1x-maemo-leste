#!/usr/bin/env python3
"""Writes a vbmeta image with verification disabled (flags=2).

Equivalent to `avbtool make_vbmeta_image --flags 2 --padding_size 4096` --
without keys (algorithm NONE), without descriptors -- and padded with zeros
to the size of the Pro1X vbmeta partition (64 KiB).
Header layout: AvbVBMetaImageHeader from libavb, 256 bytes, big-endian.
"""
import struct
import sys

PARTITION_SIZE = 64 * 1024
FLAGS_VERIFICATION_DISABLED = 2


def header() -> bytes:
    fields = [
        (">4s", b"AVB0"),
        (">I", 1), (">I", 0),          # required libavb version major.minor
        (">Q", 0), (">Q", 0),          # size of authentication/auxiliary data block
        (">I", 0),                     # algorithm NONE
    ]
    fields += [(">Q", 0)] * 10         # offset/size: hash, signature, pubkey,
                                       # pubkey metadata, descriptors
    fields += [
        (">Q", 0),                     # rollback index
        (">I", FLAGS_VERIFICATION_DISABLED),
        (">I", 0),                     # rollback index location
        (">48s", b"pro1x-maemo-leste"),
    ]
    out = b"".join(struct.pack(f, v) for f, v in fields)
    return out.ljust(256, b"\0")       # 80 bytes reserved at the end


def main(path: str) -> None:
    with open(path, "wb") as f:
        f.write(header().ljust(PARTITION_SIZE, b"\0"))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: mkvbmeta.py OUTPUT")
    main(sys.argv[1])
