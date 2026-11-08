#!/usr/bin/env python3
"""Extract Melbourne House 'front-directory zip' archives (TFA.ZIP etc.).

These are standard PKZIP archives EXCEPT the central directory sits at the
START of the file (offset 0) and there is no end-of-central-directory record,
so normal zip tools refuse them. Local file headers + data follow the
directory. Seen on Transformers (SLUS-20668, PS2): TFA.ZIP, TFA2.ZIP,
TA_XTRAS.ZIP, SOUNDE.ZIP.

Input : one or more archive paths (positional)
Output: files under <out>/<archive-stem>/<inner-path>, CRC32-verified
        (default out dir: Extracted/archives). --list prints entries only.
Deps  : Python 3.8+ stdlib (argparse, pathlib, struct, zlib)
Spec  : Tools/docs/PK-FRONTDIR-ARCHIVE.md
"""

import argparse
import pathlib
import struct
import sys
import zlib

CDIR_SIG = b"PK\x01\x02"
LOCAL_SIG = b"PK\x03\x04"
CDIR_FMT = "<4s6H3I5H2I"   # 46 bytes
LOCAL_FMT = "<4s5H3I2H"    # 30 bytes


def read_central_directory(fh):
    """Yield (name, method, crc, csize, usize, local_offset) from offset 0."""
    pos = 0
    while True:
        fh.seek(pos)
        raw = fh.read(46)
        if len(raw) < 46 or raw[:4] != CDIR_SIG:
            return
        (_, _, _, _, method, _, _, crc, csize, usize,
         fnlen, extralen, commentlen, _, _, _, lofs) = struct.unpack(CDIR_FMT, raw)
        name = fh.read(fnlen).decode("ascii", errors="replace")
        yield name, method, crc, csize, usize, lofs
        pos += 46 + fnlen + extralen + commentlen


def read_entry_data(fh, method, csize, lofs):
    fh.seek(lofs)
    raw = fh.read(30)
    if raw[:4] != LOCAL_SIG:
        raise ValueError(f"no local header at {lofs:#x}")
    fnlen, extralen = struct.unpack("<2H", raw[26:30])
    fh.seek(lofs + 30 + fnlen + extralen)
    data = fh.read(csize)
    if method == 8:
        return zlib.decompressobj(-15).decompress(data)
    if method == 0:
        return data
    raise ValueError(f"unsupported compression method {method}")


def safe_relpath(name):
    parts = [p for p in name.replace("\\", "/").split("/")
             if p not in ("", ".", "..") and ":" not in p]
    return pathlib.Path(*parts)


def process_archive(archive, out_root, list_only):
    errors = 0
    count = 0
    total = 0
    with open(archive, "rb") as fh:
        for name, method, crc, csize, usize, lofs in read_central_directory(fh):
            count += 1
            total += usize
            if list_only:
                print(f"{usize:>12,}  m{method}  {name}")
                continue
            try:
                data = read_entry_data(fh, method, csize, lofs)
                if zlib.crc32(data) & 0xFFFFFFFF != crc:
                    raise ValueError("CRC mismatch")
                dest = out_root / archive.stem / safe_relpath(name)
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(data)
            except (ValueError, OSError) as exc:
                errors += 1
                print(f"  ERROR {name}: {exc}", file=sys.stderr)
    print(f"{archive.name}: {count} entries, {total/1e6:,.1f} MB uncompressed, "
          f"{errors} errors")
    return errors


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("archives", nargs="+", type=pathlib.Path)
    ap.add_argument("--out", type=pathlib.Path,
                    default=pathlib.Path("Extracted/archives"))
    ap.add_argument("--list", action="store_true", help="list entries only")
    args = ap.parse_args()

    failed = 0
    for archive in args.archives:
        failed += process_archive(archive, args.out, args.list)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
