r"""Measures how far apart two sets of frames are: the native pixel path's pictures
(PS2X_GPU_NATIVE=1) against the emulated GPU renderer's, which they are not meant to match bit
for bit (sub-pixel coverage, blend rounding, depth precision). Frames are matched by name, case
by case: a folder per case, as tools\check_frames.ps1 -Update -GoldenRoot keeps them
(tools\check_native.ps1 makes both sets and calls this). The first folder is the reference.

    python tools\native_compare.py runs\native_ref runs\native_new [--case level,room]
    python tools\native_compare.py runs\native_ref runs\native_new --heatmaps runs\native_heat
    python tools\native_compare.py runs\native_ref runs\native_new --max-mean 1 --max-flat16 0.05

Each frame, and each case over all its frames together:
  mean R G B     the absolute difference, over the three channels and per channel
  p99 max        a pixel's largest channel difference: 99% of the pixels are within p99
  >8% >16% >32%  the pixels whose largest channel difference is more than 8, 16, 32
  PSNR           over the three channels, in dB (inf: the same picture)
  SSIM           structural similarity of the luminance (Gaussian window of 11 pixels, sigma
                 1.5, as Wang et al. and skimage's gaussian_weights); a case: its frames' average
  edges%         the pixels on an edge or in fine detail in both pictures: where the luminance
                 steps by more than --edge levels across the pixel (Sobel; 16), and --grow pixels
                 around (1); --reference-edges: in the reference alone. Sub-pixel coverage,
                 multisampling and texture sampling differences land there; one on a flat area
                 (or where detail is gone) is of colour, blending, depth or a draw missing
  edge flat      the mean difference on edges and on flat areas, and the share of their pixels
  edge>16 flat>16  off by more than 16

Frames of different sizes are reported and left out (--fit averages the bigger picture's
blocks down to the other's size when it is a whole multiple of it: scale 3 against scale 1).
With limits, each case is PASS or FAIL - every frame within all of them, none missing or of
another size - and the exit code is 1 when one fails (2: nothing compared).
"""
import argparse
import math
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

# The figures a limit can be set on: option, figure, heading, whether the limit is a floor.
LIMITS = [
    ("max-mean", "mean", "mean", False),
    ("max-p99", "p99", "p99", False),
    ("max-share8", "share8", ">8%", False),
    ("max-share16", "share16", ">16%", False),
    ("max-share32", "share32", ">32%", False),
    ("max-flat16", "flat16", "flat>16", False),
    ("min-psnr", "psnr", "PSNR", True),
    ("min-ssim", "ssim", "SSIM", True),
]

# The table: heading, figure, width, format.
COLUMNS = [
    ("mean", "mean", 5, "{:.2f}"),
    ("R", "r", 5, "{:.2f}"),
    ("G", "g", 5, "{:.2f}"),
    ("B", "b", 5, "{:.2f}"),
    ("p99", "p99", 3, "{:d}"),
    ("max", "max", 3, "{:d}"),
    (">8%", "share8", 6, "{:.2f}"),
    (">16%", "share16", 6, "{:.2f}"),
    (">32%", "share32", 6, "{:.2f}"),
    ("PSNR", "psnr", 5, "{:.1f}"),
    ("SSIM", "ssim", 6, "{:.4f}"),
    ("edges%", "edges", 6, "{:.1f}"),
    ("edge", "edge_mean", 5, "{:.2f}"),
    ("edge>16", "edge16", 7, "{:.2f}"),
    ("flat", "flat_mean", 5, "{:.2f}"),
    ("flat>16", "flat16", 7, "{:.2f}"),
]

# SSIM's window: 11 taps of a Gaussian of sigma 1.5.
WINDOW = np.exp(-0.5 * (np.arange(-5, 6) / 1.5) ** 2)
WINDOW /= WINDOW.sum()


class Tally:
    """Sums over one frame or several, which the figures come from."""

    def __init__(self):
        self.pixels = 0
        self.channels = np.zeros(3, np.int64)       # absolute differences, per channel
        self.squares = 0                            # squared differences, all channels
        self.worst = np.zeros(256, np.int64)        # pixels by their largest channel difference
        self.edge_pixels = 0
        self.edge_sum = 0                           # absolute differences on edges, all channels
        self.edge_worst = np.zeros(256, np.int64)
        self.ssim = []

    def add(self, other):
        self.pixels += other.pixels
        self.channels += other.channels
        self.squares += other.squares
        self.worst += other.worst
        self.edge_pixels += other.edge_pixels
        self.edge_sum += other.edge_sum
        self.edge_worst += other.edge_worst
        self.ssim += other.ssim

    def figures(self):
        n = self.pixels
        total = int(self.channels.sum())
        flat_pixels = n - self.edge_pixels
        flat_worst = self.worst - self.edge_worst
        mse = self.squares / (3 * n)

        def share(counts, of):
            return 100.0 * counts / of if of else math.nan

        return {
            "mean": total / (3 * n),
            "r": self.channels[0] / n, "g": self.channels[1] / n, "b": self.channels[2] / n,
            # (the smallest difference at least 99% of the pixels are within)
            "p99": int(np.searchsorted(np.cumsum(self.worst), 0.99 * n)),
            "max": int(np.flatnonzero(self.worst)[-1]),
            "share8": share(self.worst[9:].sum(), n),
            "share16": share(self.worst[17:].sum(), n),
            "share32": share(self.worst[33:].sum(), n),
            "psnr": math.inf if mse == 0 else 10.0 * math.log10(255.0 ** 2 / mse),
            "ssim": float(np.mean(self.ssim)) if self.ssim else math.nan,
            "edges": share(self.edge_pixels, n),
            "edge_mean": self.edge_sum / (3 * self.edge_pixels) if self.edge_pixels else math.nan,
            "edge16": share(self.edge_worst[17:].sum(), self.edge_pixels),
            "flat_mean": (total - self.edge_sum) / (3 * flat_pixels) if flat_pixels else math.nan,
            "flat16": share(flat_worst[17:].sum(), flat_pixels),
        }


def natural(text):
    """Sort key: numbers by value (shot_5740 before shot_25860)."""
    return [int(part) if part.isdigit() else part for part in re.split(r"(\d+)", text)]


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def luminance(picture):
    return picture[..., 0] * 0.299 + picture[..., 1] * 0.587 + picture[..., 2] * 0.114


def fit(a, b):
    """The two pictures at one size, the bigger one's blocks averaged, when it is a whole
    multiple of the other across and down (as PS2X_GPU_SCALE draws); otherwise None."""
    def down(big, small):
        (h, w), (bh, bw) = small.shape[:2], big.shape[:2]
        if bh % h or bw % w:
            return None
        return big.reshape(h, bh // h, w, bw // w, 3).mean(axis=(1, 3)).round().astype(np.int16)

    if a.shape[0] >= b.shape[0] and a.shape[1] >= b.shape[1]:
        a = down(a, b)
    elif a.shape[0] <= b.shape[0] and a.shape[1] <= b.shape[1]:
        b = down(b, a)
    else:
        return None
    return None if a is None or b is None else (a, b)


def edge_mask(lum, step, grow):
    """The pixels where the luminance steps by more than `step` across the pixel (Sobel: its
    magnitude over 4 is the height of a straight step), and `grow` pixels around them."""
    p = np.pad(lum, 1, mode="edge")
    gx = (p[:-2, 2:] + 2 * p[1:-1, 2:] + p[2:, 2:]) - (p[:-2, :-2] + 2 * p[1:-1, :-2] + p[2:, :-2])
    gy = (p[2:, :-2] + 2 * p[2:, 1:-1] + p[2:, 2:]) - (p[:-2, :-2] + 2 * p[:-2, 1:-1] + p[:-2, 2:])
    mask = np.hypot(gx, gy) > 4.0 * step
    for _ in range(grow):
        m = np.pad(mask, 1)
        mask = (m[:-2, :-2] | m[:-2, 1:-1] | m[:-2, 2:] | m[1:-1, :-2] | m[1:-1, 1:-1] | m[1:-1, 2:]
                | m[2:, :-2] | m[2:, 1:-1] | m[2:, 2:])
    return mask


def blur(x):
    """The SSIM window over the picture, where it fits whole (5 pixels lost at each side)."""
    k = len(WINDOW)
    h, w = x.shape
    rows = sum(WINDOW[i] * x[:, i:w - k + 1 + i] for i in range(k))
    return sum(WINDOW[i] * rows[i:h - k + 1 + i, :] for i in range(k))


def ssim(a, b):
    """Mean SSIM of two luminance pictures (K1 0.01, K2 0.03, range 255)."""
    if min(a.shape) < len(WINDOW):
        return math.nan
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    mean_a, mean_b = blur(a), blur(b)
    var_a = blur(a * a) - mean_a * mean_a
    var_b = blur(b * b) - mean_b * mean_b
    cov = blur(a * b) - mean_a * mean_b
    s = ((2 * mean_a * mean_b + c1) * (2 * cov + c2)) / ((mean_a * mean_a + mean_b * mean_b + c1) * (var_a + var_b + c2))
    return float(s.mean())


def compare(reference, candidate, step, grow, reference_edges=False):
    """The tally of one pair of pictures, with what a heat map needs. A pixel is on an edge
    where both pictures have one (an edge moved by a pixel or two, or smoothed, stays one; detail
    that is gone - a draw missing - is not), or the reference alone with reference_edges."""
    delta = np.abs(reference - candidate)
    worst = delta.max(axis=2)
    lum, candidate_lum = luminance(reference), luminance(candidate)
    edges = edge_mask(lum, step, grow)
    if not reference_edges:
        edges &= edge_mask(candidate_lum, step, grow)
    tally = Tally()
    tally.pixels = worst.size
    tally.channels = delta.sum(axis=(0, 1), dtype=np.int64)
    tally.squares = int((delta.astype(np.int32) ** 2).sum(dtype=np.int64))
    tally.worst = np.bincount(worst.ravel(), minlength=256)
    tally.edge_pixels = int(edges.sum())
    tally.edge_sum = int(delta[edges].sum(dtype=np.int64))
    tally.edge_worst = np.bincount(worst[edges], minlength=256)
    tally.ssim = [ssim(lum, candidate_lum)]
    return tally, worst, edges, lum


def worst_block(worst, size):
    """Top-left corner and size of the block (at steps of half a block) that differs most."""
    h, w = worst.shape
    size = max(1, min(size, h, w))
    total = np.zeros((h + 1, w + 1), np.int64)
    total[1:, 1:] = worst.cumsum(axis=0, dtype=np.int64).cumsum(axis=1)
    step = max(1, size // 2)
    ys = np.unique(np.append(np.arange(0, h - size + 1, step), h - size))
    xs = np.unique(np.append(np.arange(0, w - size + 1, step), w - size))
    y, x = np.meshgrid(ys, xs, indexing="ij")
    sums = total[y + size, x + size] - total[y, x + size] - total[y + size, x] + total[y, x]
    i, j = np.unravel_index(np.argmax(sums), sums.shape)
    return int(ys[i]), int(xs[j]), size


def write_heat_map(folder, stem, reference, candidate, worst, edges, lum, gain, block):
    """<stem>_diff.png: the differences times `gain`, red on flat areas and blue on edges, over
    the reference in dim grey, with the block that differs most in a yellow box;
    <stem>_worst.png: that block from the reference, the candidate and the map, 4x."""
    folder.mkdir(parents=True, exist_ok=True)
    amp = np.clip(worst.astype(np.int32) * gain, 0, 255)
    base = (lum * 0.3).astype(np.int32)
    heat = np.stack([np.maximum(base, np.where(edges, 0, amp)),
                     np.maximum(base, np.where(edges, amp * 3 // 5, 0)),
                     np.maximum(base, np.where(edges, amp, 0))], axis=2).astype(np.uint8)
    y, x, size = worst_block(worst, block)
    zoom = 4
    crops = [reference[y:y + size, x:x + size].astype(np.uint8), candidate[y:y + size, x:x + size].astype(np.uint8),
             heat[y:y + size, x:x + size]]
    gap = np.full((size * zoom, 6, 3), 255, np.uint8)
    parts = []
    for crop in crops:
        parts += [crop.repeat(zoom, axis=0).repeat(zoom, axis=1), gap]
    Image.fromarray(np.concatenate(parts[:-1], axis=1)).save(folder / f"{stem}_worst.png")
    picture = Image.fromarray(heat)
    ImageDraw.Draw(picture).rectangle([x - 1, y - 1, x + size, y + size], outline=(255, 255, 0))
    picture.save(folder / f"{stem}_diff.png")


def frames(root, skip):
    """The PNG files under root, at any depth, by their path relative to it."""
    found = {}
    for path in root.rglob("*.png"):
        if skip is not None and skip in path.resolve().parents:
            continue
        found[path.relative_to(root)] = path
    return found


def row(label, width, figures):
    cells = []
    for _, figure, size, form in COLUMNS:
        value = figures[figure]
        cells.append(("-" if isinstance(value, float) and math.isnan(value) else form.format(value)).rjust(size))
    return f"  {label:<{width}} " + " ".join(cells)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("reference", help="folder of the reference frames (a folder per case), or one picture")
    parser.add_argument("candidate", help="folder of the frames to measure against them, or one picture")
    parser.add_argument("--case", action="append", default=[],
                        help="cases (folders) to compare, in this order, comma-separated (default: all, by name)")
    parser.add_argument("--edge", type=float, default=16.0,
                        help="luminance step across a pixel that makes it an edge (default 16)")
    parser.add_argument("--grow", type=int, default=1, help="edges grown by this many pixels (default 1)")
    parser.add_argument("--reference-edges", action="store_true",
                        help="edges where the reference has one (default: where both pictures have one)")
    parser.add_argument("--fit", action="store_true",
                        help="average the bigger picture's blocks down when it is a whole multiple of the other")
    parser.add_argument("--heatmaps", metavar="DIR",
                        help="write <frame>_diff.png and <frame>_worst.png here for each frame that differs")
    parser.add_argument("--gain", type=int, default=8, help="heat maps: differences times this (default 8)")
    parser.add_argument("--block", type=int, default=64, help="heat maps: size of the worst block (default 64)")
    for option, _, heading, floor in LIMITS:
        parser.add_argument("--" + option, type=float, metavar="X",
                            help=f"each frame's {heading.replace('%', '%%')} at {'least' if floor else 'most'} X")
    args = parser.parse_args()

    reference_root, candidate_root = Path(args.reference), Path(args.candidate)
    heat_root = Path(args.heatmaps) if args.heatmaps else None
    limits = [(heading, figure, floor, getattr(args, option.replace("-", "_")))
              for option, figure, heading, floor in LIMITS if getattr(args, option.replace("-", "_")) is not None]
    if reference_root.is_file() and candidate_root.is_file():
        reference_frames = {Path(reference_root.name): reference_root}
        candidate_frames = {Path(reference_root.name): candidate_root}
    elif reference_root.is_dir() and candidate_root.is_dir():
        skip = heat_root.resolve() if heat_root else None
        reference_frames, candidate_frames = frames(reference_root, skip), frames(candidate_root, skip)
    else:
        print(f"need two folders (or two pictures): {reference_root}, {candidate_root}")
        return 2

    cases = {}
    for rel in sorted(set(reference_frames) | set(candidate_frames), key=lambda p: natural(p.name)):
        cases.setdefault(str(rel.parent), []).append(rel)
    order = [c.strip() for c in ",".join(args.case).split(",") if c.strip()] or sorted(cases, key=natural)
    width = max([len(rel.name) for case in order for rel in cases.get(case, [])] + [14])

    print(f"reference {reference_root}, candidate {candidate_root}; edges: luminance steps over {args.edge:g} in "
          f"{'the reference' if args.reference_edges else 'both'}, grown by {args.grow}")
    if limits:
        print("limits per frame: " + ", ".join(f"{heading} {'at least' if floor else 'at most'} {value:g}"
                                               for heading, _, floor, value in limits))
    print(f"  {'frame':<{width}} " + " ".join(heading.rjust(size) for heading, _, size, _ in COLUMNS))
    overall = Tally()
    compared = 0
    failed = []
    for case in order:
        print(case)
        tally = Tally()
        problems = []
        if not cases.get(case):
            print("  no frames")
            problems.append("no frames")
        for rel in cases.get(case, []):
            name = rel.name
            reference_path, candidate_path = reference_frames.get(rel), candidate_frames.get(rel)
            if reference_path is None or candidate_path is None:
                note = "only in the candidate" if reference_path is None else "missing in the candidate"
                print(f"  {name:<{width}} {note}")
                problems.append(f"{name} {note}")
                continue
            reference, candidate = load(reference_path), load(candidate_path)
            if reference.shape != candidate.shape:
                pair = fit(reference, candidate) if args.fit else None
                if pair is None:
                    (rh, rw), (ch, cw) = reference.shape[:2], candidate.shape[:2]
                    print(f"  {name:<{width}} sizes differ: {rw}x{rh} (reference) / {cw}x{ch} (candidate), not compared")
                    problems.append(f"{name} of another size")
                    continue
                reference, candidate = pair
            frame, worst, edges, lum = compare(reference, candidate, args.edge, args.grow, args.reference_edges)
            figures = frame.figures()
            print(row(name, width, figures))
            tally.add(frame)
            compared += 1
            for heading, figure, floor, value in limits:
                measured = figures[figure]
                if (measured < value) if floor else (measured > value):
                    form = next(f for h, _, _, f in COLUMNS if h == heading)
                    problems.append(f"{name} {heading} {form.format(measured)} ({'under' if floor else 'over'} {value:g})")
            if heat_root is not None and figures["max"]:
                write_heat_map(heat_root / rel.parent, Path(name).stem, reference, candidate, worst, edges, lum,
                               args.gain, args.block)
        if tally.pixels:
            frames_in_case = len(tally.ssim)
            print(row(f"{frames_in_case} frame{'s' if frames_in_case != 1 else ''}", width, tally.figures()))
            overall.add(tally)
        if limits:
            if problems:
                failed.append(case)
                print(f"  {case}: FAIL - " + "; ".join(problems))
            else:
                print(f"  {case}: PASS")
    if len(order) > 1 and overall.pixels:
        print(row(f"all {len(order)} cases", width, overall.figures()))
    if heat_root is not None and compared:
        print(f"heat maps: {heat_root}")
    if not compared:
        print("nothing compared")
        return 2
    if limits:
        cases_text = f"{len(order)} case{'s' if len(order) != 1 else ''}"
        print(f"FAILED: {len(failed)} of {cases_text} ({', '.join(failed)})" if failed
              else f"{'all ' if len(order) > 1 else ''}{cases_text} within the limits")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
