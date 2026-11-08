"""Finds floats in EE memory that follow an input (debugging aid): compares memory dumps taken
from runs that are identical except for one input, and lists the floats that moved one way
in one dump and the opposite way in the other.

    python tools/mem_scan.py runs/mem_yaw_none runs/mem_yaw_left runs/mem_yaw_right [--also runs/mem_yaw_right2]
                             [--unchanged runs/mem_pitch_up runs/mem_pitch_down] [--min 0.01] [--max 400]

Each folder holds rdram.bin as written by tools/state_dump.py -o. Reported: address, the
value without input, and the change in each dump. With --also, the extra dump's change must
have the sign of the second one's (more of the same input); with --unchanged, the value must
be the same as without input in those dumps (the other axis).
"""
import argparse
from pathlib import Path

import numpy as np


def load(folder):
    return np.fromfile(Path(folder) / "rdram.bin", dtype="<f4")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("none")
    parser.add_argument("minus")
    parser.add_argument("plus")
    parser.add_argument("--also", help="a dump with more of the 'plus' input")
    parser.add_argument("--unchanged", nargs="*", default=[], help="dumps in which the value must not differ")
    parser.add_argument("--min", type=float, default=0.01, help="smallest change that counts")
    parser.add_argument("--max", type=float, default=400.0, help="largest change that counts")
    parser.add_argument("--tolerance", type=float, default=0.35, help="how unequal the two changes may be (0 = exactly opposite)")
    parser.add_argument("--limit", type=int, default=200)
    args = parser.parse_args()

    none = load(args.none)
    with np.errstate(invalid="ignore", over="ignore"):
        minus = load(args.minus) - none
        plus = load(args.plus) - none
        finite = np.isfinite(none) & np.isfinite(minus) & np.isfinite(plus)
        size = np.maximum(np.abs(minus), np.abs(plus))
        keep = finite & (size >= args.min) & (size <= args.max) & (np.sign(minus) == -np.sign(plus))
        keep &= np.abs(minus + plus) <= args.tolerance * size
        keep &= np.abs(none) < 1.0e6
        if args.also:
            also = load(args.also) - none
            keep &= np.isfinite(also) & (np.sign(also) == np.sign(plus)) & (np.abs(also) >= np.abs(plus))
        for folder in args.unchanged:
            keep &= load(folder) == none
    found = np.flatnonzero(keep)
    print(f"{len(found)} floats")
    for index in found[:args.limit]:
        line = f"  {index * 4:08x}  {none[index]:14.6f}  minus {minus[index]:+12.6f}  plus {plus[index]:+12.6f}"
        if args.also:
            line += f"  also {also[index]:+12.6f}"
        print(line)


if __name__ == "__main__":
    main()
