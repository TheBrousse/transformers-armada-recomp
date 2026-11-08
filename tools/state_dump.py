"""Lists the sections of a save state and extracts its memories for inspection (debugging aid).

    python tools/state_dump.py states/amazon.ps2state               # list sections
    python tools/state_dump.py states/amazon.ps2state -o runs/amazon  # also write the files below

Written with -o: rdram.bin (EE RAM), scratchpad.bin, vu0_code.bin, vu0_data.bin, vu1_code.bin,
vu1_data.bin, vram.bin (GS local memory, for tools/vram_view.py) and iop_ram.bin.
The layout mirrors ps2_savestate.cpp (format 1); a state is game memory, so keep the output
out of version control like the state itself.
"""
import argparse
import struct
from pathlib import Path

MAGIC = b"PS2XSTAT"
RAM_PAGES_MARK = 0x5345474150584D52


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, size):
        chunk = self.data[self.pos:self.pos + size]
        if len(chunk) != size:
            raise ValueError("section is cut short")
        self.pos += size
        return chunk

    def u8(self):
        return self.take(1)[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def u64(self):
        return struct.unpack("<Q", self.take(8))[0]

    def blob(self):
        return self.take(self.u64())

    def ram(self):
        # Main RAM: one blob, or (N7, ps2_savestate.cpp saveRam) by 4 KB pages - runs of all-zero pages (0),
        # pages of one word (1) and raw pages (2) after the mark "RMXPAGES" and the size.
        first = self.u64()
        if first != RAM_PAGES_MARK:
            return self.take(first)
        size = self.u64()
        out = bytearray()
        while len(out) < size:
            kind, count = self.u8(), self.u32()
            if kind == 0:
                out += bytes(count * 4096)
            elif kind == 1:
                out += self.take(4) * (count * 1024)
            elif kind == 2:
                out += self.take(count * 4096)
            else:
                raise ValueError("RAM pages: unknown run %d" % kind)
        return bytes(out)


def sections(data):
    if data[:8] != MAGIC:
        raise SystemExit("not a save state")
    version = struct.unpack_from("<I", data, 8)[0]
    result = {}
    pos = 12
    while pos < len(data):
        tag, size = struct.unpack_from("<IQ", data, pos)
        pos += 12
        result[struct.pack("<I", tag).decode("ascii").strip()] = data[pos:pos + size]
        pos += size
    return version, result


def ee_memory(section):
    reader = Reader(section)
    files = {"rdram.bin": reader.ram(), "scratchpad.bin": reader.blob()}
    reader.u8()                           # seen GIF copy
    reader.take(reader.u64() * 8)         # I/O registers (address, value)
    privileged = struct.unpack("<20Q", reader.take(160))
    reader.take(23 * 4 * 2)               # VIF0, VIF1 registers
    reader.take(10 * 7 * 4)               # DMA channels
    reader.take(reader.u64() * 16)        # TLB entries
    for name in ("vu0_code.bin", "vu0_data.bin", "vu1_code.bin", "vu1_data.bin"):
        files[name] = reader.blob()
    return files, privileged


def gs_memory(section):
    reader = Reader(section)
    reader.take(1024 + 8)                 # CLUT buffer, CBP0/CBP1
    # Transfer command and progress, then the local-to-host buffer: skip to the VRAM blob,
    # which is the first 4 MB blob in the section.
    marker = struct.pack("<Q", 4 * 1024 * 1024)
    start = section.index(marker, reader.pos)
    return section[start + 8:start + 8 + 4 * 1024 * 1024]


def iop_ram(section):
    # The IOP section starts with the HLE module records; RAM is the first 2 MB blob.
    marker = struct.pack("<Q", 2 * 1024 * 1024)
    start = section.index(marker)
    return section[start + 8:start + 8 + 2 * 1024 * 1024]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("state")
    parser.add_argument("-o", "--output", help="folder for the extracted memories")
    args = parser.parse_args()

    data = Path(args.state).read_bytes()
    version, parts = sections(data)
    print(f"format {version}, {len(data) / (1024 * 1024):.1f} MB")
    for name, payload in parts.items():
        print(f"  {name:<5} {len(payload):>10} bytes")

    files, privileged = ee_memory(parts["EEMM"])
    names = ("PMODE", "SMODE1", "SMODE2", "SRFSH", "SYNCH1", "SYNCH2", "SYNCV", "DISPFB1", "DISPLAY1", "DISPFB2",
             "DISPLAY2", "EXTBUF", "EXTDATA", "EXTWRITE", "BGCOLOR", "CSR", "vblank tick", "IMR", "BUSDIR", "SIGLBLID")
    print("GS privileged registers:")
    for name, value in zip(names, privileged):
        if value:
            print(f"  {name:<12} {value:#x}" if name != "vblank tick" else f"  {name:<12} {value}")

    if args.output:
        files["vram.bin"] = gs_memory(parts["GS"])
        files["iop_ram.bin"] = iop_ram(parts["IOP"])
        folder = Path(args.output)
        folder.mkdir(parents=True, exist_ok=True)
        for name, payload in files.items():
            (folder / name).write_bytes(payload)
            print(f"wrote {folder / name} ({len(payload)} bytes)")


if __name__ == "__main__":
    main()
