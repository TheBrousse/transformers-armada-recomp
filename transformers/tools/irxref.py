"""Find the code of an IOP module (IRX) that refers to a string or an address (debugging aid).

Usage: python irxref.py game/IOP/SIRENS3.IRX "Potential deadlock" [--base ab300]
       python irxref.py game/IOP/SIRENS3.IRX --addr 1234 [--base ab300]

Prints every lui/addiu (or lui/ori) pair and every jal that produces the address, with the
start of the function it is in (the nearest "addiu sp, sp, -N" above). Addresses are
image-relative; --base adds the load address the IOP log reports ("base=0x...").
"""

import re
import struct
import sys

from irxdis import load, imports


def main():
    args = sys.argv[1:]
    base = 0
    if "--base" in args:
        i = args.index("--base")
        base = int(args[i + 1], 16)
        del args[i:i + 2]
    _, _, image = load(args[0])
    targets = {}
    if args[1] == "--addr":
        targets[int(args[2], 16) - base] = "addr"
    else:
        needle = args[1].encode()
        for m in re.finditer(re.escape(needle), image):
            start = m.start()
            while start > 0 and 32 <= image[start - 1] < 127:
                start -= 1
            end = image.index(b"\0", m.start())
            targets[start] = image[start:end].decode("ascii", "replace")
    if not targets:
        print("not found")
        return
    for t, text in targets.items():
        print(f"target {t + base:06x}: {text!r}")
    lui = {}
    func = 0
    for a in range(0, len(image) - 3, 4):
        w = struct.unpack_from("<I", image, a)[0]
        op = w >> 26
        rs, rt = (w >> 21) & 31, (w >> 16) & 31
        imm = w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        if op == 9 and rs == 29 and rt == 29 and simm < 0:
            func = a
            lui = {}
        if op == 0x0F:
            lui[rt] = (imm << 16, a)
        elif op in (9, 0x0D) and rs in lui:
            v = (lui[rs][0] + (simm if op == 9 else imm)) & 0xFFFFFFFF
            if v in targets:
                print(f"  {a + base:06x} (function {func + base:06x}): loads {v + base:06x}")
        elif op == 3:
            v = (w & 0x03FFFFFF) << 2
            if v in targets:
                print(f"  {a + base:06x} (function {func + base:06x}): jal {v + base:06x}")
        # data words
        if w in targets and op not in (3, 9, 0x0D, 0x0F):
            print(f"  {a + base:06x}: data word {w + base:06x}")


if __name__ == "__main__":
    main()
