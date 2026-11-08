"""Disassemble an IOP module (IRX), naming import stubs and strings (debugging aid).

Usage: python irxdis.py game/IOP/TWOFISH.IRX [start_hex] [count]
"""

import re
import struct
import sys

import capstone


def load(path):
    d = open(path, "rb").read()
    shoff = struct.unpack_from("<I", d, 32)[0]
    shnum, shstr = struct.unpack_from("<HH", d, 48)
    secs = [struct.unpack_from("<10I", d, shoff + 40 * i) for i in range(shnum)]
    so = secs[shstr][4]
    named = {d[so + s[0]:d.index(b"\0", so + s[0])].decode(): s for s in secs}
    _, ph_off = struct.unpack_from("<II", d, 24)
    ph_off = struct.unpack_from("<I", d, 28)[0]
    load_ph = [struct.unpack_from("<8I", d, ph_off + 32 * i) for i in range(struct.unpack_from("<H", d, 44)[0])]
    seg = next(p for p in load_ph if p[0] == 1)
    image = bytearray(seg[5])
    image[:seg[4]] = d[seg[1]:seg[1] + seg[4]]
    return d, named, bytes(image)


def imports(image):
    """Map stub address -> 'library:ordinal' from the import tables."""
    out = {}
    for m in re.finditer(rb"\x00\x00\xe0\x41", image):
        base = m.start()
        if base % 4:
            continue
        lib = image[base + 12:base + 20].rstrip(b"\0").decode("ascii", "replace")
        p = base + 20
        while p + 8 <= len(image):
            w0, w1 = struct.unpack_from("<II", image, p)
            if w0 != 0x03E00008 or (w1 >> 16) != 0x2400:
                break
            out[p] = f"{lib}:{w1 & 0xFFFF}"
            p += 8
    return out


def main():
    _, named, image = load(sys.argv[1])
    start = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    count = int(sys.argv[3]) if len(sys.argv) > 3 else len(image) // 4
    stubs = imports(image)
    md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_LITTLE_ENDIAN)
    lui = {}
    for a in range(start, min(start + 4 * count, len(image)), 4):
        w = struct.unpack_from("<I", image, a)[0]
        ins = next(md.disasm(w.to_bytes(4, "little"), a), None)
        text = f"{ins.mnemonic} {ins.op_str}" if ins else "???"
        note = ""
        op = w >> 26
        if op == 3:
            t = (w & 0x03FFFFFF) << 2
            note = stubs.get(t, "")
        if op == 0x0F:
            lui[(w >> 16) & 31] = (w & 0xFFFF) << 16
        elif op == 0x09 and ((w >> 21) & 31) in lui:
            v = (lui[(w >> 21) & 31] + ((w & 0xFFFF) - 0x10000 if w & 0x8000 else w & 0xFFFF)) & 0xFFFFFFFF
            if v < len(image):
                s = re.match(rb"[ -~\n]{4,}", image[v:v + 80])
                if s:
                    note = repr(s.group().decode())
        if a in stubs:
            note = f"<stub {stubs[a]}>"
        print(f"{a:06x}: {w:08x}  {text:40s} {note}")


if __name__ == "__main__":
    main()
