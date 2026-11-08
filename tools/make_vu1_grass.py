#!/usr/bin/env python3
"""Generates the grass's VU1 program as C++ (vu1_grass47dd.inc) from your disc's executable.

Purpose: the grass (VU1 microcode image 47dd78e8f5335846, routine 0x1380) runs as generated straight C++
         (docs/RENDERER.md, "Microcode recompiled"; transformers/src/vu1_native.cpp includes the result).
         That code is a translation of the game's own microcode, so it is not kept in the repository: it is
         made at build time from the ELF. The image is assembled from the pieces the game uploads (their
         places in SLUS_206.68 below), checked against its known hash, and given to tools/vu1_recompile.py
         with the measured read lags (tools/vu1_grass47dd_lags.json, which hold addresses and lags only).
Input:   the game's executable (game/SLUS_206.68).
Output:  the .inc file (written only when its contents change, so an unchanged build compiles nothing).
Deps:    Python 3.8+ standard library; tools/vu1_recompile.py and its modules beside this file.
Usage:   python tools/make_vu1_grass.py game/SLUS_206.68 work/generated/vu1_grass47dd.inc
"""
from __future__ import annotations

import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# The image's pieces: (VU1 code address, offset in SLUS_206.68, bytes). The last 32 bytes of the 16 KB are
# not part of the program (zero here).
PIECES = [
    (0x0000, 0x4DE060, 1104), (0x0450, 0x4DDBB0, 48), (0x0480, 0x4FA050, 3840), (0x1380, 0x4E1E10, 3872),
    (0x22A0, 0x4FAF50, 1936), (0x2A30, 0x4E0EC0, 704), (0x2CF0, 0x4DE840, 2416), (0x3660, 0x4E0160, 448),
    (0x3820, 0x5042B0, 832), (0x3B60, 0x4FF510, 944), (0x3F10, 0x502900, 208),
]
CODE_SHA256 = "fbe0c0839fe1166552842d907ef46e196d8c76e750c4cc959fed6f0025e89561"
LAGS = os.path.join(HERE, "vu1_grass47dd_lags.json")


def assemble(elf: bytes) -> bytes:
    code = bytearray(16384)
    for vu, offset, size in PIECES:
        if offset + size > len(elf):
            raise SystemExit(f"make_vu1_grass: {size} bytes at {offset:#x} are past the end of the executable")
        code[vu:vu + size] = elf[offset:offset + size]
    return bytes(code)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[-1].strip())
        return 2
    elf_path, out_path = sys.argv[1], sys.argv[2]
    code = assemble(open(elf_path, "rb").read())
    digest = hashlib.sha256(code).hexdigest()
    if digest != CODE_SHA256:
        raise SystemExit(f"make_vu1_grass: the grass microcode from {elf_path} is not the one expected "
                         f"({digest[:16]}...): is it the USA disc's SLUS_206.68?")
    import vu1_recompile
    text = vu1_recompile.emit_bytes(code, LAGS, [0x10, 0xF8], [0x1380], "runGrass47dd")
    old = open(out_path, "r", encoding="utf-8", newline="").read() if os.path.exists(out_path) else None
    if old != text:
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        with open(out_path, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        print(f"make_vu1_grass: {out_path} written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
