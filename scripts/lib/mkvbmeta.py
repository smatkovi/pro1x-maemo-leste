#!/usr/bin/env python3
"""Schreibt ein vbmeta-Image mit abgeschalteter Verifikation (flags=2).

Entspricht `avbtool make_vbmeta_image --flags 2 --padding_size 4096` --
ohne Schluessel (Algorithmus NONE), ohne Deskriptoren -- und wird auf die
Groesse der vbmeta-Partition des Pro1X (64 KiB) mit Nullen aufgefuellt.
Header-Layout: AvbVBMetaImageHeader aus libavb, 256 Byte, big-endian.
"""
import struct
import sys

PARTITION_SIZE = 64 * 1024
FLAGS_VERIFICATION_DISABLED = 2


def header() -> bytes:
    fields = [
        (">4s", b"AVB0"),
        (">I", 1), (">I", 0),          # benoetigte libavb-Version major.minor
        (">Q", 0), (">Q", 0),          # Groesse Authentifizierungs-/Hilfsdatenblock
        (">I", 0),                     # Algorithmus NONE
    ]
    fields += [(">Q", 0)] * 10         # Offset/Groesse: hash, signatur, pubkey,
                                       # pubkey-metadaten, deskriptoren
    fields += [
        (">Q", 0),                     # rollback index
        (">I", FLAGS_VERIFICATION_DISABLED),
        (">I", 0),                     # rollback index location
        (">48s", b"pro1x-maemo-leste"),
    ]
    out = b"".join(struct.pack(f, v) for f, v in fields)
    return out.ljust(256, b"\0")       # 80 Byte reserviert am Ende


def main(path: str) -> None:
    with open(path, "wb") as f:
        f.write(header().ljust(PARTITION_SIZE, b"\0"))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Nutzung: mkvbmeta.py AUSGABE")
    main(sys.argv[1])
