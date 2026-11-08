"""Disassemble a range of the Transformers ELF (debugging aid).

Usage: python mipsdis.py <elf> <start> [count]      addresses in hex
       python mipsdis.py <elf> --xref <addr>        find lui/addiu pairs and words referencing addr
"""

import sys

import capstone

from ps2elf import Elf, sext16


def disasm(elf, start, count):
    md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64 + capstone.CS_MODE_LITTLE_ENDIAN)
    for a in range(start, start + 4 * count, 4):
        w = elf.word(a)
        ins = next(md.disasm(w.to_bytes(4, "little"), a), None)
        text = f"{ins.mnemonic} {ins.op_str}" if ins else "???"
        print(f"{a:08x}: {w:08x}  {text}")


def xref(elf, target):
    for i, w in enumerate(elf.words):
        a = elf.base + 4 * i
        if w == target:
            print(f"{a:08x}: data word")
        if w >> 26 == 0x0F:
            rt, hi = (w >> 16) & 31, (w & 0xFFFF) << 16
            for b in range(a + 4, min(a + 64, elf.end), 4):
                w2 = elf.word(b)
                if (w2 >> 26) in (0x09, 0x23, 0x2B, 0x0D) and ((w2 >> 21) & 31) == rt:
                    lo = (w2 & 0xFFFF) if w2 >> 26 == 0x0D else sext16(w2)
                    if (hi + lo) & 0xFFFFFFFF == target:
                        print(f"{a:08x}: lui/{b:08x}")
                    break
        if w >> 26 in (2, 3) and ((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2) == target:
            print(f"{a:08x}: {'jal' if w >> 26 == 3 else 'j'}")


if __name__ == "__main__":
    elf = Elf(sys.argv[1])
    if sys.argv[2] == "--xref":
        xref(elf, int(sys.argv[3], 16))
    else:
        disasm(elf, int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 32)
