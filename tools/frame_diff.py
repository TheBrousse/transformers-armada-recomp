"""Compares two frames: how many pixels differ, by how much, and where.

    python tools/frame_diff.py runs/golden_level/shot_6700.png runs/shot_6700.png [-o diff.png]
"""
import argparse

import numpy as np
from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("first")
    parser.add_argument("second")
    parser.add_argument("-o", "--output", help="write the differing pixels (amplified) to this image")
    args = parser.parse_args()

    a = np.asarray(Image.open(args.first).convert("RGB")).astype(int)
    b = np.asarray(Image.open(args.second).convert("RGB")).astype(int)
    if a.shape != b.shape:
        print(f"sizes differ: {a.shape[1]}x{a.shape[0]} and {b.shape[1]}x{b.shape[0]}")
        return
    delta = np.abs(a - b)
    worst = delta.max(axis=2)
    changed = worst > 0
    count = int(changed.sum())
    print(f"{count} of {changed.size} pixels differ ({100.0 * count / changed.size:.3f}%)")
    if count:
        rows = np.flatnonzero(changed.any(axis=1))
        columns = np.flatnonzero(changed.any(axis=0))
        print(f"largest difference {int(worst.max())}, mean over differing pixels {float(worst[changed].mean()):.2f}")
        print(f"rows {rows[0]}-{rows[-1]}, columns {columns[0]}-{columns[-1]}")
        for limit in (1, 2, 4, 8, 16, 32):
            print(f"  more than {limit:2d}: {int((worst > limit).sum())}")
    if args.output:
        Image.fromarray(np.clip(delta * 8, 0, 255).astype(np.uint8)).save(args.output)


if __name__ == "__main__":
    main()
