"""Scans a folder of consecutive frames for flashes: yellow pixel bursts and frames that
differ sharply from both neighbours (flicker). Debugging aid.

    python tools/frame_scan.py runs/seq1 [--top 15]
"""
import argparse
import re
from pathlib import Path

import numpy as np
from PIL import Image


def frame_key(path):
    numbers = re.findall(r"\d+", path.stem)
    return int(numbers[-1]) if numbers else 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("folder")
    parser.add_argument("--top", type=int, default=15)
    args = parser.parse_args()

    paths = sorted(Path(args.folder).glob("*.png"), key=frame_key)
    frames = [np.asarray(Image.open(p).convert("RGB")).astype(np.int16) for p in paths]
    if not frames:
        print("no frames")
        return

    def yellow(frame):
        r, g, b = frame[..., 0], frame[..., 1], frame[..., 2]
        return (r > 170) & (g > 140) & (b < 110) & (r - b > 90) & (g - b > 70)

    counts = [int(yellow(f).sum()) for f in frames]
    median = float(np.median(counts))
    print(f"{len(frames)} frames, yellow pixels: median {median:.0f}, max {max(counts)}")

    # Flicker: a frame far from both neighbours while the neighbours agree.
    spikes = []
    for i in range(1, len(frames) - 1):
        before = np.abs(frames[i] - frames[i - 1]).mean()
        after = np.abs(frames[i] - frames[i + 1]).mean()
        around = np.abs(frames[i + 1] - frames[i - 1]).mean()
        spikes.append((min(before, after) - around, i, before, after, around))
    spikes.sort(reverse=True)

    print("most yellow frames:")
    for i in sorted(range(len(frames)), key=lambda k: -counts[k])[: args.top]:
        mask = yellow(frames[i])
        ys, xs = np.nonzero(mask)
        box = f"x {xs.min()}-{xs.max()} y {ys.min()}-{ys.max()}" if len(xs) else "-"
        print(f"  {paths[i].name}: {counts[i]} yellow ({box})")
    print("flicker candidates (frame differs from both neighbours more than they differ from each other):")
    for score, i, before, after, around in spikes[: args.top]:
        print(f"  {paths[i].name}: score {score:.2f} (prev {before:.2f}, next {after:.2f}, prev-next {around:.2f})")


if __name__ == "__main__":
    main()
