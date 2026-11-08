"""Estimates how far the picture moved sideways between two frames (debugging aid for camera
turning: scripted stick or mouse input, then compare a frame before and after).

    python tools/frame_shift.py runs/before.png runs/after.png [--rows 30,210] [--max 300]

Both frames are blurred (foliage and interlacing would otherwise drown the match), then every
shift is scored by the correlation of the overlapping parts of the chosen rows. Prints the
best shift in pixels (negative: the scene moved left, so the view turned right), its
correlation (1 = identical) and the correlation at no shift.
"""
import argparse

import numpy as np
from PIL import Image, ImageFilter


def load(path, blur):
    image = Image.open(path).convert("L").filter(ImageFilter.BoxBlur(blur))
    return np.asarray(image).astype(np.float32)


def correlation(a, b):
    a = a - a.mean()
    b = b - b.mean()
    scale = float(np.sqrt((a * a).sum() * (b * b).sum()))
    return float((a * b).sum() / scale) if scale > 0.0 else 0.0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("first")
    parser.add_argument("second")
    parser.add_argument("--rows", default="30,210", help="first,last row to compare")
    parser.add_argument("--max", type=int, default=300, help="largest shift to try")
    parser.add_argument("--blur", type=int, default=4, help="box blur radius")
    args = parser.parse_args()

    a = load(args.first, args.blur)
    b = load(args.second, args.blur)
    y0, y1 = (int(v) for v in args.rows.split(","))
    a = a[y0:y1]
    b = b[y0:y1]
    width = a.shape[1]
    scores = {}
    for shift in range(-args.max, args.max + 1):
        overlap = width - abs(shift)
        if overlap < width // 4:
            continue
        # A point at column x of the first frame is at x + shift in the second.
        first = a[:, max(0, -shift):max(0, -shift) + overlap]
        second = b[:, max(0, shift):max(0, shift) + overlap]
        scores[shift] = correlation(first, second)
    best = max(scores, key=scores.get)
    print(f"shift {best:+d} px, correlation {scores[best]:.3f} (no shift: {scores[0]:.3f})")


if __name__ == "__main__":
    main()
