"""Extract your own Transformers (USA) disc image for the recompilation.

Copies every file from the ISO 9660 filesystem into <out_dir>, keeping the
disc's folder layout, and checks that the executable is the SLUS-20668 build
this project targets. Nothing from the disc is ever part of the repository.

Usage: python extract_iso.py "Transformers (USA).iso" game
"""

import argparse
import struct
import sys
import zlib
from pathlib import Path

from ps2elf import load_game_config

SECTOR = 2048


class Iso:
    def __init__(self, path):
        self.f = open(path, "rb")
        pvd = self.read(16, SECTOR)
        if pvd[1:6] != b"CD001" or pvd[0] != 1:
            raise ValueError(f"{path} is not an ISO 9660 image (no primary volume descriptor)")
        self.volume_id = pvd[40:72].decode("ascii", "replace").strip()
        self.root = self.parse_record(pvd[156:156 + 34])

    def read(self, lba, size):
        self.f.seek(lba * SECTOR)
        return self.f.read(size)

    @staticmethod
    def parse_record(r):
        lba = struct.unpack_from("<I", r, 2)[0]
        size = struct.unpack_from("<I", r, 10)[0]
        flags = r[25]
        name = r[33:33 + r[32]]
        return {"lba": lba, "size": size, "dir": bool(flags & 2), "name": name}

    def listdir(self, entry):
        data = self.read(entry["lba"], entry["size"])
        i = 0
        while i < len(data):
            length = data[i]
            if length == 0:                      # records never span sectors
                i = (i // SECTOR + 1) * SECTOR
                continue
            rec = self.parse_record(data[i:i + length])
            i += length
            if rec["name"] in (b"\x00", b"\x01"):
                continue
            rec["name"] = rec["name"].decode("ascii", "replace").split(";")[0].rstrip(".")
            yield rec

    def walk(self, entry=None, prefix=Path()):
        for rec in self.listdir(entry or self.root):
            path = prefix / rec["name"]
            if rec["dir"]:
                yield from self.walk(rec, path)
            else:
                yield path, rec

    def copy(self, rec, dest):
        dest.parent.mkdir(parents=True, exist_ok=True)
        remaining = rec["size"]
        crc = 0
        self.f.seek(rec["lba"] * SECTOR)
        with open(dest, "wb") as out:
            while remaining:
                chunk = self.f.read(min(remaining, 8 << 20))
                if not chunk:
                    raise IOError(f"image ended early while reading {dest.name}")
                crc = zlib.crc32(chunk, crc)
                out.write(chunk)
                remaining -= len(chunk)
        return crc & 0xFFFFFFFF


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iso", help="your Transformers (USA) disc image")
    ap.add_argument("out_dir", help="folder to extract into (e.g. game)")
    ap.add_argument("--force", action="store_true", help="extract even if the executable does not match")
    args = ap.parse_args()

    cfg = load_game_config()
    iso = Iso(args.iso)
    files = list(iso.walk())
    names = {str(p).upper() for p, _ in files}
    if cfg["elf_name"] not in names and not args.force:
        sys.exit(f"{args.iso} does not contain {cfg['elf_name']}; this is not Transformers (USA, {cfg['serial']}).")

    out = Path(args.out_dir)
    total = sum(r["size"] for _, r in files)
    done = 0
    elf_crc = None
    print(f"Extracting {len(files)} files ({total / 2**30:.2f} GiB) from volume '{iso.volume_id}' to {out}/")
    for path, rec in files:
        dest = out / path
        if dest.exists() and dest.stat().st_size == rec["size"] and str(path).upper() != cfg["elf_name"]:
            done += rec["size"]
            continue
        crc = iso.copy(rec, dest)
        if str(path).upper() == cfg["elf_name"]:
            elf_crc = crc
        done += rec["size"]
        print(f"  [{done * 100 // max(total, 1):3d}%] {path}")

    if elf_crc is not None and f"{elf_crc:08x}" != cfg["elf_crc32"]:
        msg = (f"{cfg['elf_name']} has CRC32 {elf_crc:08x}, expected {cfg['elf_crc32']}. "
               "Only the USA release is supported.")
        if not args.force:
            sys.exit(msg)
        print("Warning: " + msg)
    print("Done.")


if __name__ == "__main__":
    main()
