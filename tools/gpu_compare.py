"""Compares the GPU renderer's pictures with the software renderer's (PS2X_GPU=1 with
PS2X_SHOT_VSYNCS writes runs/shot_<tick>.png and runs/shot_<tick>_gpu.png), and puts them side
by side: software, GPU, and where they differ.

    python tools\\gpu_compare.py [runs\\folder] [--sheet out.png]
"""
import argparse
import re
from pathlib import Path

import numpy as np
from PIL import Image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("folder", nargs="?", default="runs")
    parser.add_argument("--sheet", default=None, help="side-by-side image of all pairs")
    args = parser.parse_args()
    folder = Path(args.folder)
    rows = []
    for gpu in sorted(folder.glob("shot_*_gpu.png"), key=lambda p: int(re.findall(r"\d+", p.stem)[0])):
        tick = re.findall(r"\d+", gpu.stem)[0]
        cpu = folder / f"shot_{tick}.png"
        if not cpu.exists():
            print(f"tick {tick}: no software picture")
            continue
        a = np.asarray(Image.open(cpu).convert("RGB"), dtype=np.int16)
        b = np.asarray(Image.open(gpu).convert("RGB"), dtype=np.int16)
        # A picture drawn at a scale (PS2X_GPU_SCALE): its n x n blocks averaged first.
        n = b.shape[1] // a.shape[1] if a.shape[1] else 1
        if n > 1 and b.shape[0] // n >= a.shape[0] - 1:
            hb, wb = b.shape[0] // n, b.shape[1] // n
            b = b[:hb * n, :wb * n].reshape(hb, n, wb, n, 3).mean(axis=(1, 3)).round().astype(np.int16)
        h, w = min(a.shape[0], b.shape[0]), min(a.shape[1], b.shape[1])
        if a.shape != b.shape:
            print(f"tick {tick}: sizes differ {a.shape[1]}x{a.shape[0]} (software) / {b.shape[1]}x{b.shape[0]} (GPU)")
        a, b = a[:h, :w], b[:h, :w]
        diff = np.abs(a - b).max(axis=2)
        print(f"tick {tick}: mean difference {np.abs(a - b).mean():.2f}, pixels off by more than 8: "
              f"{100.0 * (diff > 8).mean():.1f}%, by more than 32: {100.0 * (diff > 32).mean():.1f}%")
        heat = np.zeros((h, w, 3), dtype=np.uint8)
        heat[..., 0] = np.clip(diff * 4, 0, 255)
        rows.append(np.concatenate([a.astype(np.uint8), np.full((h, 6, 3), 255, np.uint8), b.astype(np.uint8),
                                    np.full((h, 6, 3), 255, np.uint8), heat], axis=1))
    if args.sheet and rows:
        width = max(r.shape[1] for r in rows)
        rows = [np.pad(r, ((0, 6), (0, width - r.shape[1]), (0, 0)), constant_values=255) for r in rows]
        Image.fromarray(np.concatenate(rows, axis=0)).save(args.sheet)
        print(f"sheet: {args.sheet}")


if __name__ == "__main__":
    main()
