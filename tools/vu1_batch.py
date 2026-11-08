"""Reads a VU1 batch saved by PS2X_VU1_CAPTURE (runs/vu1_batch_<n>.bin): the batch header,
its routine table and data blocks, the vertices it was given, and the packets VU1 sent.

    python tools/vu1_batch.py runs/vu1_batch_0.bin [--vertices 4] [--blocks]
"""
import argparse
import struct

REGISTER_BYTES = 616


class Registers:
    def __init__(self, data):
        self.vf = [struct.unpack_from("<4f", data, 16 * r) for r in range(32)]
        self.vf_bits = [struct.unpack_from("<4I", data, 16 * r) for r in range(32)]
        self.vi = list(struct.unpack_from("<16i", data, 512))
        self.acc = struct.unpack_from("<4f", data, 576)
        self.q, self.p, self.i = struct.unpack_from("<3f", data, 592)
        self.status, self.mac, self.clip = struct.unpack_from("<3I", data, 604)


class Batch:
    def __init__(self, path):
        raw = open(path, "rb").read()
        if raw[:4] != b"VU1B":
            raise ValueError(f"{path}: not a VU1 batch capture")
        version, self.image, self.pc, self.itop, self.top = struct.unpack_from("<IQIII", raw, 4)
        self.routines = raw[28:92].split(b"\0")[0].decode()
        at = 92
        self.before = Registers(raw[at:at + REGISTER_BYTES])
        at += REGISTER_BYTES
        self.data_before = raw[at:at + 16384]
        at += 16384
        self.after = Registers(raw[at:at + REGISTER_BYTES])
        at += REGISTER_BYTES
        self.data_after = raw[at:at + 16384]
        at += 16384
        self.code = None
        if version >= 2:  # the microcode, 16 KB
            self.code = raw[at:at + 16384]
            at += 16384
        self.packets = []
        while at + 4 <= len(raw):
            size = struct.unpack_from("<I", raw, at)[0]
            self.packets.append(raw[at + 4:at + 4 + size])
            at += 4 + size

    @staticmethod
    def qword(data, index, kind="f"):
        return struct.unpack_from("<4" + kind, data, 16 * (index & 0x3FF))

    def q(self, index, kind="f", after=False):
        return self.qword(self.data_after if after else self.data_before, index, kind)


def fmt(values):
    return "(" + ", ".join(f"{v:.6g}" if isinstance(v, float) else f"{v:#x}" for v in values) + ")"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("batch")
    parser.add_argument("--vertices", type=int, default=4, help="input vertices to print")
    parser.add_argument("--blocks", action="store_true", help="print the routines' data blocks")
    args = parser.parse_args()
    b = Batch(args.batch)
    header = b.q(b.itop, "I")
    count = header[0] & 0x7FFF
    print(f"image {b.image:016x} start pc {b.pc:04x} ITOP q{b.itop:x} TOP q{b.top:x} routines {b.routines}")
    print(f"header q{b.itop:x}: count {count} (x {header[0]:#x}) type {header[1]:#x} bias {struct.unpack('<f', struct.pack('<I', header[2]))[0]:.9g} w {header[3]:#x}")
    table = b.q(4, "I")[0] & 0xFFFF
    for k in range(table):
        entry = b.q(5 + k, "I")
        print(f"  routine {k}: {(entry[0] & 0x7FF) * 8:04x}, data block q{entry[1] & 0xFFFF:x}")
    print("vf01-vf04 (field A screen transform):")
    for r in range(1, 5):
        print("   ", fmt(b.before.vf[r]))
    print(f"vi before: {[hex(v & 0xFFFF) for v in b.before.vi]}")
    print(f"input vertices from q{b.itop + 1:x} (3 qwords each):")
    for v in range(min(count, args.vertices)):
        base = b.itop + 1 + 3 * v
        print(f"  {v}: " + "  ".join(fmt(b.q(base + k)) + "/" + fmt(b.q(base + k, "I")) for k in range(3)))
    if args.blocks:
        for k in range(table):
            entry = b.q(5 + k, "I")
            block = entry[1] & 0xFFFF
            print(f"data block q{block:x}:")
            for i in range(16):
                print(f"  +{i:2d} q{block + i:x}: {fmt(b.q(block + i))}  {fmt(b.q(block + i, 'I'))}")
    print(f"{len(b.packets)} packets: " + ", ".join(f"{len(p)} bytes" for p in b.packets))


if __name__ == "__main__":
    main()
