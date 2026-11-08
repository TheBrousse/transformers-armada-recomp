"""Disassemble VU micro code (debugging aid).

Usage: python vudis.py <vu_code.bin> <start byte address, hex> [pairs]

The input is a raw dump of VU micro memory (e.g. runs/vu1_code.bin written by
the runtime's PS2X_VIF_STATS_DUMP). Each line is one instruction pair:
upper (FMAC) and lower (FDIV/LSU/IALU/branch/EFU).
"""

import struct
import sys

DEST = "xyzw"


def dest(bits):
    return "".join(c for i, c in enumerate(DEST) if bits & (8 >> i))


def vf(n):
    return f"vf{n:02d}"


def vi(n):
    return f"vi{n:02d}"


BC = "xyzw"


def upper(w):
    flags = []
    if w & (1 << 31):
        flags.append("I")
    if w & (1 << 30):
        flags.append("E")
    if w & (1 << 29):
        flags.append("M")
    if w & (1 << 28):
        flags.append("D")
    if w & (1 << 27):
        flags.append("T")
    d = dest((w >> 21) & 0xF)
    ft = (w >> 16) & 0x1F
    fs = (w >> 11) & 0x1F
    fd = (w >> 6) & 0x1F
    op = w & 0x3F
    bc = BC[w & 3]
    names = {
        0x00: "addbc", 0x04: "subbc", 0x08: "maddbc", 0x0C: "msubbc", 0x10: "maxbc", 0x14: "minibc",
        0x18: "mulbc",
    }
    if op < 0x1C:
        base = op & 0x3C
        return f"{names[base][:-2]}{bc}.{d} {vf(fd)}, {vf(fs)}, {vf(ft)}{bc}", flags
    table = {
        0x1C: ("mulq", "q"), 0x1D: ("maxi", "i"), 0x1E: ("muli", "i"), 0x1F: ("minii", "i"),
        0x20: ("addq", "q"), 0x21: ("maddq", "q"), 0x22: ("addi", "i"), 0x23: ("maddi", "i"),
        0x24: ("subq", "q"), 0x25: ("msubq", "q"), 0x26: ("subi", "i"), 0x27: ("msubi", "i"),
        0x28: ("add", None), 0x29: ("madd", None), 0x2A: ("mul", None), 0x2B: ("max", None),
        0x2C: ("sub", None), 0x2D: ("msub", None), 0x2E: ("opmsub", None), 0x2F: ("mini", None),
    }
    if op in table:
        name, special = table[op]
        if special:
            return f"{name}.{d} {vf(fd)}, {vf(fs)}, {special}", flags
        return f"{name}.{d} {vf(fd)}, {vf(fs)}, {vf(ft)}", flags
    if op >= 0x3C:
        sop = (w & 3) | ((w >> 4) & 0x7C)
        acc_bc = {0x00: "addabc", 0x04: "subabc", 0x08: "maddabc", 0x0C: "msubabc", 0x18: "mulabc"}
        base = sop & 0x7C
        if base in acc_bc and sop < 0x1C:
            return f"{acc_bc[base][:-2]}{BC[sop & 3]}.{d} acc, {vf(fs)}, {vf(ft)}{BC[sop & 3]}", flags
        special = {
            0x10: "itof0", 0x11: "itof4", 0x12: "itof12", 0x13: "itof15",
            0x14: "ftoi0", 0x15: "ftoi4", 0x16: "ftoi12", 0x17: "ftoi15",
            0x1C: "mulaq", 0x1D: "abs", 0x1E: "mulai", 0x1F: "clipw",
            0x20: "addaq", 0x21: "maddaq", 0x22: "addai", 0x23: "maddai",
            0x24: "subaq", 0x25: "msubaq", 0x26: "subai", 0x27: "msubai",
            0x28: "adda", 0x29: "madda", 0x2A: "mula", 0x2C: "suba", 0x2D: "msuba",
            0x2E: "opmula", 0x2F: "nop",
        }
        name = special.get(sop, f"upper?{sop:#x}")
        if name == "nop":
            return "nop", flags
        if name.startswith(("itof", "ftoi", "abs")):
            return f"{name}.{d} {vf(ft)}, {vf(fs)}", flags
        if name == "clipw":
            return f"clipw.xyz {vf(fs)}, {vf(ft)}w", flags
        if name in ("mulaq", "addaq", "maddaq", "subaq", "msubaq"):
            return f"{name}.{d} acc, {vf(fs)}, q", flags
        if name in ("mulai", "addai", "maddai", "subai", "msubai"):
            return f"{name}.{d} acc, {vf(fs)}, i", flags
        return f"{name}.{d} acc, {vf(fs)}, {vf(ft)}", flags
    return f"upper?{op:#x}", flags


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def lower(w, pc):
    if w == 0x8000033C:  # move.0 vf00, vf00: how assemblers write a lower NOP
        return "nop"
    op = w >> 25
    d = dest((w >> 21) & 0xF)
    ft = (w >> 16) & 0x1F
    fs = (w >> 11) & 0x1F
    imm11 = sext(w & 0x7FF, 11)
    target = pc + 8 + imm11 * 8
    it, is_ = ft & 0xF, fs & 0xF
    if op == 0x40:
        sub = w & 0x3F
        if sub >= 0x3C:
            sop = (w & 3) | ((w >> 4) & 0x7C)
            names = {
                0x30: "move", 0x31: "mr32", 0x34: "lqi", 0x35: "sqi", 0x36: "lqd", 0x37: "sqd",
                0x38: "div", 0x39: "sqrt", 0x3A: "rsqrt", 0x3B: "waitq", 0x3C: "mtir", 0x3D: "mfir",
                0x3E: "ilwr", 0x3F: "iswr", 0x40: "rnext", 0x41: "rget", 0x42: "rinit", 0x43: "rxor",
                0x64: "mfp", 0x68: "xtop", 0x69: "xitop", 0x6C: "xgkick",
                0x70: "esadd", 0x71: "ersadd", 0x72: "eleng", 0x73: "erleng", 0x74: "eatanxy",
                0x75: "eatanxz", 0x76: "esum", 0x78: "esqrt", 0x79: "ersqrt", 0x7A: "ercpr",
                0x7B: "waitp", 0x7C: "esin", 0x7D: "eatan", 0x7E: "eexp",
            }
            name = names.get(sop, f"lower?{sop:#x}")
            fsf, ftf = BC[(w >> 21) & 3], BC[(w >> 23) & 3]
            if name in ("move", "mr32"):
                return f"{name}.{d} {vf(ft)}, {vf(fs)}"
            if name in ("lqi", "lqd"):
                return f"{name}.{d} {vf(ft)}, ({vi(is_)}{'++' if name == 'lqi' else '--'})"
            if name in ("sqi", "sqd"):
                return f"{name}.{d} {vf(fs)}, ({vi(it)}{'++' if name == 'sqi' else '--'})"
            if name == "div":
                return f"div q, {vf(fs)}{fsf}, {vf(ft)}{ftf}"
            if name in ("sqrt", "rsqrt"):
                return f"{name} q, {vf(fs)}{fsf}, {vf(ft)}{ftf}" if name == "rsqrt" else f"sqrt q, {vf(ft)}{ftf}"
            if name == "mtir":
                return f"mtir {vi(it)}, {vf(fs)}{fsf}"
            if name == "mfir":
                return f"mfir.{d} {vf(ft)}, {vi(is_)}"
            if name in ("ilwr", "iswr"):
                return f"{name}.{d} {vi(it)}, ({vi(is_)})"
            if name in ("xtop", "xitop"):
                return f"{name} {vi(it)}"
            if name == "xgkick":
                return f"xgkick {vi(is_)}"
            if name == "mfp":
                return f"mfp.{d} {vf(ft)}, p"
            if name.startswith("e"):
                return f"{name} p, {vf(fs)}{fsf}"
            return name
        names = {0x30: "iadd", 0x31: "isub", 0x32: "iaddi", 0x34: "iand", 0x35: "ior"}
        name = names.get(sub, f"lower?{sub:#x}")
        id_ = (w >> 6) & 0xF
        if name == "iaddi":
            return f"iaddi {vi(it)}, {vi(is_)}, {sext((w >> 6) & 0x1F, 5)}"
        return f"{name} {vi(id_)}, {vi(is_)}, {vi(it)}"
    table = {
        0x00: "lq", 0x01: "sq", 0x04: "ilw", 0x05: "isw", 0x08: "iaddiu", 0x09: "isubiu",
        0x10: "fceq", 0x11: "fcset", 0x12: "fcand", 0x13: "fcor", 0x14: "fseq", 0x15: "fsset",
        0x16: "fsand", 0x17: "fsor", 0x18: "fmeq", 0x1A: "fmand", 0x1B: "fmor", 0x1C: "fcget",
        0x20: "b", 0x21: "bal", 0x24: "jr", 0x25: "jalr", 0x28: "ibeq", 0x29: "ibne",
        0x2C: "ibltz", 0x2D: "ibgtz", 0x2E: "iblez", 0x2F: "ibgez",
    }
    name = table.get(op)
    if name is None:
        return f"lower?op{op:#x} ({w:08x})"
    if name == "lq":
        return f"lq.{d} {vf(ft)}, {imm11}({vi(is_)})"
    if name == "sq":
        return f"sq.{d} {vf(fs)}, {imm11}({vi(it)})"
    if name in ("ilw", "isw"):
        return f"{name}.{d} {vi(it)}, {imm11}({vi(is_)})"
    if name in ("iaddiu", "isubiu"):
        imm15 = (w & 0x7FF) | ((w >> 10) & 0x7800)
        return f"{name} {vi(it)}, {vi(is_)}, {imm15}"
    if name in ("fceq", "fcset", "fcand", "fcor"):
        return f"{name} vi01, 0x{w & 0xFFFFFF:06x}" if name != "fcset" else f"fcset 0x{w & 0xFFFFFF:06x}"
    if name in ("fseq", "fsset", "fsand", "fsor"):
        imm12 = (w & 0x7FF) | ((w >> 10) & 0x800)
        return f"{name} {vi(it)}, 0x{imm12:03x}"
    if name in ("fmeq", "fmand", "fmor"):
        return f"{name} {vi(it)}, {vi(is_)}"
    if name == "fcget":
        return f"fcget {vi(it)}"
    if name in ("b", "bal"):
        return f"{name} {vi(it)}, 0x{target:04x}" if name == "bal" else f"b 0x{target:04x}"
    if name in ("jr", "jalr"):
        return f"{name} {vi(is_)}" if name == "jr" else f"jalr {vi(it)}, {vi(is_)}"
    if name in ("ibeq", "ibne"):
        return f"{name} {vi(it)}, {vi(is_)}, 0x{target:04x}"
    return f"{name} {vi(is_)}, 0x{target:04x}"


def main():
    data = open(sys.argv[1], "rb").read()
    start = int(sys.argv[2], 16)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    for pc in range(start, min(len(data), start + 8 * count), 8):
        lo, up = struct.unpack_from("<II", data, pc)
        utext, flags = upper(up)
        if up & (1 << 31):
            ltext = f"loi {struct.unpack('<f', struct.pack('<I', lo))[0]:g}"
        else:
            ltext = lower(lo, pc)
        mark = "".join(f"[{f}]" for f in flags)
        print(f"{pc:04x}: {utext:<40} {ltext:<40} {mark}")


if __name__ == "__main__":
    main()
