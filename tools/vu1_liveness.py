"""Which VU1 registers a code image's programs read before writing them (debugging aid for
docs/RENDERER.md): the state a program run has to leave exactly as the microcode would, for
the runs after it. Whole-program backward liveness over every pair: VF lanes, VI registers,
ACC, Q, P, I, R and the MAC, status and clip flags.

    python tools/vu1_liveness.py runs/vu1_image_4.bin --routines 1410,0480 [--entries 0,10,f8,450]

Calls are followed as calls: what is live before a BAL or JALR is what its callee reads
(a JALR may call any routine given) and what is live where it returns, as if the callee wrote
nothing; a JR is the end of a callee. A program end goes on to every entry point given (the
next program run).
"""
import argparse
import struct

LANES = "xyzw"
ACC, Q, P, I, R, MAC, STATUS, CLIP = range(144, 152)
NAMES = {ACC: "acc", Q: "q", P: "p", I: "i", R: "r", MAC: "mac", STATUS: "status", CLIP: "clip"}


def vf(reg, dest):
    """VF `reg`'s lanes in `dest` (x = 8) as a bit set; VF0 is a constant."""
    if reg == 0:
        return 0
    bits = 0
    for lane in range(4):
        if dest & (8 >> lane):
            bits |= 1 << (reg * 4 + lane)
    return bits


def vf_lane(reg, field):
    return vf(reg, 8 >> field)


def vi(reg):
    return 0 if reg == 0 else 1 << (128 + reg)


def acc(dest):
    return sum(1 << (ACC + 0) for lane in range(4) if dest & (8 >> lane)) and (1 << ACC)


def special(name):
    return 1 << name


class Pair:
    def __init__(self, pc, lower, upper):
        self.pc = pc
        self.reads = 0
        self.writes = 0
        self.ebit = bool(upper & 0x40000000)
        self.kind = None  # None, "branch", "cond", "jr", "jalr", "bal"
        self.target = None
        self.link = None
        self.decode_upper(upper)
        if upper & 0x80000000:
            self.writes |= special(I)
        else:
            self.decode_lower(lower)

    def decode_upper(self, w):
        dest = (w >> 21) & 0xF
        ft, fs, fd = (w >> 16) & 0x1F, (w >> 11) & 0x1F, (w >> 6) & 0x1F
        op = w & 0x3F
        r = 0
        wr = 0
        if op < 0x3C:
            if op > 0x2F:
                return
            kind = op >> 2 if op < 0x1C else None
            flags = True
            if op < 0x1C:  # bc forms: add sub madd msub max mini mul
                r |= vf(fs, dest) | vf_lane(ft, op & 3)
                if kind in (2, 3):
                    r |= special(ACC)
                if kind in (4, 5):
                    flags = False
            else:
                operand = {0x1C: Q, 0x1D: I, 0x1E: I, 0x1F: I, 0x20: Q, 0x21: Q, 0x22: I, 0x23: I, 0x24: Q,
                           0x25: Q, 0x26: I, 0x27: I}.get(op)
                if operand is not None:
                    r |= vf(fs, dest) | special(operand)
                    if op in (0x21, 0x23, 0x25, 0x27):
                        r |= special(ACC)
                    if op in (0x1D, 0x1F):
                        flags = False
                elif op == 0x2E:  # OPMSUB
                    r |= vf(fs, 0xE) | vf(ft, 0xE) | special(ACC)
                else:
                    r |= vf(fs, dest) | vf(ft, dest)
                    if op in (0x29, 0x2D):
                        r |= special(ACC)
                    if op in (0x2B, 0x2F):
                        flags = False
            wr |= vf(fd, dest)
            if flags and dest:
                wr |= special(MAC) | special(STATUS)
                r |= special(STATUS)  # sticky bits accumulate
        else:
            code = (w & 3) | ((w >> 4) & 0x7C)
            if code <= 0x0F or 0x18 <= code <= 0x1B:  # ADDA SUBA MADDA MSUBA MULA bc
                r |= vf(fs, dest) | vf_lane(ft, code & 3)
                if (code >> 2) in (2, 3):
                    r |= special(ACC)
                wr |= special(ACC) | special(MAC) | special(STATUS)
                r |= special(STATUS)
            elif 0x10 <= code <= 0x17 or code == 0x1D:  # ITOF FTOI ABS
                r |= vf(fs, dest)
                wr |= vf(ft, dest)
            elif code == 0x1F:  # CLIP
                r |= vf(fs, 0xE) | vf_lane(ft, 3) | special(CLIP)
                wr |= special(CLIP)
            elif code in (0x1C, 0x1E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27):
                operand = Q if code in (0x1C, 0x20, 0x21, 0x24, 0x25) else I
                r |= vf(fs, dest) | special(operand)
                if code in (0x21, 0x23, 0x25, 0x27):
                    r |= special(ACC)
                wr |= special(ACC) | special(MAC) | special(STATUS)
                r |= special(STATUS)
            elif code in (0x28, 0x29, 0x2A, 0x2C, 0x2D):
                r |= vf(fs, dest) | vf(ft, dest)
                if code in (0x29, 0x2D):
                    r |= special(ACC)
                wr |= special(ACC) | special(MAC) | special(STATUS)
                r |= special(STATUS)
            elif code == 0x2E:  # OPMULA
                r |= vf(fs, 0xE) | vf(ft, 0xE)
                wr |= special(ACC) | special(MAC) | special(STATUS)
                r |= special(STATUS)
        self.reads |= r
        self.writes |= wr

    def decode_lower(self, w):
        if w in (0, 0x8000033C):
            return
        op7 = w >> 25
        dest = (w >> 21) & 0xF
        it, is_, id_ = (w >> 16) & 0x1F, (w >> 11) & 0x1F, (w >> 6) & 0x1F
        ft, fs = it, is_
        fsf, ftf = (w >> 21) & 3, (w >> 23) & 3
        imm11 = (w & 0x7FF) - 0x800 if w & 0x400 else w & 0x7FF
        r = 0
        wr = 0
        it &= 0xF
        is_ &= 0xF
        id_ &= 0xF
        if op7 == 0x40:
            low = w & 0x3F
            if low in (0x30, 0x31, 0x34, 0x35):
                r |= vi(is_) | vi(it)
                wr |= vi(id_)
            elif low == 0x32:
                r |= vi(is_)
                wr |= vi(it)
            elif low >= 0x3C:
                code = (w & 3) | ((w >> 4) & 0x7C)
                if code == 0x30:
                    r |= vf(fs, dest)
                    wr |= vf(ft, dest)
                elif code == 0x31:
                    r |= vf(fs, 0xF)
                    wr |= vf(ft, dest)
                elif code in (0x34, 0x36):  # LQI LQD
                    r |= vi(is_)
                    wr |= vf(ft, dest) | vi(is_)
                elif code in (0x35, 0x37):  # SQI SQD
                    r |= vf(fs, dest) | vi(it)
                    wr |= vi(it)
                elif code == 0x38:  # DIV
                    r |= vf_lane(fs, fsf) | vf_lane(ft, ftf)
                    wr |= special(Q) | special(STATUS)
                    r |= special(STATUS)
                elif code == 0x39:
                    r |= vf_lane(ft, ftf)
                    wr |= special(Q) | special(STATUS)
                    r |= special(STATUS)
                elif code == 0x3A:
                    r |= vf_lane(fs, fsf) | vf_lane(ft, ftf)
                    wr |= special(Q) | special(STATUS)
                    r |= special(STATUS)
                elif code == 0x3C:  # MTIR
                    r |= vf_lane(fs, fsf)
                    wr |= vi(it)
                elif code == 0x3D:  # MFIR
                    r |= vi(is_)
                    wr |= vf(ft, dest)
                elif code == 0x3E:  # ILWR
                    r |= vi(is_)
                    wr |= vi(it)
                elif code == 0x3F:  # ISWR
                    r |= vi(is_) | vi(it)
                elif code in (0x40, 0x41):  # RNEXT RGET
                    r |= special(R)
                    wr |= vf(ft, dest) | (special(R) if code == 0x40 else 0)
                elif code in (0x42, 0x43):  # RINIT RXOR
                    r |= vf_lane(fs, fsf) | (special(R) if code == 0x43 else 0)
                    wr |= special(R)
                elif code == 0x64:  # MFP
                    r |= special(P)
                    wr |= vf(ft, dest)
                elif code in (0x68, 0x69):  # XTOP XITOP
                    wr |= vi(it)
                elif code == 0x6C:  # XGKICK
                    r |= vi(is_)
                elif code in (0x70, 0x71, 0x72, 0x73):
                    r |= vf(fs, 0xE)
                    wr |= special(P)
                elif code == 0x76:
                    r |= vf(fs, 0xF)
                    wr |= special(P)
                elif code in (0x74, 0x75):
                    r |= vf(fs, 0xF)
                    wr |= special(P)
                elif code in (0x78, 0x79, 0x7A, 0x7C, 0x7D, 0x7E):
                    r |= vf_lane(fs, fsf)
                    wr |= special(P)
        elif op7 == 0x00:  # LQ
            r |= vi(is_)
            wr |= vf(ft, dest)
        elif op7 == 0x01:  # SQ
            r |= vf(fs, dest) | vi(it)
        elif op7 == 0x04:  # ILW
            r |= vi(is_)
            wr |= vi(it)
        elif op7 == 0x05:  # ISW
            r |= vi(is_) | vi(it)
        elif op7 in (0x08, 0x09):  # IADDIU ISUBIU
            r |= vi(is_)
            wr |= vi(it)
        elif op7 in (0x10, 0x12, 0x13):  # FCEQ FCAND FCOR
            r |= special(CLIP)
            wr |= vi(1)
        elif op7 == 0x11:  # FCSET
            wr |= special(CLIP)
        elif op7 in (0x14, 0x16, 0x17):  # FSEQ FSAND FSOR
            r |= special(STATUS)
            wr |= vi(it)
        elif op7 == 0x15:  # FSSET
            wr |= special(STATUS)
        elif op7 in (0x18, 0x1A, 0x1B):  # FMEQ FMAND FMOR
            r |= special(MAC) | vi(is_)
            wr |= vi(it)
        elif op7 == 0x1C:  # FCGET
            r |= special(CLIP)
            wr |= vi(it)
        elif op7 == 0x20:  # B
            self.kind, self.target = "branch", (self.pc + 8 + imm11 * 8) & 0x3FFF
        elif op7 == 0x21:  # BAL
            self.kind, self.target = "bal", (self.pc + 8 + imm11 * 8) & 0x3FFF
            wr |= vi(it)
        elif op7 == 0x24:  # JR
            self.kind = "jr"
            r |= vi(is_)
        elif op7 == 0x25:  # JALR
            self.kind = "jalr"
            r |= vi(is_)
            wr |= vi(it)
        elif op7 in (0x28, 0x29):
            self.kind, self.target = "cond", (self.pc + 8 + imm11 * 8) & 0x3FFF
            r |= vi(is_) | vi(it)
        elif op7 in (0x2C, 0x2D, 0x2E, 0x2F):
            self.kind, self.target = "cond", (self.pc + 8 + imm11 * 8) & 0x3FFF
            r |= vi(is_)
        self.reads |= r
        self.writes |= wr


def describe(bits):
    out = []
    for reg in range(1, 32):
        lanes = "".join(LANES[lane] for lane in range(4) if bits >> (reg * 4 + lane) & 1)
        if lanes:
            out.append(f"vf{reg:02d}.{lanes}")
    for reg in range(1, 16):
        if bits >> (128 + reg) & 1:
            out.append(f"vi{reg:02d}")
    for bit, name in NAMES.items():
        if bits >> bit & 1:
            out.append(name)
    return " ".join(out) or "(nothing)"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image")
    parser.add_argument("--routines", default="", help="routine entry points a JALR may reach (hex, comma separated)")
    parser.add_argument("--entries", default="0,10,f8,450", help="program entry points (hex)")
    parser.add_argument("--at", default="", help="also print what is live before these pcs (hex)")
    args = parser.parse_args()
    code = open(args.image, "rb").read()
    pairs = {pc: Pair(pc, *struct.unpack_from("<II", code, pc)) for pc in range(0, len(code), 8)}
    routines = [int(x, 16) for x in args.routines.split(",") if x]
    entries = [int(x, 16) for x in args.entries.split(",") if x]

    # Successors: a pair's own next pc, or, for the delay slot of a jump, where it goes.
    succ = {}
    for pc, p in pairs.items():
        nxt = (pc + 8) & 0x3FFF
        prev = pairs.get((pc - 8) & 0x3FFF)
        if prev is not None and prev.ebit:
            succ[pc] = ["END"]
        elif prev is not None and prev.kind:
            k = prev.kind
            if k == "branch":
                succ[pc] = [prev.target]
            elif k == "bal":
                succ[pc] = [prev.target, nxt]  # the callee, then back here
            elif k == "cond":
                succ[pc] = [prev.target, nxt]
            elif k == "jr":
                succ[pc] = []  # back to the caller, which accounts for it
            elif k == "jalr":
                succ[pc] = routines + [nxt]
        else:
            succ[pc] = [nxt]
    live_in = {pc: 0 for pc in pairs}
    changed = True
    while changed:
        changed = False
        end = 0
        for e in entries:
            end |= live_in[e]
        for pc in sorted(pairs, reverse=True):
            out = 0
            for s in succ[pc]:
                out |= end if s == "END" else live_in[s]
            p = pairs[pc]
            new = p.reads | (out & ~p.writes)
            if new != live_in[pc]:
                live_in[pc] = new
                changed = True
    end = 0
    for e in entries:
        end |= live_in[e]
    print(f"live at a program's start (what a run must leave for the next): {describe(end)}")
    for e in entries:
        print(f"  entry {e:04x}: {describe(live_in[e])}")
    for at in [int(x, 16) for x in args.at.split(",") if x]:
        print(f"live before {at:04x}: {describe(live_in[at])}")


if __name__ == "__main__":
    main()
