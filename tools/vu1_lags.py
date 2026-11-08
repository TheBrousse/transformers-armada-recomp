"""How far behind program order each timing-dependent read of a routine is (debugging aid for
docs/RENDERER.md): run captured batches on tools/vu1_trace.py's model of the fast engine and,
at every pair in the range that reads Q, P, the clip flags or the MAC/status flags, note how
many producers of that kind were issued after the one whose result it sees (DIV/SQRT/RSQRT for
Q, EFU for P, CLIP/FCSET for the clip flags, flag-setting FMAC instructions for MAC and
status). A program-order rewrite of the routine is exact where each site always has one lag.

    python tools/vu1_lags.py 1380 22a0 runs/cap1380/vu1_batch_*.bin [--json lags.json]
"""
import argparse
import glob
import json
import struct
import sys
from collections import defaultdict

import vu1_trace as T
from vu1_batch import Batch


def reads_of(lower, upper):
    """The kinds of timed state a pair reads: 'q', 'p', 'clip', 'mac', 'status'."""
    out = []
    if upper & 0x80000000:
        return out
    op_ = upper & 0x3F
    if op_ < 0x3C and op_ in (0x1C, 0x20, 0x21, 0x24, 0x25):
        out.append("q")
    if op_ >= 0x3C:
        code = (upper & 3) | ((upper >> 4) & 0x7C)
        if code in (0x1C, 0x20, 0x21, 0x24, 0x25):
            out.append("q")
    op7 = lower >> 25
    if op7 in (0x10, 0x12, 0x13, 0x1C):
        out.append("clip")
    if op7 in (0x14, 0x16, 0x17):
        out.append("status")
    if op7 in (0x18, 0x1A, 0x1B):
        out.append("mac")
    if op7 == 0x40 and (lower & 0x7FF) == 0x67C:  # MFP
        out.append("p")
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("start")
    parser.add_argument("end")
    parser.add_argument("batches", nargs="+")
    parser.add_argument("--json", help="write {pc: {kind: lag}} here when every site has one lag")
    args = parser.parse_args()
    lo, hi = int(args.start, 16), int(args.end, 16)
    lags = defaultdict(lambda: defaultdict(int))
    files = [f for pattern in args.batches for f in glob.glob(pattern)]

    def hook(m, pc):
        if not (lo <= pc < hi):
            return
        lower, upper = struct.unpack_from("<II", m.code, pc)
        for kind in reads_of(lower, upper):
            cat = "mac" if kind == "status" else kind
            lag = m.issued[cat] - 1 - m.visible[cat]
            lags[(pc, kind)][lag] += 1

    runs = 0
    for name in files:
        b = Batch(name)
        if not b.packets:
            continue
        runs += 1
        m = T.Machine(b)
        T.run(m, hook=hook)
    single = True
    table = {}
    for (pc, kind), seen in sorted(lags.items()):
        many = len(seen) > 1
        single = single and not many
        print(f"{pc:04x} {kind:6s} lag {dict(seen)}{'   <-- MORE THAN ONE' if many else ''}")
        table.setdefault(f"{pc:04x}", {})[kind] = max(seen) if not many else sorted(seen)
    print(f"{runs} batches; {'every site has one lag' if single else 'some sites have more than one lag'}")
    if args.json:
        json.dump(table, open(args.json, "w"), indent=1)


if __name__ == "__main__":
    main()
