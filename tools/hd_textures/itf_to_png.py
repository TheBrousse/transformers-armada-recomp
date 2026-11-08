#!/usr/bin/env python3
"""Convert Melbourne House ITF textures (PS2) to PNG.

ITF is a little-endian IFF container: `FORM <size> ITF0`, then chunks.
`HDR<ver>` (ver 2/3, 32-byte payload): "PS2" + format byte, then dwords
[unk, width, height, mipcount, palcount(v3), 0, 0]. `TXTR` holds
[palettes][mip0 pixels][mip1...]. Format byte low 7 bits: 0x0a=4bpp CLUT,
0x0b=8bpp CLUT, 0x07=16bpp RGBA5551, 0x02=32bpp RGBA, 0x0c=24bpp RGB.
High bit 0x80 = GS-swizzled pixel data (4/8bpp). PS2 alpha 0x80 = opaque.
Full spec: Tools/docs/ITF-TEXTURE.md

Input : .ITF file(s) or directories (recursed)
Output: <out>/<relative-path>.png (mip 0, palette 0; --all-palettes for more)
Deps  : Python 3.8+, Pillow
"""

import argparse
import pathlib
import struct
import sys
from collections import Counter

from PIL import Image

FMT_4BPP, FMT_8BPP, FMT_16BPP, FMT_32BPP, FMT_24BPP = 0x0A, 0x0B, 0x07, 0x02, 0x0C


def parse_itf(data):
    if data[:4] != b"FORM" or data[8:12] != b"ITF0":
        raise ValueError("not an ITF file")
    hdr = txtr = None
    pos = 12
    while pos + 8 <= len(data):
        tag, csize = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        if tag[:3] == b"HDR":
            hdr = (tag[3], data[pos + 8:pos + 8 + csize])
        elif tag == b"TXTR":
            txtr = data[pos + 8:pos + 8 + csize]
        pos += 8 + csize
    if hdr is None or txtr is None:
        raise ValueError("missing HDR or TXTR chunk")
    ver, payload = hdr
    if payload[:3] != b"PS2":
        raise ValueError(f"unsupported platform {payload[:3]!r}")
    fmt_byte = payload[3]
    _, width, height, mips, pals, _, _ = struct.unpack("<7I", payload[4:32])
    fmt = fmt_byte & 0x7F
    swizzled = bool(fmt_byte & 0x80)
    if ver != 3 or fmt not in (FMT_4BPP, FMT_8BPP) or pals < 1 or pals > 64:
        pals = 1  # v2 palcount field is uninitialized garbage
    return fmt, swizzled, width, height, max(1, mips), pals, txtr


def scale_alpha(a):
    return 255 if a >= 128 else a * 2


_PERM_CACHE = {}


def _perm_8bpp(width, height):
    """Source index for each (y,x) of a GS-swizzled PSMT8 image."""
    key = ("8", width, height)
    if key not in _PERM_CACHE:
        perm = []
        for y in range(height):
            for x in range(width):
                block = (y & ~0xF) * width + (x & ~0xF) * 2
                swap = (((y + 2) >> 2) & 0x1) * 4
                pos_y = (((y & ~3) >> 1) + (y & 1)) & 0x7
                column = pos_y * width * 2 + ((x + swap) & 0x7) * 4
                byte_num = ((y >> 1) & 1) + ((x >> 2) & 2)
                perm.append(block + column + byte_num)
        _PERM_CACHE[key] = perm
    return _PERM_CACHE[key]


# GS local-memory layout tables (PCSX2 GSTables.cpp). Block table is shared
# by PSMCT16 and PSMT4; column tables are full intra-block maps.
_GS_BLOCK = [
    [0, 2, 8, 10], [1, 3, 9, 11], [4, 6, 12, 14], [5, 7, 13, 15],
    [16, 18, 24, 26], [17, 19, 25, 27], [20, 22, 28, 30], [21, 23, 29, 31]]
_GS_COL16 = [
    [0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27],
    [4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31],
    [32, 34, 40, 42, 48, 50, 56, 58, 33, 35, 41, 43, 49, 51, 57, 59],
    [36, 38, 44, 46, 52, 54, 60, 62, 37, 39, 45, 47, 53, 55, 61, 63],
    [64, 66, 72, 74, 80, 82, 88, 90, 65, 67, 73, 75, 81, 83, 89, 91],
    [68, 70, 76, 78, 84, 86, 92, 94, 69, 71, 77, 79, 85, 87, 93, 95],
    [96, 98, 104, 106, 112, 114, 120, 122, 97, 99, 105, 107, 113, 115, 121, 123],
    [100, 102, 108, 110, 116, 118, 124, 126, 101, 103, 109, 111, 117, 119, 125, 127]]
_GS_COL4 = [
    [0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106,
     4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110],
    [16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122,
     20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126],
    [65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43,
     69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47],
    [81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59,
     85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63],
    [192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170,
     196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174],
    [208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186,
     212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190],
    [129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235,
     133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239],
    [145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251,
     149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255],
    [256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362,
     260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366],
    [272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378,
     276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382],
    [321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299,
     325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303],
    [337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315,
     341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319],
    [448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426,
     452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430],
    [464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442,
     468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446],
    [385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491,
     389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495],
    [401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507,
     405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511]]


def _perm_4bpp(width, height):
    """File nibble index for each image pixel of a flag-0x80 4bpp texture.

    The game's loader (SLUS_206.68 @ 0x243080) treats these files as a linear
    PSMCT16 image of (w/2, h/2) uploaded to GS memory, then sampled as PSMT4
    at (w, h) — the GS page layouts do the swizzle. We compose both mappings.
    """
    key = ("4", width, height)
    if key not in _PERM_CACHE:
        w2, h2 = width // 2, height // 2
        ppr16 = max(1, max(64, w2) // 64)
        inv = {}                       # GS 16-bit unit address -> file unit index
        i = 0
        for y in range(h2):
            for x in range(w2):
                u = ((y // 64) * ppr16 + (x // 64)) * 4096 \
                    + _GS_BLOCK[(y % 64) // 8][(x % 64) // 16] * 128 \
                    + _GS_COL16[y % 8][x % 16]
                inv[u] = i
                i += 1
        ppr4 = max(1, width // 128)
        perm = []
        for y in range(height):
            for x in range(width):
                n = ((y // 128) * ppr4 + (x // 128)) * 16384 \
                    + _GS_BLOCK[(y % 128) // 16][(x % 128) // 32] * 512 \
                    + _GS_COL4[y % 16][x % 32]
                f = inv.get(n >> 2)
                perm.append(0 if f is None else f * 4 + (n & 3))
        _PERM_CACHE[key] = perm
    return _PERM_CACHE[key]


def unswizzle_pixels(fmt, pix, width, height):
    """Linearize GS-swizzled index data (formats with the 0x80 flag)."""
    if fmt == FMT_8BPP:
        return bytes(pix[i] for i in _perm_8bpp(width, height))
    out = bytearray((width * height + 1) // 2)
    for i, src in enumerate(_perm_4bpp(width, height)):
        nib = (pix[src >> 1] >> ((src & 1) * 4)) & 0xF
        if i % 2 == 0:
            out[i >> 1] = nib
        else:
            out[i >> 1] |= nib << 4
    return bytes(out)


def unswizzle_csm1(pal):
    """Undo GS CSM1 palette layout: within each 32-color block, swap rows 1<->2."""
    out = bytearray(pal)
    for base in range(0, len(pal), 128):
        out[base + 32:base + 64], out[base + 64:base + 96] = \
            pal[base + 64:base + 96], pal[base + 32:base + 64]
    return bytes(out)


def clut_to_rgba(pal):
    return [(pal[i], pal[i + 1], pal[i + 2], scale_alpha(pal[i + 3]))
            for i in range(0, len(pal), 4)]


def decode(fmt, width, height, pals, txtr, pal_index=0, swizzled=False):
    n = width * height
    if fmt == FMT_4BPP:
        pal = clut_to_rgba(txtr[pal_index * 64:pal_index * 64 + 64])
        pix = txtr[pals * 64:]
        if swizzled:
            pix = unswizzle_pixels(FMT_4BPP, pix, width, height)
        out = bytearray(n * 4)
        for i in range(n):
            byte = pix[i >> 1]
            idx = byte & 0x0F if i % 2 == 0 else byte >> 4
            out[i * 4:i * 4 + 4] = bytes(pal[idx])
        return Image.frombytes("RGBA", (width, height), bytes(out))
    if fmt == FMT_8BPP:
        raw = unswizzle_csm1(txtr[pal_index * 1024:pal_index * 1024 + 1024])
        pal = clut_to_rgba(raw)
        pix = txtr[pals * 1024:]
        if swizzled:
            pix = unswizzle_pixels(FMT_8BPP, pix, width, height)
        out = bytearray(n * 4)
        for i in range(n):
            out[i * 4:i * 4 + 4] = bytes(pal[pix[i]])
        return Image.frombytes("RGBA", (width, height), bytes(out))
    if fmt == FMT_16BPP:
        vals = struct.unpack(f"<{n}H", txtr[:n * 2])
        any_alpha = any(v & 0x8000 for v in vals)
        out = bytearray(n * 4)
        for i, v in enumerate(vals):
            out[i * 4:i * 4 + 4] = bytes((
                (v & 31) << 3, (v >> 5 & 31) << 3, (v >> 10 & 31) << 3,
                255 if (v & 0x8000 or not any_alpha) else 0))
        return Image.frombytes("RGBA", (width, height), bytes(out))
    if fmt == FMT_24BPP:
        return Image.frombytes("RGB", (width, height), txtr[:n * 3]).convert("RGBA")
    if fmt == FMT_32BPP:
        out = bytearray(txtr[:n * 4])
        for i in range(3, n * 4, 4):
            out[i] = scale_alpha(out[i])
        return Image.frombytes("RGBA", (width, height), bytes(out))
    raise ValueError(f"unknown pixel format {fmt:#x}")


def convert_file(src, dest, all_palettes=False):
    fmt, swizzled, width, height, mips, pals, txtr = parse_itf(src.read_bytes())
    dest.parent.mkdir(parents=True, exist_ok=True)
    decode(fmt, width, height, pals, txtr, 0, swizzled).save(dest)
    if all_palettes and pals > 1:
        for p in range(1, pals):
            decode(fmt, width, height, pals, txtr, p, swizzled).save(
                dest.with_name(f"{dest.stem}_pal{p}.png"))
    return fmt, swizzled, pals


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("inputs", nargs="+", type=pathlib.Path)
    ap.add_argument("--out", type=pathlib.Path,
                    default=pathlib.Path("Extracted/converted/textures"))
    ap.add_argument("--all-palettes", action="store_true",
                    help="also export palettes 1..N as _palN.png")
    ap.add_argument("--skip-swizzled-4bpp", action="store_true",
                    help="skip 0x8A textures (pixel order not yet solved)")
    args = ap.parse_args()

    todo = []
    for inp in args.inputs:
        if inp.is_dir():
            todo += [(f, inp.parent) for f in sorted(inp.rglob("*.ITF"))]
        else:
            todo.append((inp, inp.parent))

    stats, failures = Counter(), []
    for src, base in todo:
        try:
            if args.skip_swizzled_4bpp:
                head = src.read_bytes()[:0x18]
                if len(head) >= 0x18 and head[0x17] == 0x8A:
                    stats["skipped-0x8a"] += 1
                    continue
            fmt, swizzled, _ = convert_file(
                src, (args.out / src.relative_to(base)).with_suffix(".png"),
                args.all_palettes)
            stats[f"fmt{fmt:#04x}{'+swz' if swizzled else ''}"] += 1
        except (ValueError, OSError, struct.error) as exc:
            failures.append((src, exc))
            print(f"  ERROR {src}: {exc}", file=sys.stderr)

    print(f"converted {sum(stats.values())}/{len(todo)}  {dict(stats)}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
