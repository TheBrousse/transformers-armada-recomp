"""Renders part of a GS local memory dump (runs/vram.bin, from PS2X_GS_DUMP_VRAM_TICK or
PS2X_GS_DUMP_VRAM_AT) as an image (debugging aid).

    python tools/vram_view.py runs/vram.bin --fbp 0 --fbw 8 --psm ct24 -o runs/page0.png
    python tools/vram_view.py runs/vram.bin --fbp 224 --fbw 8 --psm z24 -o runs/z.png

--fbp is in pages (2048 words), --fbw in units of 64 pixels, as in the FRAME register.
Formats: ct32, ct24, ct16, z32, z24, z16 (depth shows as grey levels).
"""
import argparse

import numpy as np
from PIL import Image

BLOCK32 = [[0, 1, 4, 5, 16, 17, 20, 21],
           [2, 3, 6, 7, 18, 19, 22, 23],
           [8, 9, 12, 13, 24, 25, 28, 29],
           [10, 11, 14, 15, 26, 27, 30, 31]]
BLOCK32Z = [[24, 25, 28, 29, 8, 9, 12, 13],
            [26, 27, 30, 31, 10, 11, 14, 15],
            [16, 17, 20, 21, 0, 1, 4, 5],
            [18, 19, 22, 23, 2, 3, 6, 7]]
BLOCK16 = [[0, 2, 8, 10], [1, 3, 9, 11], [4, 6, 12, 14], [5, 7, 13, 15],
           [16, 18, 24, 26], [17, 19, 25, 27], [20, 22, 28, 30], [21, 23, 29, 31]]
BLOCK16Z = [[24, 26, 16, 18], [25, 27, 17, 19], [28, 30, 20, 22], [29, 31, 21, 23],
            [8, 10, 0, 2], [9, 11, 1, 3], [12, 14, 4, 6], [13, 15, 5, 7]]
COLUMN32 = [[0, 1, 4, 5, 8, 9, 12, 13],
            [2, 3, 6, 7, 10, 11, 14, 15],
            [16, 17, 20, 21, 24, 25, 28, 29],
            [18, 19, 22, 23, 26, 27, 30, 31],
            [32, 33, 36, 37, 40, 41, 44, 45],
            [34, 35, 38, 39, 42, 43, 46, 47],
            [48, 49, 52, 53, 56, 57, 60, 61],
            [50, 51, 54, 55, 58, 59, 62, 63]]
COLUMN16 = [[0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27],
            [4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31],
            [32, 34, 40, 42, 48, 50, 56, 58, 33, 35, 41, 43, 49, 51, 57, 59],
            [36, 38, 44, 46, 52, 54, 60, 62, 37, 39, 45, 47, 53, 55, 61, 63],
            [64, 66, 72, 74, 80, 82, 88, 90, 65, 67, 73, 75, 81, 83, 89, 91],
            [68, 70, 76, 78, 84, 86, 92, 94, 69, 71, 77, 79, 85, 87, 93, 95],
            [96, 98, 104, 106, 112, 114, 120, 122, 97, 99, 105, 107, 113, 115, 121, 123],
            [100, 102, 108, 110, 116, 118, 124, 126, 101, 103, 109, 111, 117, 119, 125, 127]]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("vram")
    parser.add_argument("--fbp", type=int, required=True)
    parser.add_argument("--fbw", type=int, default=8)
    parser.add_argument("--psm", default="ct32", choices=["ct32", "ct24", "ct16", "z32", "z24", "z16"])
    parser.add_argument("--width", type=int)
    parser.add_argument("--height", type=int, default=448)
    parser.add_argument("-o", "--output", required=True)
    args = parser.parse_args()

    data = np.fromfile(args.vram, dtype=np.uint8)
    width = args.width or args.fbw * 64
    height = args.height
    depth = args.psm.startswith("z")
    sixteen = args.psm.endswith("16")
    base_block = args.fbp * 32
    ys, xs = np.mgrid[0:height, 0:width]
    if sixteen:
        page = (ys // 64) * args.fbw + xs // 64
        table = np.array(BLOCK16Z if depth else BLOCK16)
        block = table[(ys % 64) // 8, (xs % 64) // 16]
        entry = np.array(COLUMN16)[ys % 8, xs % 16]
        address = ((base_block + page * 32 + block) * 256 + entry * 2) % data.size
        values = data[address].astype(np.uint32) | (data[(address + 1) % data.size].astype(np.uint32) << 8)
    else:
        page = (ys // 32) * args.fbw + xs // 64
        table = np.array(BLOCK32Z if depth else BLOCK32)
        block = table[(ys % 32) // 8, (xs % 64) // 8]
        entry = np.array(COLUMN32)[ys % 8, xs % 8]
        address = ((base_block + page * 32 + block) * 256 + entry * 4) % data.size
        values = np.zeros((height, width), dtype=np.uint32)
        for byte in range(4):
            values |= data[(address + byte) % data.size].astype(np.uint32) << (8 * byte)

    if depth:
        if args.psm == "z24":
            values &= 0xFFFFFF
        top = max(int(values.max()), 1)
        grey = (values.astype(np.float64) / top * 255.0).astype(np.uint8)
        image = np.dstack([grey, grey, grey])
        print(f"depth range {int(values.min())}..{top}, {int((values != 0).sum())} non-zero of {values.size}")
    elif sixteen:
        r = (values & 31) << 3
        g = ((values >> 5) & 31) << 3
        b = ((values >> 10) & 31) << 3
        image = np.dstack([r, g, b]).astype(np.uint8)
    else:
        image = np.dstack([values & 255, (values >> 8) & 255, (values >> 16) & 255]).astype(np.uint8)
        alpha = values >> 24
        print(f"non-black pixels {int(((values & 0xFFFFFF) != 0).sum())} of {values.size}, "
              f"alpha range {int(alpha.min())}..{int(alpha.max())}")
    Image.fromarray(image, "RGB").save(args.output)
    print(f"{width}x{height} {args.psm} at page {args.fbp} -> {args.output}")


if __name__ == "__main__":
    main()
