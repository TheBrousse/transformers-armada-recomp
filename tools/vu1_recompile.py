"""VU1 microcode as straight code (docs/RENDERER.md, "Microcode recompiled").

VU1's interlocks make every VF and VI read see the latest write in program order; what its
pipeline lets lag behind are Q (DIV/SQRT/RSQRT), P (the EFU), the clip flags and the MAC and
status flags, read before the latest producer's result has landed - on purpose, in the
game's software-pipelined loops. tools/vu1_lags.py measures, per pair, how many producers
behind program order such a read is. Run in program order with those lags (and the branch's
view of a VI written by the pair before), a program does what VU1 does:

- `check`: runs captured batches so - tools/vu1_trace.py's instruction semantics, every
  result landing at once, the lagged reads taking the producer that many back - and compares
  data memory and packets with VU1's.
- `emit`: writes the program, from its entry points, as one C++ function in program order
  (every pair a block of SSE code; Q, P and flags kept as short histories; the branch's
  VI view; the dispatcher's jumps as a switch) for transformers/src/vu1_native.cpp.

    python tools/vu1_recompile.py check lags.json runs/cap1380/vu1_batch_*.bin
    python tools/vu1_recompile.py emit code.bin lags.json --entries 10,f8 --calls 1380 --name grass47dd > out.inc
"""
import argparse
import glob
import json
import struct
import sys

import vu1_trace as T
from vu1_batch import Batch

FAR = 1 << 40  # a cycle by which everything issued has landed


# ---- the program-order model, on vu1_trace's semantics ----

def run_program_order(m, lags, limit=2000000, hook=None):
    """Runs machine `m` (a vu1_trace.Machine) to the program's end, every pair's results
    landing before the next pair; at the pcs in `lags` ({pc: {kind: lag}}, kinds 'q', 'p',
    'clip', 'mac', 'status') the read sees the producer `lag` back. Returns the pairs run."""
    values = {"q": [], "p": [], "clip": [], "mac": []}
    first = {"q": m.q, "p": m.p, "clip": m.clip, "mac": (m.mac, m.status)}
    code = m.code
    branch_target = None
    branch_delay = False
    end_after = False
    steps = 0

    def lagged(kind, lag):
        cat = "mac" if kind == "status" else kind
        index = m.issued[cat] - 1 - lag
        if index < 0:
            if index != -1:
                raise RuntimeError(f"a {kind} read {lag} back reaches before the run")
            return first[cat]
        return values[cat][index]

    while steps < limit:
        steps += 1
        pc = m.pc
        lower, upper = struct.unpack_from("<II", code, pc)
        site = lags.get(pc)
        current = None
        if site:
            current = (m.q, m.p, m.clip, m.mac, m.status)
            for kind, lag in site.items():
                if not isinstance(lag, int):
                    raise RuntimeError(f"{pc:04x} {kind}: more than one lag {lag}")
                value = lagged(kind, lag)
                if kind == "q":
                    m.q = value
                elif kind == "p":
                    m.p = value
                elif kind == "clip":
                    m.clip = value
                else:
                    m.mac, m.status = value
        if hook:
            hook(m, pc)
        issued = dict(m.issued)
        m.pending_backup = None
        result = T.execute(m, pc, lower, upper)
        m.backup = m.pending_backup
        if current:
            m.q, m.p, m.clip, m.mac, m.status = current
        now = m.cycle
        m.cycle = FAR
        m.commit()
        m.cycle = now + 1
        for cat in values:
            for _ in range(m.issued[cat] - issued[cat]):
                values[cat].append({"q": m.q, "p": m.p, "clip": m.clip, "mac": (m.mac, m.status)}[cat])
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
    return steps


def load_lags(path):
    raw = json.load(open(path))
    return {int(pc, 16): kinds for pc, kinds in raw.items()}


def check(lags_path, patterns):
    lags = load_lags(lags_path)
    files = [f for p in patterns for f in glob.glob(p)]
    good = total = 0
    for name in files:
        b = Batch(name)
        if not b.packets:
            continue
        total += 1
        m = T.Machine(b)
        steps = run_program_order(m, lags)
        differ = [(q, l) for q in range(1024) for l in range(4)
                  if m.mem[q][l].bits != struct.unpack_from("<I", b.data_after, 16 * q + 4 * l)[0]]
        sent = [b"".join(struct.pack("<4I", *[x.bits for x in qw]) for qw in packet) for _, _, packet in m.kicks]
        if not differ and sent == b.packets:
            good += 1
            continue
        first = f" (first q{differ[0][0]:x}.{'xyzw'[differ[0][1]]})" if differ else ""
        print(f"{name}: {steps} pairs, {len(differ)} words differ{first}; packets "
              f"{'as sent' if sent == b.packets else f'DIFFER ({len(sent)} made, {len(b.packets)} sent)'}")
    print(f"{good} of {total} batches as VU1 ran them, in program order with the lags")


# ---- decoding ----

LANE_NAMES = "xyzw"


def blend_mask(dest):
    """VU dest (x = 8) to _mm_blend_ps's (x = 1)."""
    return ((dest >> 3) & 1) | (((dest >> 2) & 1) << 1) | (((dest >> 1) & 1) << 2) | ((dest & 1) << 3)


def flag_setting(upper):
    """Whether the upper is an FMAC instruction that sets the MAC and status flags."""
    if upper & 0x80000000:
        return False
    op_ = upper & 0x3F
    dest = (upper >> 21) & 0xF
    if dest == 0:
        return False
    if op_ < 0x3C:
        if op_ < 0x1C:
            return (op_ >> 2) not in (4, 5)  # not MAX/MINI
        return op_ in (0x1C, 0x1E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2C, 0x2D, 0x2E)
    code = (upper & 3) | ((upper >> 4) & 0x7C)
    return code <= 0x0F or 0x18 <= code <= 0x1C or code in (0x1E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                                                            0x28, 0x29, 0x2A, 0x2C, 0x2D, 0x2E)


def is_clip(upper):
    return not (upper & 0x80000000) and (upper & 0x3F) >= 0x3C and ((upper & 3) | ((upper >> 4) & 0x7C)) == 0x1F


def lower_fields(lower):
    return {
        "op7": lower >> 25, "dest": (lower >> 21) & 0xF, "it": (lower >> 16) & 0x1F, "is": (lower >> 11) & 0x1F,
        "id": (lower >> 6) & 0x1F, "fsf": (lower >> 21) & 3, "ftf": (lower >> 23) & 3,
        "imm11": (lower & 0x7FF) - 0x800 if lower & 0x400 else lower & 0x7FF,
    }


def control_of(pc, lower, upper):
    """('none',) / ('branch', kind, target) / ('jr', reg, link) / ('b', target, link)."""
    if upper & 0x80000000:
        return ("none",)
    f = lower_fields(lower)
    op7 = f["op7"]
    target = (pc + 8 + f["imm11"] * 8) & 0x3FFF
    if op7 == 0x20:
        return ("b", target, None)
    if op7 == 0x21:
        return ("b", target, f["it"] & 0xF)
    if op7 == 0x24:
        return ("jr", f["is"] & 0xF, None)
    if op7 == 0x25:
        return ("jr", f["is"] & 0xF, f["it"] & 0xF)
    if op7 in (0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
        return ("branch", op7, target)
    return ("none",)


# ---- the C++ emitter ----

class Emitter:
    def __init__(self, code, lags, entries, calls, name):
        self.code = code
        self.lags = lags
        self.entries = entries
        self.calls = calls
        self.name = name
        self.pairs = {}        # pc -> (lower, upper)
        self.labels = set(entries) | set(calls)
        self.links = set()     # return addresses (BAL/JALR)
        self.flag_producers = set()
        self.lines = []

    def word(self, pc):
        return struct.unpack_from("<II", self.code, pc)

    def explore(self):
        todo = list(self.entries) + list(self.calls)
        while todo:
            pc = todo.pop()
            while pc not in self.pairs:
                lower, upper = self.word(pc)
                self.pairs[pc] = (lower, upper)
                ctl = control_of(pc, lower, upper)
                if upper & 0x40000000:  # E bit: the pair after it ends the program
                    delay = pc + 8
                    if delay not in self.pairs:
                        self.pairs[delay] = self.word(delay)
                        if control_of(delay, *self.pairs[delay])[0] != "none":
                            raise RuntimeError(f"{delay:04x}: a branch in the E bit's delay slot")
                    break
                if ctl[0] != "none":
                    delay = pc + 8
                    dl, du = self.word(delay)
                    if control_of(delay, dl, du)[0] != "none":
                        raise RuntimeError(f"{delay:04x}: a branch in a delay slot")
                    if du & 0x40000000:
                        raise RuntimeError(f"{delay:04x}: an E bit in a delay slot")
                    self.pairs[delay] = (dl, du)
                    if ctl[0] == "b":
                        self.labels.add(ctl[1])
                        todo.append(ctl[1])
                        if ctl[2] is not None:
                            self.links.add(pc + 16)
                            self.labels.add(pc + 16)
                            todo.append(pc + 16)
                        break
                    if ctl[0] == "jr":
                        if ctl[2] is not None:
                            self.links.add(pc + 16)
                            self.labels.add(pc + 16)
                            todo.append(pc + 16)
                        break
                    self.labels.add(ctl[2])
                    self.labels.add(pc + 16)
                    todo.append(ctl[2])
                    todo.append(pc + 16)
                    break
                pc += 8
        # where status/MAC reads take their value from: the flag-setting FMAC instructions
        # `lag` back in the same straight-line code
        for pc, kinds in self.lags.items():
            for kind, lag in kinds.items():
                if kind not in ("status", "mac") or pc not in self.pairs:
                    continue
                count = -1
                at = pc
                while True:
                    at -= 8
                    if at not in self.pairs:
                        raise RuntimeError(f"{pc:04x}: the {kind} read's producer is not in straight code")
                    if flag_setting(self.pairs[at][1]):
                        count += 1
                        if count == lag:
                            self.flag_producers.add(at)
                            break
                    if at in self.labels:
                        raise RuntimeError(f"{pc:04x}: the {kind} read's producer is behind a label")

    # helpers for one pair's code
    def lag(self, pc, kind):
        site = self.lags.get(pc, {})
        if kind not in site:
            raise RuntimeError(f"{pc:04x}: no measured lag for its {kind} read")
        lag = site[kind]
        if not isinstance(lag, int) or lag > 6:
            raise RuntimeError(f"{pc:04x}: {kind} lag {lag}")
        return lag

    def emit_upper(self, pc, upper, out):
        """Code that computes the upper's result into `u` (VF) / `ua` (ACC); returns
        (target, dest) with target 'vf<n>', 'acc' or None."""
        if upper & 0x80000000 and False:
            pass
        op_ = upper & 0x3F
        dest = (upper >> 21) & 0xF
        ft, fs, fd = (upper >> 16) & 0x1F, (upper >> 11) & 0x1F, (upper >> 6) & 0x1F
        mask = blend_mask(dest)
        flags = pc in self.flag_producers

        def operand(form, bc=None):
            if form == "vec":
                return f"clamp(vf[{ft}])"
            if form == "bc":
                return f"lane(clamp(vf[{ft}]), {bc})"
            if form == "q":
                return f"clamp(_mm_set1_ps(qh[(qn - 1u - {self.lag(pc, 'q')}u) & 7u]))"
            if form == "i":
                return "clamp(_mm_set1_ps(ireg))"
            raise RuntimeError(form)

        def raw_operand(form, bc=None):
            if form == "vec":
                return f"vf[{ft}]"
            if form == "bc":
                return f"lane(vf[{ft}], {bc})"
            if form == "i":
                return "_mm_set1_ps(ireg)"
            raise RuntimeError(form)

        def arith(kind, form, bc, to_acc, target_reg):
            a = f"clamp(vf[{fs}])"
            b = operand(form, bc)
            out.append(f"const __m128 ua_ = {a}, ub_ = {b};")
            if kind == "add":
                expr = "_mm_add_ps(ua_, ub_)"
            elif kind == "sub":
                expr = "_mm_sub_ps(ua_, ub_)"
            elif kind == "mul":
                expr = "_mm_mul_ps(ua_, ub_)"
            elif kind == "madd":
                expr = "_mm_add_ps(clamp(acc), _mm_mul_ps(ua_, ub_))"
            elif kind == "msub":
                expr = "_mm_sub_ps(clamp(acc), _mm_mul_ps(ua_, ub_))"
            else:
                raise RuntimeError(kind)
            out.append(f"const __m128 u = {expr};")
            if flags:
                out.append(f"pushFlags(fh, fn, fmacFlags({['add', 'sub', 'mul', 'madd', 'msub'].index(kind)}, ua_, ub_, clamp(acc), {dest}));")
            return ("acc" if to_acc else f"vf{target_reg}", dest)

        if op_ < 0x3C:
            if op_ < 0x1C:
                kind = ["add", "sub", "madd", "msub", "max", "mini", "mul"][op_ >> 2]
                bc = op_ & 3
                if kind in ("max", "mini"):
                    fn = "maxBits" if kind == "max" else "mini"
                    out.append(f"const __m128 u = {fn}(vf[{fs}], {raw_operand('bc', bc)});")
                    return (f"vf{fd}", dest)
                return arith(kind, "bc", bc, False, fd)
            table = {0x1C: ("mul", "q"), 0x1D: ("max", "i"), 0x1E: ("mul", "i"), 0x1F: ("mini", "i"),
                     0x20: ("add", "q"), 0x21: ("madd", "q"), 0x22: ("add", "i"), 0x23: ("madd", "i"),
                     0x24: ("sub", "q"), 0x25: ("msub", "q"), 0x26: ("sub", "i"), 0x27: ("msub", "i"),
                     0x28: ("add", "vec"), 0x29: ("madd", "vec"), 0x2A: ("mul", "vec"), 0x2B: ("max", "vec"),
                     0x2C: ("sub", "vec"), 0x2D: ("msub", "vec"), 0x2F: ("mini", "vec")}
            if op_ == 0x2E:  # OPMSUB
                out.append(f"const __m128 ua_ = _mm_shuffle_ps(clamp(vf[{fs}]), clamp(vf[{fs}]), _MM_SHUFFLE(3, 0, 2, 1));")
                out.append(f"const __m128 ub_ = _mm_shuffle_ps(clamp(vf[{ft}]), clamp(vf[{ft}]), _MM_SHUFFLE(3, 1, 0, 2));")
                out.append("const __m128 u = _mm_blend_ps(_mm_sub_ps(clamp(acc), _mm_mul_ps(ua_, ub_)), _mm_setzero_ps(), 0x8);")
                if flags:
                    out.append(f"pushFlags(fh, fn, fmacFlags(5, ua_, ub_, clamp(acc), {dest}));")
                return (f"vf{fd}", dest)
            if op_ not in table:
                raise RuntimeError(f"{pc:04x}: upper op {op_:#x}")
            kind, form = table[op_]
            if kind in ("max", "mini"):
                fn = "maxBits" if kind == "max" else "mini"
                out.append(f"const __m128 u = {fn}(vf[{fs}], {raw_operand(form)});")
                return (f"vf{fd}", dest)
            return arith(kind, form, None, False, fd)
        code = (upper & 3) | ((upper >> 4) & 0x7C)
        if code <= 0x0F or 0x18 <= code <= 0x1B:
            kind = {0: "add", 1: "sub", 2: "madd", 3: "msub", 6: "mul"}[code >> 2]
            return arith(kind, "bc", code & 3, True, None)
        if 0x10 <= code <= 0x17:
            scale = (1, 16, 4096, 32768)[code & 3]
            if code < 0x14:
                expr = "_mm_cvtepi32_ps(_mm_castps_si128(vf[%d]))" % fs
                if scale != 1:
                    expr = f"_mm_mul_ps({expr}, _mm_set1_ps(1.0f / {scale}.0f))"
                out.append(f"const __m128 u = {expr};")
            else:
                out.append(f"const __m128 u = _mm_castsi128_ps(ftoi(vf[{fs}], {scale}.0f));")
            return (f"vf{ft}", dest)
        if code == 0x1D:  # ABS
            out.append(f"const __m128 u = _mm_andnot_ps(_mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000u))), clamp(vf[{fs}]));")
            return (f"vf{ft}", dest)
        if code == 0x1F:  # CLIP
            out.append(f"const uint32_t judged = clipFlags(_mm_blend_ps(vf[{fs}], lane(vf[{ft}], 3), 0x8));")
            return ("clip", None)
        if code in (0x1C, 0x1E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27):
            kind = {0x1C: "mul", 0x1E: "mul", 0x20: "add", 0x21: "madd", 0x22: "add", 0x23: "madd", 0x24: "sub",
                    0x25: "msub", 0x26: "sub", 0x27: "msub"}[code]
            form = "q" if code in (0x1C, 0x20, 0x21, 0x24, 0x25) else "i"
            return arith(kind, form, None, True, None)
        if code in (0x28, 0x29, 0x2A, 0x2C, 0x2D):
            kind = {0x28: "add", 0x29: "madd", 0x2A: "mul", 0x2C: "sub", 0x2D: "msub"}[code]
            return arith(kind, "vec", None, True, None)
        if code == 0x2E:  # OPMULA
            out.append(f"const __m128 ua_ = _mm_shuffle_ps(clamp(vf[{fs}]), clamp(vf[{fs}]), _MM_SHUFFLE(3, 0, 2, 1));")
            out.append(f"const __m128 ub_ = _mm_shuffle_ps(clamp(vf[{ft}]), clamp(vf[{ft}]), _MM_SHUFFLE(3, 1, 0, 2));")
            out.append("const __m128 u = _mm_blend_ps(_mm_mul_ps(ua_, ub_), _mm_setzero_ps(), 0x8);")
            if flags:
                out.append(f"pushFlags(fh, fn, fmacFlags(6, ua_, ub_, clamp(acc), {dest}));")
            return ("acc", dest)
        if code in (0x2F, 0x30):
            return (None, None)
        raise RuntimeError(f"{pc:04x}: upper special {code:#x}")

    def emit_lower(self, pc, lower, upper, out):
        """Code for the lower; returns (vf write (reg, dest, expr) or None, backup reg or None,
        control)."""
        if upper & 0x80000000:  # LOI
            out.append(f"ireg = fromBits(0x{lower:08x}u);")
            return None, None, ("none",)
        if lower in (0, 0x8000033C):
            return None, None, ("none",)
        f = lower_fields(lower)
        op7 = f["op7"]
        it, is_, id_ = f["it"] & 0xF, f["is"] & 0xF, f["id"] & 0xF
        lft, lfs = f["it"], f["is"]
        dest = f["dest"]
        mask = blend_mask(dest)
        imm11 = f["imm11"]
        lanes = [l for l in range(4) if dest & (8 >> l)]

        def vi_set(reg, expr, backup):
            if reg == 0:
                out.append(f"(void)({expr});")
                return None
            if backup:
                out.append(f"{{ const int32_t v_ = static_cast<int16_t>({expr}); bkVal = vi[{reg}]; vi[{reg}] = v_; }}")
                return reg
            out.append(f"vi[{reg}] = static_cast<int16_t>({expr});")
            return None

        if op7 == 0x40:
            low = lower & 0x3F
            if low == 0x30:
                return None, vi_set(id_, f"vi[{is_}] + vi[{it}]", True), ("none",)
            if low == 0x31:
                return None, vi_set(id_, f"vi[{is_}] - vi[{it}]", True), ("none",)
            if low == 0x32:
                imm5 = ((lower >> 6) & 0x1F) - 0x20 if lower & 0x400 else (lower >> 6) & 0x1F
                return None, vi_set(it, f"vi[{is_}] + ({imm5})", True), ("none",)
            if low == 0x34:
                return None, vi_set(id_, f"vi[{is_}] & vi[{it}]", True), ("none",)
            if low == 0x35:
                return None, vi_set(id_, f"vi[{is_}] | vi[{it}]", True), ("none",)
            if low < 0x3C:
                raise RuntimeError(f"{pc:04x}: lower special {low:#x}")
            c = (lower & 3) | ((lower >> 4) & 0x7C)
            if c == 0x30:  # MOVE
                return (lft, dest, f"vf[{lfs}]"), None, ("none",)
            if c == 0x31:  # MR32
                return (lft, dest, f"_mm_shuffle_ps(vf[{lfs}], vf[{lfs}], _MM_SHUFFLE(0, 3, 2, 1))"), None, ("none",)
            if c in (0x34, 0x36):  # LQI LQD
                if c == 0x36:
                    out.append(f"bkVal = vi[{is_}]; vi[{is_}] = static_cast<int16_t>(vi[{is_}] - 1);")
                out.append(f"const __m128 loaded_ = loadQ(data, static_cast<uint32_t>(vi[{is_}]));")
                if c == 0x34:
                    out.append(f"bkVal = vi[{is_}]; vi[{is_}] = static_cast<int16_t>(vi[{is_}] + 1);")
                return (lft, dest, "loaded_"), (is_ if is_ else None), ("none",)
            if c in (0x35, 0x37):  # SQI SQD
                if c == 0x37:
                    out.append(f"bkVal = vi[{it}]; vi[{it}] = static_cast<int16_t>(vi[{it}] - 1);")
                out.append(f"storeMasked(data, static_cast<uint32_t>(vi[{it}]), vf[{lfs}], {mask});")
                if c == 0x35:
                    out.append(f"bkVal = vi[{it}]; vi[{it}] = static_cast<int16_t>(vi[{it}] + 1);")
                return None, (it if it else None), ("none",)
            if c == 0x38:  # DIV
                out.append(f"pushQ(qh, qn, _mm_cvtss_f32(divide(lane(vf[{lfs}], {f['fsf']}), lane(vf[{lft}], {f['ftf']}))));")
                return None, None, ("none",)
            if c == 0x39:  # SQRT
                out.append(f"pushQ(qh, qn, _mm_cvtss_f32(_mm_sqrt_ps(_mm_andnot_ps(signMask(), clamp(lane(vf[{lft}], {f['ftf']}))))));")
                return None, None, ("none",)
            if c == 0x3A:  # RSQRT
                out.append(f"pushQ(qh, qn, _mm_cvtss_f32(rsqrtDivide(lane(vf[{lfs}], {f['fsf']}), lane(vf[{lft}], {f['ftf']}))));")
                return None, None, ("none",)
            if c == 0x3B:  # WAITQ
                return None, None, ("none",)
            if c == 0x3C:  # MTIR
                fsf = f["fsf"]
                return None, vi_set(it, f"_mm_extract_epi32(_mm_castps_si128(vf[{lfs}]), {fsf}) & 0xFFFF", True), ("none",)
            if c == 0x3D:  # MFIR
                return (lft, dest, f"_mm_castsi128_ps(_mm_set1_epi32(static_cast<int16_t>(vi[{is_}])))"), None, ("none",)
            if c == 0x3E:  # ILWR
                lane_ = lanes[0]
                return None, vi_set(it, f"word(data, static_cast<uint32_t>(vi[{is_}]), {lane_}) & 0xFFFF", False), ("none",)
            if c == 0x3F:  # ISWR
                for lane_ in lanes:
                    out.append(f"storeHalf(data, static_cast<uint32_t>(vi[{is_}]), {lane_}, static_cast<uint32_t>(vi[{it}]));")
                return None, None, ("none",)
            if c == 0x64:  # MFP
                return (lft, dest, f"_mm_set1_ps(ph[(pn - 1u - {self.lag(pc, 'p')}u) & 7u])"), None, ("none",)
            if c in (0x68, 0x69):  # XTOP XITOP
                field = "top" if c == 0x68 else "itop"
                return None, vi_set(it, f"static_cast<int32_t>(state.{field} & 0x3FFu)", False), ("none",)
            if c == 0x6C:  # XGKICK
                out.append(f"kickAt(run, data, static_cast<uint32_t>(vi[{is_}]) & 0xFFFFu);")
                return None, None, ("none",)
            if c == 0x72:  # ELENG
                out.append(f"pushP(ph, pn, eleng(vf[{lfs}]));")
                return None, None, ("none",)
            if c == 0x73:  # ERLENG
                out.append(f"pushP(ph, pn, erleng(vf[{lfs}]));")
                return None, None, ("none",)
            raise RuntimeError(f"{pc:04x}: lower special {c:#x}")
        if op7 == 0x00:  # LQ
            return (lft, dest, f"loadQ(data, static_cast<uint32_t>(vi[{is_}] + ({imm11})))"), None, ("none",)
        if op7 == 0x01:  # SQ
            out.append(f"storeMasked(data, static_cast<uint32_t>(vi[{it}] + ({imm11})), vf[{lfs}], {mask});")
            return None, None, ("none",)
        if op7 == 0x04:  # ILW
            return None, vi_set(it, f"word(data, static_cast<uint32_t>(vi[{is_}] + ({imm11})), {lanes[0]}) & 0xFFFF", False), ("none",)
        if op7 == 0x05:  # ISW
            for lane_ in lanes:
                out.append(f"storeHalf(data, static_cast<uint32_t>(vi[{is_}] + ({imm11})), {lane_}, static_cast<uint32_t>(vi[{it}]));")
            return None, None, ("none",)
        if op7 in (0x08, 0x09):
            imm15 = (lower & 0x7FF) | ((lower >> 10) & 0x7800)
            sign = "+" if op7 == 0x08 else "-"
            return None, vi_set(it, f"vi[{is_}] {sign} {imm15}", True), ("none",)
        if op7 == 0x11:  # FCSET
            out.append(f"cf = 0x{lower & 0xFFFFFF:06x}u; pushClip(ch, cn, cf);")
            return None, None, ("none",)
        if op7 in (0x10, 0x12, 0x13):  # FCEQ FCAND FCOR
            imm24 = lower & 0xFFFFFF
            seen = f"ch[(cn - 1u - {self.lag(pc, 'clip')}u) & 7u]"
            expr = {0x10: f"({seen} & 0xFFFFFFu) == 0x{imm24:06x}u",
                    0x12: f"({seen} & 0x{imm24:06x}u) != 0u",
                    0x13: f"(({seen} | 0x{imm24:06x}u) & 0xFFFFFFu) == 0xFFFFFFu"}[op7]
            return None, vi_set(1, f"({expr}) ? 1 : 0", False), ("none",)
        if op7 == 0x1C:  # FCGET
            return None, vi_set(it, f"static_cast<int32_t>(ch[(cn - 1u - {self.lag(pc, 'clip')}u) & 7u] & 0xFFFu)", False), ("none",)
        if op7 in (0x14, 0x16, 0x17):  # FSEQ FSAND FSOR
            imm12 = (((lower >> 21) & 1) << 11) | (lower & 0x7FF)
            if imm12 & ~0x00F and op7 != 0x14:
                raise RuntimeError(f"{pc:04x}: a status read of sticky or I/D bits")
            seen = f"(fh[(fn - 1u - {self.lag(pc, 'status')}u) & 7u] >> 16)"
            expr = {0x14: f"static_cast<int32_t>({seen} == 0x{imm12:03x}u)",
                    0x16: f"static_cast<int32_t>({seen} & 0x{imm12:03x}u)",
                    0x17: f"static_cast<int32_t>({seen} | 0x{imm12:03x}u)"}[op7]
            return None, vi_set(it, expr, False), ("none",)
        if op7 in (0x18, 0x1A, 0x1B):  # FMEQ FMAND FMOR
            seen = f"(fh[(fn - 1u - {self.lag(pc, 'mac')}u) & 7u] & 0xFFFFu)"
            expr = {0x18: f"static_cast<int32_t>({seen} == static_cast<uint32_t>(vi[{is_}] & 0xFFFF))",
                    0x1A: f"static_cast<int32_t>({seen} & static_cast<uint32_t>(vi[{is_}]))",
                    0x1B: f"static_cast<int32_t>({seen} | static_cast<uint32_t>(vi[{is_}]))"}[op7]
            return None, vi_set(it, expr, False), ("none",)
        ctl = control_of(pc, lower, upper)
        if ctl[0] == "none":
            raise RuntimeError(f"{pc:04x}: lower op {op7:#x}")
        return None, None, ctl

    def emit_pair(self, pc, out, inline_delay=False):
        lower, upper = self.pairs[pc]
        out.append(f"// {pc:04x}")
        out.append("{")
        body = []
        target, dest = (None, None)
        if not (upper & 0x80000000) or True:
            if upper & 0x80000000:
                # the upper still runs with the old I
                pass
            target, dest = self.emit_upper(pc, upper, body)
        lvf, backup, ctl = self.emit_lower(pc, lower, upper, body)
        # branch decision (reads VIs as the branch sees them) comes before the writes of this pair
        if lvf is not None:
            reg, ldest, expr = lvf
            body.append(f"const __m128 l_ = {expr};")
        if lvf is not None and reg != 0 and not (target == f"vf{reg}"):
            body.append(f"vf[{reg}] = _mm_blend_ps(vf[{reg}], l_, {blend_mask(ldest)});")
        if target is not None:
            if target == "acc":
                body.append(f"acc = _mm_blend_ps(acc, u, {blend_mask(dest)});")
            elif target == "clip":
                body.append("cf = ((cf << 6) | judged) & 0xFFFFFFu; pushClip(ch, cn, cf);")
            else:
                reg = int(target[2:])
                if reg != 0:
                    body.append(f"vf[{reg}] = _mm_blend_ps(vf[{reg}], u, {blend_mask(dest)});")
        body.append(f"bkReg = {backup if backup is not None else -1};")
        out.extend("    " + line for line in body)
        out.append("}")
        return ctl

    def branch_vi(self, reg):
        return f"(bkReg == {reg} ? bkVal : vi[{reg}])" if reg else "0"

    def emit(self):
        self.explore()
        out = self.lines
        out.append(f"// Generated by tools/vu1_recompile.py: {self.name}, {len(self.pairs)} pairs. Do not edit.")
        out.append(f"bool {self.name}(VU1State &state, uint8_t *data, ps2vu1native::Run &run)")
        out.append("{")
        out.append("    __m128 vf[32];")
        out.append("    for (int r = 0; r < 32; ++r) vf[r] = _mm_loadu_ps(state.vf[r]);")
        out.append("    vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);")
        out.append("    int32_t vi[16];")
        out.append("    for (int r = 0; r < 16; ++r) vi[r] = static_cast<int16_t>(state.vi[r]);")
        out.append("    vi[0] = 0;")
        out.append("    __m128 acc = _mm_loadu_ps(state.acc);")
        out.append("    float ireg = state.i;")
        out.append("    float qh[8]; uint32_t qn = 0; qh[7] = state.q;")
        out.append("    float ph[8]; uint32_t pn = 0; ph[7] = state.p;")
        out.append("    uint32_t cf = state.clip & 0xFFFFFFu; uint32_t ch[8]; uint32_t cn = 0; ch[7] = cf;")
        out.append("    uint32_t fh[8]; uint32_t fn = 0; fh[7] = (state.status << 16) | (state.mac & 0xFFFFu);")
        out.append("    int bkReg = -1; int32_t bkVal = 0;")
        out.append("    uint32_t pc = state.pc;")
        out.append("    (void)fn; (void)fh; (void)cn; (void)ch; (void)pn; (void)ph; (void)qn; (void)qh; (void)ireg; (void)bkVal;")
        out.append("dispatch:")
        out.append("    switch (pc)")
        out.append("    {")
        for target in sorted(set(self.entries) | set(self.calls) | self.links):
            out.append(f"    case 0x{target:04x}u: goto L_{target:04x};")
        out.append("    default: return false; // (an unknown jump: not taken by the batches let in)")
        out.append("    }")
        ordered = sorted(self.pairs)
        falls_in = False  # whether the pair before falls through to this one
        for index, pc in enumerate(ordered):
            if pc not in self.labels and not falls_in:
                continue  # only ever run as a delay slot, emitted with its branch
            if pc in self.labels:
                out.append(f"L_{pc:04x}:")
            lower, upper = self.pairs[pc]
            ctl = control_of(pc, lower, upper)
            falls_in = False
            if upper & 0x40000000:
                self.emit_pair(pc, out)
                self.emit_pair(pc + 8, out)
                out.append(f"    pc = 0x{(pc + 16) & 0x3FFF:04x}u; goto done;")
                continue
            if ctl[0] == "none":
                self.emit_pair(pc, out)
                nxt = pc + 8
                if nxt not in self.pairs:
                    raise RuntimeError(f"{pc:04x}: falls through to {nxt:04x}, which was not explored")
                if index + 1 < len(ordered) and ordered[index + 1] == nxt:
                    falls_in = True
                    continue
                out.append(f"    goto L_{nxt:04x};")
                continue
            # a branch: its decision, then its delay slot, then the jump
            if ctl[0] == "branch":
                op7, target = ctl[1], ctl[2]
                f = lower_fields(lower)
                a = self.branch_vi(f["is"] & 0xF)
                b = self.branch_vi(f["it"] & 0xF)
                cond = {0x28: f"{a} == {b}", 0x29: f"{a} != {b}", 0x2C: f"{a} < 0", 0x2D: f"{a} > 0",
                        0x2E: f"{a} <= 0", 0x2F: f"{a} >= 0"}[op7]
                out.append("{")
                out.append(f"    const bool taken = {cond};")
                self.emit_pair(pc, out)
                self.emit_pair(pc + 8, out)
                out.append(f"    if (taken) goto L_{target:04x};")
                out.append(f"    goto L_{(pc + 16) & 0x3FFF:04x};")
                out.append("}")
            elif ctl[0] == "b":
                target, link = ctl[1], ctl[2]
                out.append("{")
                if link is not None and link != 0:
                    out.append(f"    vi[{link}] = 0x{(pc + 16) >> 3:x};")
                self.emit_pair(pc, out)
                self.emit_pair(pc + 8, out)
                out.append(f"    goto L_{target:04x};")
                out.append("}")
            else:  # jr / jalr
                reg, link = ctl[1], ctl[2]
                out.append("{")
                out.append(f"    const uint32_t target = (static_cast<uint32_t>({self.branch_vi(reg)}) * 8u) & 0x3FFFu;")
                if link is not None and link != 0:
                    out.append(f"    vi[{link}] = 0x{(pc + 16) >> 3:x};")
                self.emit_pair(pc, out)
                self.emit_pair(pc + 8, out)
                out.append("    pc = target; goto dispatch;")
                out.append("}")
        out.append("done:")
        out.append("    for (int r = 1; r < 32; ++r) _mm_storeu_ps(state.vf[r], vf[r]);")
        out.append("    for (int r = 1; r < 16; ++r) state.vi[r] = vi[r];")
        out.append("    _mm_storeu_ps(state.acc, acc);")
        out.append("    state.i = ireg; state.q = qh[(qn - 1u) & 7u]; state.p = ph[(pn - 1u) & 7u]; state.clip = cf;")
        out.append("    state.pc = pc;")
        out.append("    return true;")
        out.append("}")
        return "\n".join(out) + "\n"


def emit_bytes(code, lags_path, entries, calls, name):
    """emit() with the microcode given as bytes (tools/make_vu1_grass.py: assembled from the executable)."""
    return Emitter(code, load_lags(lags_path), entries, calls, name).emit()


def emit(code_path, lags_path, entries, calls, name):
    code = open(code_path, "rb").read()
    lags = load_lags(lags_path)
    return Emitter(code, lags, entries, calls, name).emit()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    c = sub.add_parser("check", help="run captured batches in program order with the lags")
    c.add_argument("lags")
    c.add_argument("batches", nargs="+")
    e = sub.add_parser("emit", help="write the program as C++")
    e.add_argument("code")
    e.add_argument("lags")
    e.add_argument("--entries", default="10,f8")
    e.add_argument("--calls", default="", help="routine entry points the dispatcher may jump to (hex)")
    e.add_argument("--name", required=True)
    args = parser.parse_args()
    if args.command == "check":
        check(args.lags, args.batches)
    else:
        entries = [int(x, 16) for x in args.entries.split(",") if x]
        calls = [int(x, 16) for x in args.calls.split(",") if x]
        sys.stdout.write(emit(args.code, args.lags, entries, calls, args.name))


if __name__ == "__main__":
    main()
