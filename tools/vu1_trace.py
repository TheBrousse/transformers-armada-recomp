"""Runs a VU1 batch saved by PS2X_VU1_CAPTURE (version 2: with its code) on a Python model of
the fast engine, with real values and, beside each, the expression it was computed from
(docs/RENDERER.md: how the game's routines are read). The run is checked first: the data
memory it leaves must be the one VU1 left, word for word. Then it prints, for the stores
asked for, what each word is made of - the way to read software-pipelined microcode.

    python tools/vu1_trace.py runs/vu1_batch_3.bin                 # the check, and the stores by routine
    python tools/vu1_trace.py runs/vu1_batch_3.bin --vertex 5      # the expressions of vertex 5's qwords
    python tools/vu1_trace.py runs/vu1_batch_3.bin --at 1f90       # of the stores made by the pair at 0x1f90

Values follow VU1: single precision rounded toward zero, operands clamped (exponent 255 as
+-FLT_MAX, denormals as zero), a product and a sum rounded separately. Timing follows the
fast engine (ps2_vu1_fast.cpp): a pair issues a cycle after the one before unless a register
it reads is not ready (FMAC and VF loads 4 cycles, VI loads 4, other VI writes 1), Q and P
land their latency after issue (DIV and SQRT 7, RSQRT 13, the EFU's own), a branch reads the
old value of a VI the pair before it wrote with an IALU instruction.
"""
import argparse
import math
import struct
import sys
from fractions import Fraction

from vu1_batch import Batch

FLT_MAX_BITS = 0x7F7FFFFF
LANES = "xyzw"


# ---- single precision as VU1 does it ----

def to_f(bits):
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


def to_bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def clamp(bits):
    exponent = (bits >> 23) & 0xFF
    if exponent == 0xFF:
        return (bits & 0x80000000) | FLT_MAX_BITS
    if exponent == 0:
        return bits & 0x80000000
    return bits


def rz(exact):
    """An exact result to single precision toward zero; overflow to FLT_MAX; tiny results to zero."""
    if exact == 0.0:
        return 0x80000000 if math.copysign(1.0, exact) < 0 else 0
    sign = 0x80000000 if exact < 0 else 0
    magnitude = abs(exact)
    if magnitude >= 3.4028235677973366e38:
        return sign | FLT_MAX_BITS
    bits = to_bits(magnitude)  # rounded to nearest
    if to_f(bits) > magnitude:
        bits -= 1
    if (bits >> 23) == 0:  # denormal: flushed
        return sign
    return sign | bits


def rz_exact(value):
    """An exact rational result (not zero) to single precision toward zero, without denormals."""
    sign = 0x80000000 if value < 0 else 0
    magnitude = -value if value < 0 else value
    if magnitude >= Fraction(2) ** 128:
        return sign | FLT_MAX_BITS
    exponent = magnitude.numerator.bit_length() - magnitude.denominator.bit_length()
    if Fraction(2) ** exponent > magnitude:
        exponent -= 1
    if exponent < -126:
        return sign
    mantissa = int(magnitude / Fraction(2) ** (exponent - 23))  # toward zero
    if mantissa >= 1 << 24:  # (rounding of the exponent guess)
        exponent += 1
        mantissa >>= 1
    if exponent > 127:
        return sign | FLT_MAX_BITS
    return sign | ((exponent + 127) << 23) | (mantissa & 0x7FFFFF)


def f_add(a, b):
    a, b = clamp(a), clamp(b)
    exact = Fraction(to_f(a)) + Fraction(to_f(b))
    if exact == 0:  # +0 unless both are -0
        return 0x80000000 if (a & b) & 0x80000000 and not (a | b) & 0x7FFFFFFF else 0
    return rz_exact(exact)


def f_sub(a, b):
    return f_add(a, clamp(b) ^ 0x80000000)


def f_mul(a, b):
    a, b = clamp(a), clamp(b)
    exact = Fraction(to_f(a)) * Fraction(to_f(b))
    if exact == 0:
        return (a ^ b) & 0x80000000
    return rz_exact(exact)


def f_div(a, b):
    a, b = clamp(a), clamp(b)
    if to_f(b) == 0.0:
        return ((a ^ b) & 0x80000000) | FLT_MAX_BITS
    exact = Fraction(to_f(a)) / Fraction(to_f(b))
    if exact == 0:
        return (a ^ b) & 0x80000000
    return rz_exact(exact)


FLT_MIN = Fraction(2) ** -126
FLT_MAX = Fraction(to_f(FLT_MAX_BITS))


def mac_lane(kind, a, b, acc, lane):
    """The MAC flag bits an FMAC result sets for one component (x = lane 0): zero, sign,
    underflow and overflow of the exact result (from the operands as read, clamped), in the
    register's order (x is bit 3 of each group of four)."""
    a, b = clamp(a), clamp(b)
    fa, fb = Fraction(to_f(a)), Fraction(to_f(b))
    if kind in ("add", "sub"):
        if kind == "sub":
            b ^= 0x80000000
            fb = -fb
        exact = fa + fb
        negative_zero = (a & b & 0x80000000) != 0
    else:
        product = fa * fb
        product_sign = (a ^ b) & 0x80000000
        if kind == "mul":
            exact = product
            negative_zero = product_sign != 0
        else:
            c = clamp(acc)
            fc = Fraction(to_f(c))
            if kind == "msub":
                product = -product
                product_sign ^= 0x80000000
            exact = fc + product
            negative_zero = (c & 0x80000000) != 0 and product_sign != 0 and product == 0
    magnitude = abs(exact)
    sign = exact < 0 or (exact == 0 and negative_zero)
    under = 0 < magnitude < FLT_MIN
    zero = magnitude == 0 or under
    over = magnitude > FLT_MAX
    shift = 3 - lane
    return (int(zero) << shift) | (int(sign) << (4 + shift)) | (int(under) << (8 + shift)) | (int(over) << (12 + shift))


def status_of(mac):
    """The status register's Z, S, U, O bits (any component) from MAC flags."""
    return (int((mac & 0xF) != 0)) | (int((mac & 0xF0) != 0) << 1) | (int((mac & 0xF00) != 0) << 2) | \
        (int((mac & 0xF000) != 0) << 3)


def f_sqrt(a):
    return rz(math.sqrt(abs(to_f(clamp(a)))))


def int32(bits):
    bits &= 0xFFFFFFFF
    return bits - (1 << 32) if bits & 0x80000000 else bits


def f_max(a, b):  # MAX/MINI compare the bits as sign and magnitude
    ia, ib = int32(a), int32(b)
    both_negative = ia < 0 and ib < 0
    return (min(ia, ib) if both_negative else max(ia, ib)) & 0xFFFFFFFF


def f_min(a, b):
    ia, ib = int32(a), int32(b)
    both_negative = ia < 0 and ib < 0
    return (max(ia, ib) if both_negative else min(ia, ib)) & 0xFFFFFFFF


def ftoi(bits, scale):
    value = to_f(clamp(bits)) * scale
    value = to_f(rz(value)) if value != 0 else value
    if value >= 2147483648.0:
        return 0x7FFFFFFF
    if value < -2147483648.0:
        return 0x80000000
    return int(value) & 0xFFFFFFFF  # toward zero


def itof(bits, scale):
    return rz(to_f(rz(float(int32(bits)))) * scale) if scale != 1.0 else rz(float(int32(bits)))


# ---- expressions ----

class Expressions:
    """Hash-consed expression nodes: ('mem', q, lane), ('reg', name), ('k', bits) or (op, args...)."""

    def __init__(self):
        self.nodes = {}

    def make(self, *node):
        return self.nodes.setdefault(node, node)


E = Expressions()


def const(bits):
    return E.make("k", bits & 0xFFFFFFFF)


class Value:
    __slots__ = ("bits", "expr")

    def __init__(self, bits, expr):
        self.bits = bits & 0xFFFFFFFF
        self.expr = expr


def op(name, fn, *values):
    return Value(fn(*[v.bits for v in values]), E.make(name, *[v.expr for v in values]))


# ---- the machine ----

class Machine:
    def __init__(self, batch):
        b = batch
        self.code = b.code
        self.mem = {}
        for q in range(1024):
            words = struct.unpack_from("<4I", b.data_before, 16 * q)
            self.mem[q] = [Value(words[l], E.make("mem", q, l)) for l in range(4)]
        self.vf = [[Value(b.before.vf_bits[r][l], E.make("reg", f"vf{r:02d}{LANES[l]}")) for l in range(4)] for r in range(32)]
        self.vf[0] = [Value(to_bits(v), const(to_bits(v))) for v in (0.0, 0.0, 0.0, 1.0)]
        self.vi = [v & 0xFFFF for v in b.before.vi]
        self.vi[0] = 0
        self.acc = [Value(to_bits(b.before.acc[l]), E.make("reg", f"acc{LANES[l]}")) for l in range(4)]
        self.q = Value(to_bits(b.before.q), E.make("reg", "q"))
        self.p = Value(to_bits(b.before.p), E.make("reg", "p"))
        self.i = Value(to_bits(b.before.i), E.make("reg", "i"))
        self.clip = b.before.clip & 0xFFFFFF
        self.mac = b.before.mac
        self.status = b.before.status
        self.itop, self.top = b.itop, b.top
        self.pc = b.pc
        self.cycle = 0
        self.vf_ready = [[0] * 4 for _ in range(32)]
        self.vi_ready = [0] * 16
        self.q_pending = None  # (ready, value, status I/D bits)
        self.p_pending = []    # [(ready, value)]
        self.efu_resource = 0
        self.flags = []        # [(ready, kind, value, pc)] kind 'clip' / 'mac'
        self.mac_pc = None     # the pc whose MAC (and status) flags are the current ones
        self.q_pc = None       # the pc of the instruction whose result Q holds
        self.p_pc = None       # ... P
        self.clip_pcs = []     # the pcs of the judgements in the clip flags, newest first
        # Program-order sequence numbers of the producers (DIV/SQRT/RSQRT; EFU; CLIP and FCSET;
        # FMAC results with flags) and the one whose result is visible now: how far behind
        # program order a read is (tools/vu1_recompile.py's lags).
        self.issued = {"q": 0, "p": 0, "clip": 0, "mac": 0}
        self.visible = {"q": -1, "p": -1, "clip": -1, "mac": -1}
        self.backup = None     # (reg, old value) hidden from the next branch
        self.stores = []       # (cycle, pc, q, lane, Value)
        self.kicks = []

    # registers
    def vf_read(self, reg, lane):
        return self.vf[reg][lane]

    def vf_write(self, reg, lanes, values, ready):
        if reg == 0:
            return
        for l in range(4):
            if lanes & (8 >> l):
                self.vf[reg][l] = values[l]
                self.vf_ready[reg][l] = ready

    def vi_write(self, reg, value, latency, backup):
        if reg == 0:
            return
        self.pending_backup = (reg, self.vi[reg]) if backup else None
        self.vi[reg] = value & 0xFFFF
        self.vi_ready[reg] = self.cycle + latency

    def vi_s(self, reg):
        v = self.vi[reg]
        return v - 0x10000 if v & 0x8000 else v

    def branch_vi(self, reg):
        if reg == 0:
            return 0
        if self.backup and self.backup[0] == reg:
            v = self.backup[1]
        else:
            v = self.vi[reg]
        return v - 0x10000 if v & 0x8000 else v

    # memory
    def load(self, q):
        return self.mem[q & 0x3FF]

    def store(self, q, lanes, values, pc):
        q &= 0x3FF
        for l in range(4):
            if lanes & (8 >> l):
                self.mem[q][l] = values[l]
                self.stores.append((self.cycle, pc, q, l, values[l]))

    # events
    def commit(self):
        # what lands by now, in the engine's order: by time, the flags before Q at a tie
        events = [(entry[0], 0, i) for i, entry in enumerate(self.flags) if entry[0] <= self.cycle]
        if self.q_pending and self.q_pending[0] <= self.cycle:
            events.append((self.q_pending[0], 1, 0))
        for ready, order, i in sorted(events):
            if order == 1:
                self.q = self.q_pending[1]
                self.q_pc = self.q_pending[3] if len(self.q_pending) > 3 else None
                if len(self.q_pending) > 4:
                    self.visible["q"] = self.q_pending[4]
                di = self.q_pending[2] & 0x30
                self.status = (self.status & 0xFCF) | di | (di << 6)
                continue
            ready, kind, value, pc = self.flags[i][:4]
            if len(self.flags[i]) > 4:
                self.visible["clip" if kind == "clip" else "mac"] = self.flags[i][4]
            if kind == "clip":
                self.clip = value
                self.clip_pcs = ([pc] + self.clip_pcs)[:4] if not getattr(self, "fcset_pcs", None) or pc not in self.fcset_pcs else [pc]
            elif kind == "sticky":  # FSSET
                self.status = (self.status & 0x03F) | value
            else:
                mac, extra = value
                self.mac = mac
                self.mac_pc = pc
                current = status_of(mac)
                self.status = (self.status & 0xFF0) | current | ((current | extra) << 6)
        if self.q_pending and self.q_pending[0] <= self.cycle:
            self.q_pending = None
        self.flags = [f for f in self.flags if f[0] > self.cycle]
        keep = []
        for entry in self.p_pending:
            if entry[0] <= self.cycle:
                self.p = entry[1]
                self.p_pc = entry[2] if len(entry) > 2 else None
                if len(entry) > 3:
                    self.visible["p"] = entry[3]
            else:
                keep.append(entry)
        self.p_pending = keep


def dest_lanes(d):
    return [l for l in range(4) if d & (8 >> l)]


def run(machine, limit=200000, stop_at=None, hook=None):
    """Runs to the program's end (or, given `stop_at`, until the next pair to run is that pc,
    outside a branch's delay slot); the number of pairs run. `hook(machine, pc)` is called
    before each pair runs (after its stalls and the flags landed by then)."""
    m = machine
    code = m.code
    branch_target = None
    branch_delay = False
    end_after = False
    steps = 0
    while steps < limit:
        if stop_at is not None and m.pc == stop_at and not branch_delay:
            return steps
        steps += 1
        pc = m.pc
        lower, upper = struct.unpack_from("<II", code, pc)
        reads_vf, reads_vi, resource = decode_reads(lower, upper)
        ready = m.cycle
        for reg, lane in reads_vf:
            ready = max(ready, m.vf_ready[reg][lane])
        for reg in reads_vi:
            ready = max(ready, m.vi_ready[reg])
        if resource == "fdiv" and m.q_pending:
            ready = max(ready, m.q_pending[0])
        elif resource == "efu":
            ready = max(ready, m.efu_resource)
        elif resource == "waitp":
            for entry in m.p_pending:
                ready = max(ready, entry[0])
        m.cycle = ready
        m.commit()
        if hook:
            hook(m, pc)
        m.pending_backup = None
        result = execute(m, pc, lower, upper)
        m.backup = m.pending_backup
        m.cycle += 1
        if branch_delay:
            m.pc = branch_target if branch_target is not None else (pc + 8) & 0x3FFF
            branch_delay = False
            branch_target = None
        else:
            m.pc = (pc + 8) & 0x3FFF
        if result is not None:
            branch_delay = True
            branch_target = result if result >= 0 else None
        if end_after:
            break
        if upper & 0x40000000:
            end_after = True
    # the program's end: everything lands
    m.cycle += 1000
    m.commit()
    return steps


def decode_reads(lower, upper):
    """The VF lanes and VI registers a pair reads (for the interlock), and its resource."""
    vf, vi = [], []
    resource = None
    dest = (upper >> 21) & 0xF
    ft, fs = (upper >> 16) & 0x1F, (upper >> 11) & 0x1F
    op_ = upper & 0x3F
    if op_ < 0x3C:
        if op_ <= 0x2F:
            if op_ < 0x1C:
                vf += [(fs, l) for l in dest_lanes(dest)] + [(ft, op_ & 3)]
            elif op_ in (0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2F):
                vf += [(fs, l) for l in dest_lanes(dest)] + [(ft, l) for l in dest_lanes(dest)]
            elif op_ == 0x2E:
                vf += [(fs, l) for l in range(3)] + [(ft, l) for l in range(3)]
            else:
                vf += [(fs, l) for l in dest_lanes(dest)]
    else:
        code = (upper & 3) | ((upper >> 4) & 0x7C)
        if code <= 0x0F or 0x18 <= code <= 0x1B:
            vf += [(fs, l) for l in dest_lanes(dest)] + [(ft, code & 3)]
        elif 0x10 <= code <= 0x17 or code == 0x1D:
            vf += [(fs, l) for l in dest_lanes(dest)]
        elif code == 0x1F:
            vf += [(fs, 0), (fs, 1), (fs, 2), (ft, 3)]
        elif code in (0x28, 0x29, 0x2A, 0x2C, 0x2D):
            vf += [(fs, l) for l in dest_lanes(dest)] + [(ft, l) for l in dest_lanes(dest)]
        elif code == 0x2E:
            vf += [(fs, l) for l in range(3)] + [(ft, l) for l in range(3)]
        elif code in (0x1C, 0x1E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27):
            vf += [(fs, l) for l in dest_lanes(dest)]
    if not upper & 0x80000000 and lower not in (0, 0x8000033C):
        op7 = lower >> 25
        ldest = (lower >> 21) & 0xF
        it, is_ = (lower >> 16) & 0x1F, (lower >> 11) & 0x1F
        fsf, ftf = (lower >> 21) & 3, (lower >> 23) & 3
        if op7 == 0x40:
            low = lower & 0x3F
            if low in (0x30, 0x31, 0x34, 0x35):
                vi += [is_ & 0xF, it & 0xF]
            elif low == 0x32:
                vi += [is_ & 0xF]
            elif low >= 0x3C:
                c = (lower & 3) | ((lower >> 4) & 0x7C)
                if c == 0x30:
                    vf += [(is_, l) for l in dest_lanes(ldest)]
                elif c == 0x31:
                    vf += [(is_, l) for l in range(4)]
                elif c in (0x34, 0x36):
                    vi += [is_ & 0xF]
                elif c in (0x35, 0x37):
                    vf += [(is_, l) for l in dest_lanes(ldest)]
                    vi += [it & 0xF]
                elif c in (0x38, 0x3A):
                    vf += [(is_, fsf), (it, ftf)]
                    resource = "fdiv"
                elif c == 0x39:
                    vf += [(it, ftf)]
                    resource = "fdiv"
                elif c == 0x3B:
                    resource = "fdiv"
                elif c == 0x3C:
                    vf += [(is_, fsf)]
                elif c in (0x3D, 0x3E, 0x3F):
                    vi += [is_ & 0xF] + ([it & 0xF] if c == 0x3F else [])
                elif c in (0x70, 0x71, 0x72, 0x73):
                    vf += [(is_, l) for l in range(3)]
                    resource = "efu"
                elif c == 0x76:
                    vf += [(is_, l) for l in range(4)]
                    resource = "efu"
                elif c in (0x78, 0x79, 0x7A, 0x7C, 0x7D, 0x7E):
                    vf += [(is_, fsf)]
                    resource = "efu"
                elif c in (0x74, 0x75):
                    vf += [(is_, l) for l in range(4)]
                    resource = "efu"
                elif c == 0x7B:
                    resource = "waitp"
                elif c == 0x6C:
                    vi += [is_ & 0xF]
        elif op7 == 0x00:
            vi += [is_ & 0xF]
        elif op7 == 0x01:
            vf += [(is_, l) for l in dest_lanes(ldest)]
            vi += [it & 0xF]
        elif op7 in (0x04, 0x05):
            vi += [is_ & 0xF] + ([it & 0xF] if op7 == 0x05 else [])
        elif op7 in (0x08, 0x09):
            vi += [is_ & 0xF]
        elif op7 in (0x18, 0x1A, 0x1B):
            vi += [is_ & 0xF]
        elif op7 in (0x24, 0x25):
            vi += [is_ & 0xF]
        elif op7 in (0x28, 0x29):
            vi += [is_ & 0xF, it & 0xF]
        elif op7 in (0x2C, 0x2D, 0x2E, 0x2F):
            vi += [is_ & 0xF]
    return [(r, l) for r, l in vf if r != 0], [r for r in vi if r != 0], resource


def bc_value(m, ft, bc):
    return m.vf_read(ft, bc)


def execute(m, pc, lower, upper):
    """One pair: the upper reads, the lower runs, the upper's result lands. Returns a branch
    target (or -1 for a taken branch to the next pair, None for no branch)."""
    dest = (upper >> 21) & 0xF
    ft, fs, fd = (upper >> 16) & 0x1F, (upper >> 11) & 0x1F, (upper >> 6) & 0x1F
    op_ = upper & 0x3F
    up_write = None  # (target 'vf'/'acc', reg, lanes, values)
    flag_entry = None
    acc = m.acc
    ready = m.cycle + 4

    mac = [None]  # the MAC flags of this upper, if it sets them
    extra = [0]   # MADD/MSUB: the product's flags, which go to the sticky ones too

    def arith(kind, a_lane, b_lane, use_acc, to_acc, reg):
        out = list(acc) if to_acc else [m.vf_read(reg, l) for l in range(4)]
        if kind not in ("max", "mini") and dest:
            mac[0] = 0
        for l in dest_lanes(dest):
            a = a_lane(l)
            b = b_lane(l)
            if kind not in ("max", "mini"):
                mac[0] |= mac_lane(kind, a.bits, b.bits, acc[l].bits, l)
                if kind in ("madd", "msub"):
                    extra[0] |= status_of(mac_lane("mul", a.bits, b.bits, 0, l))
            if kind == "add":
                r = op("add", f_add, a, b)
            elif kind == "sub":
                r = op("sub", f_sub, a, b)
            elif kind == "mul":
                r = op("mul", f_mul, a, b)
            elif kind == "madd":
                r = op("add", f_add, acc[l], op("mul", f_mul, a, b))
            elif kind == "msub":
                r = op("sub", f_sub, acc[l], op("mul", f_mul, a, b))
            elif kind == "max":
                r = op("max", f_max, a, b)
            elif kind == "mini":
                r = op("mini", f_min, a, b)
            out[l] = r
        return out

    if op_ < 0x3C and op_ <= 0x2F:
        if op_ < 0x1C:
            kind = ["add", "sub", "madd", "msub", "max", "mini", "mul"][op_ >> 2]
            bc = op_ & 3
            values = arith(kind, lambda l: m.vf_read(fs, l), lambda l: m.vf_read(ft, bc), False, False, fd)
            up_write = ("vf", fd, dest, values)
        else:
            table = {0x1C: ("mul", "q"), 0x1D: ("max", "i"), 0x1E: ("mul", "i"), 0x1F: ("mini", "i"),
                     0x20: ("add", "q"), 0x21: ("madd", "q"), 0x22: ("add", "i"), 0x23: ("madd", "i"),
                     0x24: ("sub", "q"), 0x25: ("msub", "q"), 0x26: ("sub", "i"), 0x27: ("msub", "i"),
                     0x28: ("add", None), 0x29: ("madd", None), 0x2A: ("mul", None), 0x2B: ("max", None),
                     0x2C: ("sub", None), 0x2D: ("msub", None), 0x2F: ("mini", None)}
            if op_ == 0x2E:  # OPMSUB: fd.xyz = acc - fs.yzx * ft.zxy
                out = [m.vf_read(fd, l) for l in range(4)]
                mac[0] = 0
                for l, (a, b) in zip(range(3), ((1, 2), (2, 0), (0, 1))):
                    if dest & (8 >> l):
                        out[l] = op("sub", f_sub, acc[l], op("mul", f_mul, m.vf_read(fs, a), m.vf_read(ft, b)))
                        mac[0] |= mac_lane("msub", m.vf_read(fs, a).bits, m.vf_read(ft, b).bits, acc[l].bits, l)
                        extra[0] |= status_of(mac_lane("mul", m.vf_read(fs, a).bits, m.vf_read(ft, b).bits, 0, l))
                if dest & 1:
                    mac[0] |= 1  # w: +0
                if dest & 1:
                    out[3] = Value(0, const(0))
                up_write = ("vf", fd, dest, out)
            else:
                kind, operand = table[op_]
                if operand == "q":
                    other = lambda l: m.q
                elif operand == "i":
                    other = lambda l: m.i
                else:
                    other = lambda l: m.vf_read(ft, l)
                values = arith(kind, lambda l: m.vf_read(fs, l), other, False, False, fd)
                up_write = ("vf", fd, dest, values)
    elif op_ >= 0x3C:
        code = (upper & 3) | ((upper >> 4) & 0x7C)
        if code <= 0x0F or 0x18 <= code <= 0x1B:
            kind = {0: "add", 1: "sub", 2: "madd", 3: "msub", 6: "mul"}[code >> 2]
            bc = code & 3
            values = arith(kind, lambda l: m.vf_read(fs, l), lambda l: m.vf_read(ft, bc), False, True, 0)
            up_write = ("acc", 0, dest, values)
        elif 0x10 <= code <= 0x17:
            scale = (1.0, 16.0, 4096.0, 32768.0)[code & 3]
            out = [m.vf_read(ft, l) for l in range(4)]
            for l in dest_lanes(dest):
                v = m.vf_read(fs, l)
                if code < 0x14:
                    out[l] = Value(itof(v.bits, 1.0 / scale), E.make(f"itof{(0, 4, 12, 15)[code & 3]}", v.expr))
                else:
                    out[l] = Value(ftoi(v.bits, scale), E.make(f"ftoi{(0, 4, 12, 15)[code & 3]}", v.expr))
            up_write = ("vf", ft, dest, out)
        elif code == 0x1D:  # ABS
            out = [m.vf_read(ft, l) for l in range(4)]
            for l in dest_lanes(dest):
                v = m.vf_read(fs, l)
                out[l] = Value(clamp(v.bits) & 0x7FFFFFFF, E.make("abs", v.expr))
            up_write = ("vf", ft, dest, out)
        elif code == 0x1F:  # CLIP
            w = m.vf_read(ft, 3).bits
            limit = (w & 0x7FFFFFFF) if (w & 0x7F800000) else 0x007FFFFF
            flags = 0
            for l in range(3):
                v = int32(m.vf_read(fs, l).bits)
                if v > limit:
                    flags |= 1 << (2 * l)
                if int32(m.vf_read(fs, l).bits ^ 0x80000000) > limit:
                    flags |= 2 << (2 * l)
            m.working_clip = ((getattr(m, "working_clip", m.clip) << 6) | flags) & 0xFFFFFF
            flag_entry = ("clip", m.working_clip)
        elif code in (0x1C, 0x1E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27):
            kind = {0x1C: "mul", 0x1E: "mul", 0x20: "add", 0x21: "madd", 0x22: "add", 0x23: "madd", 0x24: "sub",
                    0x25: "msub", 0x26: "sub", 0x27: "msub"}[code]
            other = (lambda l: m.q) if code in (0x1C, 0x20, 0x21, 0x24, 0x25) else (lambda l: m.i)
            values = arith(kind, lambda l: m.vf_read(fs, l), other, False, True, 0)
            up_write = ("acc", 0, dest, values)
        elif code in (0x28, 0x29, 0x2A, 0x2C, 0x2D):
            kind = {0x28: "add", 0x29: "madd", 0x2A: "mul", 0x2C: "sub", 0x2D: "msub"}[code]
            values = arith(kind, lambda l: m.vf_read(fs, l), lambda l: m.vf_read(ft, l), False, True, 0)
            up_write = ("acc", 0, dest, values)
        elif code == 0x2E:  # OPMULA
            out = list(acc)
            mac[0] = 0
            for l, (a, b) in zip(range(3), ((1, 2), (2, 0), (0, 1))):
                if dest & (8 >> l):
                    out[l] = op("mul", f_mul, m.vf_read(fs, a), m.vf_read(ft, b))
                    mac[0] |= mac_lane("mul", m.vf_read(fs, a).bits, m.vf_read(ft, b).bits, 0, l)
            if dest & 1:
                mac[0] |= 1  # w: +0
            if dest & 1:
                out[3] = Value(0, const(0))
            up_write = ("acc", 0, dest, out)

    # ---- the lower ----
    branch = None
    lower_vf = None  # (reg, lanes, values) written by the lower
    if upper & 0x80000000:
        m.i = Value(lower, const(lower))
    elif lower not in (0, 0x8000033C):
        op7 = lower >> 25
        ldest = (lower >> 21) & 0xF
        it, is_, id_ = (lower >> 16) & 0x1F, (lower >> 11) & 0x1F, (lower >> 6) & 0x1F
        lft, lfs = it, is_
        it, is_, id_ = it & 0xF, is_ & 0xF, id_ & 0xF
        fsf, ftf = (lower >> 21) & 3, (lower >> 23) & 3
        imm11 = (lower & 0x7FF) - 0x800 if lower & 0x400 else lower & 0x7FF
        target = (pc + 8 + imm11 * 8) & 0x3FFF
        if op7 == 0x40:
            low = lower & 0x3F
            if low == 0x30:
                m.vi_write(id_, m.vi[is_] + m.vi[it], 1, True)
            elif low == 0x31:
                m.vi_write(id_, m.vi[is_] - m.vi[it], 1, True)
            elif low == 0x32:
                imm5 = ((lower >> 6) & 0x1F) - 0x20 if lower & 0x400 else (lower >> 6) & 0x1F
                m.vi_write(it, m.vi[is_] + imm5, 1, True)
            elif low == 0x34:
                m.vi_write(id_, m.vi[is_] & m.vi[it], 1, True)
            elif low == 0x35:
                m.vi_write(id_, m.vi[is_] | m.vi[it], 1, True)
            elif low >= 0x3C:
                c = (lower & 3) | ((lower >> 4) & 0x7C)
                if c == 0x30:  # MOVE
                    lower_vf = (lft, ldest, [m.vf_read(lfs, l) for l in range(4)])
                elif c == 0x31:  # MR32
                    src = [m.vf_read(lfs, l) for l in range(4)]
                    lower_vf = (lft, ldest, [src[1], src[2], src[3], src[0]])
                elif c in (0x34, 0x36):  # LQI LQD
                    if c == 0x36:
                        m.vi_write(is_, m.vi[is_] - 1, 1, True)
                    lower_vf = (lft, ldest, list(m.load(m.vi[is_])))
                    if c == 0x34:
                        m.vi_write(is_, m.vi[is_] + 1, 1, True)
                elif c in (0x35, 0x37):  # SQI SQD
                    if c == 0x37:
                        m.vi_write(it, m.vi[it] - 1, 1, True)
                    m.store(m.vi[it], ldest, [m.vf_read(lfs, l) for l in range(4)], pc)
                    if c == 0x35:
                        m.vi_write(it, m.vi[it] + 1, 1, True)
                elif c in (0x38, 0x39, 0x3A):  # DIV SQRT RSQRT
                    if c == 0x38:
                        num, den = m.vf_read(lfs, fsf), m.vf_read(lft, ftf)
                        v = op("div", f_div, num, den)
                        di = 0
                        if to_f(clamp(den.bits)) == 0:
                            di = 0x10 if to_f(clamp(num.bits)) == 0 else 0x20
                        m.issued["q"] += 1
                        m.q_pending = (m.cycle + 7, v, di, pc, m.issued["q"] - 1)
                    elif c == 0x39:
                        rad = m.vf_read(lft, ftf)
                        v = op("sqrt", f_sqrt, rad)
                        m.issued["q"] += 1
                        m.q_pending = (m.cycle + 7, v, 0x10 if to_f(clamp(rad.bits)) < 0 else 0, pc, m.issued["q"] - 1)
                    else:
                        num, rad = m.vf_read(lfs, fsf), m.vf_read(lft, ftf)
                        den = f_sqrt(rad.bits)
                        di = 0x10 if to_f(clamp(rad.bits)) < 0 else 0
                        if to_f(den) != 0:
                            bits = f_div(num.bits, den)
                        else:
                            bits = (num.bits & 0x80000000) | FLT_MAX_BITS
                            di = 0x10 if to_f(clamp(num.bits)) == 0 else 0x20
                        m.issued["q"] += 1
                        m.q_pending = (m.cycle + 13, Value(bits, E.make("rsqrt", num.expr, rad.expr)), di, pc, m.issued["q"] - 1)
                elif c == 0x3B:
                    pass  # WAITQ: the stall did it
                elif c == 0x3C:  # MTIR
                    m.vi_write(it, m.vf_read(lfs, fsf).bits & 0xFFFF, 1, True)
                elif c == 0x3D:  # MFIR
                    value = m.vi_s(is_) & 0xFFFFFFFF
                    lower_vf = (lft, ldest, [Value(value, E.make("vi", is_, value))] * 4)
                elif c == 0x3E:  # ILWR
                    lane = dest_lanes(ldest)[0]
                    m.vi_write(it, m.load(m.vi[is_])[lane].bits & 0xFFFF, 4, False)
                elif c == 0x3F:  # ISWR
                    v = m.vi[it]
                    m.store(m.vi[is_], ldest, [Value(v, const(v))] * 4, pc)
                elif c == 0x64:  # MFP
                    lower_vf = (lft, ldest, [m.p] * 4)
                elif c in (0x68, 0x69):
                    m.vi_write(it, (m.top if c == 0x68 else m.itop) & 0x3FF, 1, False)
                elif c == 0x6C:  # XGKICK
                    m.kicks.append((m.cycle, m.vi[is_], snapshot_packet(m, m.vi[is_])))
                elif 0x70 <= c <= 0x7E and c != 0x7B:
                    v = efu(m, c, lfs, fsf)
                    latency = {0x70: 11, 0x71: 18, 0x72: 18, 0x73: 24, 0x74: 54, 0x75: 54, 0x76: 12, 0x78: 12, 0x79: 18,
                               0x7A: 12, 0x7C: 29, 0x7D: 54, 0x7E: 44}[c]
                    m.issued["p"] += 1
                    m.p_pending.append((m.cycle + latency, v, pc, m.issued["p"] - 1))
                    m.efu_resource = m.cycle + latency - 1
        elif op7 == 0x00:  # LQ
            lower_vf = (lft, ldest, list(m.load(m.vi[is_] + imm11)))
        elif op7 == 0x01:  # SQ
            m.store(m.vi[it] + imm11, ldest, [m.vf_read(lfs, l) for l in range(4)], pc)
        elif op7 == 0x04:  # ILW
            lane = dest_lanes(ldest)[0]
            m.vi_write(it, m.load(m.vi[is_] + imm11)[lane].bits & 0xFFFF, 4, False)
        elif op7 == 0x05:  # ISW
            v = m.vi[it]
            m.store(m.vi[is_] + imm11, ldest, [Value(v, const(v))] * 4, pc)
        elif op7 in (0x08, 0x09):
            imm15 = (lower & 0x7FF) | ((lower >> 10) & 0x7800)
            m.vi_write(it, m.vi[is_] + (imm15 if op7 == 0x08 else -imm15), 1, True)
        elif op7 in (0x10, 0x12, 0x13):  # FCEQ FCAND FCOR
            imm24 = lower & 0xFFFFFF
            if op7 == 0x10:
                r = int((m.clip & 0xFFFFFF) == imm24)
            elif op7 == 0x12:
                r = int((m.clip & imm24) != 0)
            else:
                r = int(((m.clip | imm24) & 0xFFFFFF) == 0xFFFFFF)
            m.vi_write(1, r, 1, False)
        elif op7 == 0x1C:  # FCGET
            m.vi_write(it, m.clip & 0xFFF, 1, False)
        elif op7 in (0x14, 0x16, 0x17):  # FSEQ FSAND FSOR
            imm12 = (((lower >> 21) & 1) << 11) | (lower & 0x7FF)
            status = m.status & 0xFFF
            r = int(status == imm12) if op7 == 0x14 else (status & imm12 if op7 == 0x16 else status | imm12)
            m.vi_write(it, r, 1, False)
        elif op7 == 0x15:  # FSSET: the sticky bits replaced (lands as an FMAC's flags would)
            imm12 = (((lower >> 21) & 1) << 11) | (lower & 0x7FF)
            m.fsset_cycle = m.cycle
            m.flags.append((m.cycle + 4, "sticky", imm12 & 0xFC0, pc))
        elif op7 == 0x11:  # FCSET: the judgements so far replaced (lands as a CLIP's would)
            imm24 = lower & 0xFFFFFF
            m.working_clip = imm24
            m.fcset_cycle = m.cycle
            m.fcset_pcs = getattr(m, "fcset_pcs", set()) | {pc}
            m.issued["clip"] += 1
            m.flags.append((m.cycle + 4, "clip", imm24, pc, m.issued["clip"] - 1))
        elif op7 in (0x18, 0x1A, 0x1B):  # FMEQ FMAND FMOR
            if op7 == 0x1A:
                m.vi_write(it, m.mac & m.vi[is_], 1, False)
            elif op7 == 0x1B:
                m.vi_write(it, m.mac | m.vi[is_], 1, False)
            else:
                m.vi_write(it, int((m.mac & 0xFFFF) == m.vi[is_]), 1, False)
        elif op7 == 0x20:
            branch = target
        elif op7 == 0x21:
            m.vi_write(it, (pc + 16) // 8, 1, False)
            branch = target
        elif op7 == 0x24:
            branch = (m.branch_vi(is_) * 8) & 0x3FFF
        elif op7 == 0x25:
            dest_pc = (m.branch_vi(is_) * 8) & 0x3FFF
            m.vi_write(it, (pc + 16) // 8, 1, False)
            branch = dest_pc
        elif op7 in (0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
            a = m.branch_vi(is_)
            b_ = m.branch_vi(it)
            taken = {0x28: a == b_, 0x29: a != b_, 0x2C: a < 0, 0x2D: a > 0, 0x2E: a <= 0, 0x2F: a >= 0}[op7]
            branch = target if taken else -1
        else:
            raise NotImplementedError(f"lower op {op7:#x} at {pc:04x} ({lower:08x})")

    # the lower's VF write (lost if the upper writes the same register), then the upper's
    if lower_vf and not (up_write and up_write[0] == "vf" and up_write[1] == lower_vf[0]):
        m.vf_write(lower_vf[0], lower_vf[1], lower_vf[2], m.cycle + 4)
    if up_write:
        if up_write[0] == "vf":
            m.vf_write(up_write[1], up_write[2], up_write[3], ready)
        else:
            for l in dest_lanes(up_write[2]):
                m.acc[l] = up_write[3][l]
    if flag_entry and not (flag_entry[0] == "clip" and getattr(m, "fcset_cycle", None) == m.cycle):
        m.issued[flag_entry[0]] += 1
        m.flags.append((m.cycle + 4, flag_entry[0], flag_entry[1], pc, m.issued[flag_entry[0]] - 1))
    elif mac[0] is not None and getattr(m, "fsset_cycle", None) != m.cycle:
        m.issued["mac"] += 1
        m.flags.append((m.cycle + 4, "mac", (mac[0], extra[0]), pc, m.issued["mac"] - 1))
    return branch


def snapshot_packet(m, start):
    """The packet an XGKICK at qword `start` sends, as it is in memory now (its size from its
    GIF tags): a list of qwords of Values."""
    qwords = []
    at = start & 0x3FF
    while len(qwords) < 1024:
        tag = m.mem[at]
        lo = tag[0].bits | (tag[1].bits << 32)
        nloop = lo & 0x7FFF
        flg = (lo >> 58) & 3
        nreg = ((lo >> 60) & 0xF) or 16
        size = 1 + (nloop * nreg if flg == 0 else ((nloop * nreg + 1) // 2 if flg == 1 else nloop))
        for k in range(size):
            qwords.append(list(m.mem[(at + k) & 0x3FF]))
        at = (at + size) & 0x3FF
        if (lo >> 15) & 1:
            break
    return qwords


def efu(m, c, fs, fsf):
    v = [m.vf_read(fs, l) for l in range(4)]
    def sum3():
        s = op("add", f_add, op("mul", f_mul, v[0], v[0]), op("mul", f_mul, v[1], v[1]))
        return op("add", f_add, s, op("mul", f_mul, v[2], v[2]))
    one = Value(to_bits(1.0), const(to_bits(1.0)))
    if c == 0x70:
        return sum3()
    if c == 0x71:
        s = sum3()
        return op("rcp", lambda a: f_div(to_bits(1.0), a) if to_f(clamp(a)) != 0 else a, s)
    if c == 0x72:
        return op("sqrt", f_sqrt, sum3())
    if c == 0x73:
        length = op("sqrt", f_sqrt, sum3())
        return op("rcp", lambda a: f_div(to_bits(1.0), a) if to_f(a) != 0 else a, length)
    if c == 0x76:
        s = op("add", f_add, Value(0, const(0)), v[0])
        for l in (1, 2, 3):
            s = op("add", f_add, s, v[l])
        return s
    if c == 0x78:
        x = v[fsf]
        return op("esqrt", lambda a: a if to_f(clamp(a)) < 0 else f_sqrt(a), x)
    if c == 0x79:
        x = v[fsf]
        return op("ersqrt", lambda a: clamp(a) if to_f(clamp(a)) < 0 else (f_div(to_bits(1.0), f_sqrt(a)) if f_sqrt(a) & 0x7FFFFFFF else f_sqrt(a)), x)
    if c == 0x7A:
        x = v[fsf]
        return op("ercpr", lambda a: f_div(to_bits(1.0), a) if to_f(clamp(a)) != 0 else clamp(a) & 0x80000000, x)
    raise NotImplementedError(f"EFU {c:#x}")


# ---- printing ----

class Printer:
    def __init__(self, batch, names):
        self.batch = batch
        self.names = names  # {('mem', q, lane): label}
        self.shared = {}
        self.order = []
        self.uses = {}

    def count(self, expr):
        stack = [expr]
        while stack:
            e = stack.pop()
            if e[0] in ("mem", "reg", "k", "vi"):
                continue
            self.uses[e] = self.uses.get(e, 0) + 1
            if self.uses[e] == 1:
                stack.extend(a for a in e[1:] if isinstance(a, tuple))

    def leaf(self, e):
        if e[0] == "mem":
            return self.names.get((e[1], e[2]), f"q{e[1]:x}.{LANES[e[2]]}")
        if e[0] == "reg":
            return e[1]
        if e[0] == "k":
            f = to_f(e[1])
            return f"{f:.9g}" if (e[1] >> 23) & 0xFF not in (0, 0xFF) or e[1] == 0 else f"#{e[1]:08x}"
        if e[0] == "vi":
            return f"vi{e[1]:02d}={e[2]:#x}"
        return None

    def text(self, e, top=True):
        leaf = self.leaf(e)
        if leaf is not None:
            return leaf
        if not top and self.uses.get(e, 0) > 1:
            if e not in self.shared:
                self.shared[e] = f"t{len(self.shared)}"
                self.order.append(e)
            return self.shared[e]
        return f"{e[0]}(" + ", ".join(self.text(a, False) if isinstance(a, tuple) else str(a) for a in e[1:]) + ")"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("batch")
    parser.add_argument("--vertex", type=int, action="append", default=[], help="print this input vertex's qwords")
    parser.add_argument("--at", action="append", default=[], help="print the stores of the pair at this pc (hex)")
    parser.add_argument("--qword", action="append", default=[], help="print this qword's final value (hex)")
    parser.add_argument("--same", action="store_true", help="check that every vertex's qwords are made the same way")
    parser.add_argument("--kick", type=int, action="append", default=[], help="print packet K as it was sent (expressions)")
    parser.add_argument("--kick-qwords", default="0-12", help="which qwords of it (a range)")
    parser.add_argument("--spec", type=int, default=0, metavar="COPIES",
                        help="every word the batch writes, by copy of the vertices (COPIES of them, each 4 qwords of tags "
                             "and 3 a vertex, from ITOP - 3), slot and lane: one formula where all vertices agree")
    args = parser.parse_args()
    b = Batch(args.batch)
    if b.code is None:
        sys.exit("needs a version 2 capture (with the code)")
    m = Machine(b)
    steps = run(m)
    # the check: the data memory VU1 left
    differences = 0
    for q in range(1024):
        after = struct.unpack_from("<4I", b.data_after, 16 * q)
        for l in range(4):
            if m.mem[q][l].bits != after[l]:
                differences += 1
                if differences <= 10:
                    print(f"DIFFERS q{q:x}.{LANES[l]}: model {m.mem[q][l].bits:08x}, VU1 {after[l]:08x}")
    count = b.q(b.itop, "I")[0] & 0x7FFF
    kick_differences = 0
    for k, (cycle, start, packet) in enumerate(m.kicks):
        sent = b.packets[k] if k < len(b.packets) else b""
        made = b"".join(struct.pack("<4I", *[v.bits for v in q]) for q in packet)
        if made != sent:
            kick_differences += 1
            if kick_differences <= 4:
                print(f"KICK {k} differs: {len(made)} bytes made, {len(sent)} sent")
    if len(m.kicks) != len(b.packets):
        print(f"KICKS: {len(m.kicks)} made, {len(b.packets)} sent")
    print(f"{steps} pairs, {m.cycle} cycles, {len(m.stores)} stores, {len(m.kicks)} kicks "
          f"({'as VU1 sent them' if kick_differences == 0 and len(m.kicks) == len(b.packets) else 'DIFFER'}); "
          f"data memory {'as VU1 left it' if differences == 0 else f'{differences} words differ'}")
    by_pc = {}
    for cycle, pc, q, l, v in m.stores:
        by_pc.setdefault(pc, 0)
        by_pc[pc] += 1
    print("stores by pc: " + " ".join(f"{pc:04x}:{n}" for pc, n in sorted(by_pc.items())))

    h = b.itop
    names = {}
    for k in range(count + 4):
        for j, kind in enumerate(("a", "b", "c")):
            for l in range(4):
                names[(h + 1 + 3 * k + j, l)] = f"v{k}.{kind}{LANES[l]}"
    for k in range(int(b.before.vi[13]) if 0 < b.before.vi[13] < 8 else 0):
        entry = b.q(5 + k, "I")
        block = entry[1] & 0x3FF
        for i in range(24):
            for l in range(4):
                names.setdefault((block + i, l), f"blk{k}[{i}].{LANES[l]}")
    if args.same:
        def canonical(expr, k):
            # the expression with vertex k's inputs named as "this vertex", others as they are
            if expr[0] == "mem":
                label = names.get((expr[1], expr[2]), f"q{expr[1]:x}.{LANES[expr[2]]}")
                prefix = f"v{k}."
                return ("in", label[len(prefix):]) if label.startswith(prefix) else ("in", label)
            if expr[0] in ("reg", "k", "vi"):
                return expr if expr[0] != "vi" else ("vi", expr[1])
            return (expr[0],) + tuple(canonical(a, k) if isinstance(a, tuple) else a for a in expr[1:])
        reference = None
        for k in range(count + 3):
            forms = tuple(canonical(m.mem[(h + 1 + 3 * k + j) & 0x3FF][l].expr, k) for j in range(3) for l in range(4))
            if reference is None:
                reference = forms
                first = k
                continue
            same = [f == r for f, r in zip(forms, reference)]
            if not all(same):
                where = ", ".join(f"{'abc'[i // 4]}.{LANES[i % 4]}" for i, s_ in enumerate(same) if not s_)
                print(f"vertex {k}{' (past the end)' if k >= count else ''}: differs from vertex {first} in {where}")
        print(f"(vertices 0-{count - 1}; checked against vertex {first})")
    if args.spec:
        stride = 3 * count + 4
        def place(q):
            o = (q - h) & 0x3FF
            if o >= 0x3FD:
                o -= 0x400
            c, r = divmod(o + 3, stride)
            if c >= args.spec or o < -3:
                return None
            if r < 4:
                return (c, "tag", r)
            k, j = divmod(r - 4, 3)
            return (c, "abc"[j], k)
        def canonical(expr, k):
            if expr[0] == "mem":
                label = names.get((expr[1], expr[2]), f"q{expr[1]:x}.{LANES[expr[2]]}")
                prefix = f"v{k}."
                return ("in", label[len(prefix):]) if k is not None and label.startswith(prefix) else ("in", label)
            if expr[0] in ("reg", "k", "vi"):
                return expr
            return (expr[0],) + tuple(canonical(a, k) if isinstance(a, tuple) else a for a in expr[1:])
        groups = {}
        others = []
        for q in range(1024):
            for l in range(4):
                e = m.mem[q][l].expr
                if e == ("mem", q, l):
                    continue  # not written (or written back as it was)
                where = place(q)
                if where is None:
                    others.append((q, l, e))
                    continue
                c, kind, k = where
                key = (c, kind, k if kind == "tag" else None, l)
                groups.setdefault(key, []).append((k, canonical(e, None if kind == "tag" else k), e))
        printer = Printer(b, names)
        lines = []
        for key in sorted(groups, key=lambda x: (x[0], x[1] != "tag", x[1], x[2] or 0, x[3])):
            c, kind, t, l = key
            items = groups[key]
            forms = {}
            for k, form, e in items:
                forms.setdefault(form, []).append((k, e))
            ranked = sorted(forms.items(), key=lambda x: -len(x[1]))
            for form, members in ranked:
                ks = sorted(k for k, _ in members)
                if kind == "tag":
                    label = f"copy {c} tag {t}.{LANES[l]}"
                else:
                    in_range = [k for k in ks if k < count]
                    label = f"copy {c} {kind}.{LANES[l]} vertices " + (
                        f"{ks[0]}-{ks[-1]}" if ks == list(range(ks[0], ks[-1] + 1)) else ",".join(map(str, ks)))
                e = members[0][1]
                printer.count(e)
                lines.append((label, e))
        for q, l, e in others:
            printer.count(e)
            lines.append((f"q{q:x}.{LANES[l]} (ITOP{q - h:+d})", e))
        texts = [(label, printer.text(e)) for label, e in lines]
        i = 0
        while i < len(printer.order):
            e = printer.order[i]
            print(f"  {printer.shared[e]} = {printer.text(e)}")
            i += 1
        for label, text in texts:
            print(f"{label}: {text}")
        return
    printer = Printer(b, names)
    if args.kick:
        first, last = (int(x) for x in args.kick_qwords.split("-"))
        exprs = []
        for k in args.kick:
            packet = m.kicks[k][2]
            for i in range(first, min(last + 1, len(packet))):
                for l in range(4):
                    exprs.append((f"kick {k} q{i}.{LANES[l]} = {packet[i][l].bits:08x}", packet[i][l].expr))
        for _, e in exprs:
            printer.count(e)
        texts = [(label, printer.text(e)) for label, e in exprs]
        i = 0
        while i < len(printer.order):
            e = printer.order[i]
            print(f"  {printer.shared[e]} = {printer.text(e)}")
            i += 1
        for label, text in texts:
            print(f"{label}: {text}")
        return
    wanted = []
    for v in args.vertex:
        for j in range(3):
            wanted.append(h + 1 + 3 * v + j)
    for q in args.qword:
        wanted.append(int(q, 16))
    exprs = []
    for q in wanted:
        for l in range(4):
            exprs.append((f"q{q:x}.{LANES[l]} = {m.mem[q][l].bits:08x}", m.mem[q][l].expr))
    for at in args.at:
        pc = int(at, 16)
        for cycle, spc, q, l, v in m.stores:
            if spc == pc:
                exprs.append((f"store at {pc:04x} cycle {cycle} q{q:x}.{LANES[l]} = {v.bits:08x}", v.expr))
    for _, e in exprs:
        printer.count(e)
    lines = [(label, printer.text(e)) for label, e in exprs]
    for e in list(printer.order):
        pass
    i = 0
    while i < len(printer.order):  # (names can be added while printing)
        e = printer.order[i]
        print(f"  {printer.shared[e]} = {printer.text(e)}")
        i += 1
    for label, text in lines:
        print(f"{label}: {text}")


if __name__ == "__main__":
    main()
