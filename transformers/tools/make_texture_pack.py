"""Make a texture pack (replacements\\<name>.dds) from the port's texture dumps.

Input: a folder of dumps (PS2X_TEXTURE_DUMP=<folder> while playing: <hash>-<clut>-<bits>.png, RGBA
as the GS has it, alpha 128 = opaque). For each dump the tool takes, in this order:

1. a ready upscale of the same texture: a folder pair (--converted, --upscaled) of the game's own
   textures decoded to PNG and their upscales under the same relative paths (the remaster
   project's Extracted\\converted\\textures and Extracted\\upscaled). A dump is matched to a decoded
   texture by its pixels (RGB at the native size; a dump may be a texture's top-left part padded
   to a power of two, which is matched too);
2. an upscale made for it: dumps that matched nothing are copied to <work>\\todo (alpha doubled to
   0-255) for an upscaler (Reforge, realesrgan, ...); its results, put in <work>\\done under the
   same names, are taken on the next run.

Output: <out>\\<name>.dds, uncompressed RGBA8 with mipmaps, alpha back to the GS's 0-128, the long
side capped (--max, 2048). The pack's names come from the dumps, so it only replaces textures the
port has seen; dump more (play more) and run again: finished files are kept.

Usage:
    python make_texture_pack.py --dump <dumps> --out <game>\\replacements --work <work>
        [--converted <decoded textures> --upscaled <their upscales>] [--max 2048] [--scale 4]

Needs Pillow and numpy.
"""

import argparse
import hashlib
import os
import shutil
import struct
import sys

import numpy as np
from PIL import Image


def rgb_key(rgb):
    return hashlib.sha1(np.ascontiguousarray(rgb).tobytes() + bytes(str(rgb.shape), "ascii")).hexdigest()


def index_converted(converted, cache_path):
    """RGB key -> relative path, for every decoded texture (cached: the folder is big)."""
    index = {}
    if os.path.exists(cache_path):
        with open(cache_path, encoding="utf-8") as f:
            for line in f:
                k, rel = line.rstrip("\n").split("\t", 1)
                index[k] = rel
        return index
    count = 0
    for root, _, files in os.walk(converted):
        for name in files:
            if not name.lower().endswith(".png"):
                continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, converted)
            try:
                im = np.asarray(Image.open(path).convert("RGB"))
            except OSError:
                continue
            index.setdefault(rgb_key(im), rel)
            count += 1
    with open(cache_path, "w", encoding="utf-8") as f:
        for k, rel in index.items():
            f.write(f"{k}\t{rel}\n")
    print(f"indexed {count} decoded textures")
    return index


def match_dump(rgba, index):
    """The decoded texture a dump shows: the whole of it, or its top-left part (padding)."""
    rgb = rgba[:, :, :3]
    k = rgb_key(rgb)
    if k in index:
        return index[k], rgb.shape[1], rgb.shape[0]
    # (a texture smaller than its power-of-two area: try the sizes it could have)
    h, w = rgb.shape[:2]
    for th in sorted({h, h // 2, h * 3 // 4} - {0}, reverse=True):
        for tw in sorted({w, w // 2, w * 3 // 4} - {0}, reverse=True):
            if (tw, th) == (w, h):
                continue
            k = rgb_key(rgb[:th, :tw])
            if k in index:
                return index[k], tw, th
    return None, w, h


def write_dds(path, rgba):
    """Uncompressed RGBA8 DDS with a full mip chain (box filtered)."""
    h, w = rgba.shape[:2]
    levels = [rgba]
    while levels[-1].shape[0] > 1 or levels[-1].shape[1] > 1:
        im = Image.fromarray(levels[-1], "RGBA")
        nw, nh = max(1, im.width // 2), max(1, im.height // 2)
        levels.append(np.asarray(im.resize((nw, nh), Image.BOX)))
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x8 | 0x20000  # caps, height, width, pixelformat, pitch, mipmapcount
    header = struct.pack("<4sIIIIIII", b"DDS ", 124, flags, h, w, w * 4, 0, len(levels))
    header += b"\0" * 44
    header += struct.pack("<IIIIIIII", 32, 0x41, 0, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000)
    header += struct.pack("<IIIII", 0x1000 | 0x8 | 0x400000, 0, 0, 0, 0)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(header)
        for level in levels:
            f.write(np.ascontiguousarray(level, dtype=np.uint8).tobytes())
    os.replace(tmp, path)


def finish(src_rgba_0_255, target_w, target_h, max_side):
    """The replacement as the pack wants it: size, alpha back to the GS's 0-128."""
    im = Image.fromarray(src_rgba_0_255, "RGBA")
    scale = min(1.0, max_side / max(target_w, target_h))
    tw, th = max(1, round(target_w * scale)), max(1, round(target_h * scale))
    if im.size != (tw, th):
        im = im.resize((tw, th), Image.LANCZOS)
    a = np.asarray(im).copy()
    a[:, :, 3] = np.round(a[:, :, 3].astype(np.float32) * (128.0 / 255.0)).astype(np.uint8)
    return a


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dump", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--converted")
    ap.add_argument("--upscaled")
    ap.add_argument("--max", type=int, default=2048)
    ap.add_argument("--scale", type=int, default=4, help="the size of a made upscale, times the dump's")
    ap.add_argument("--force", action="store_true", help="make the .dds files again")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    todo = os.path.join(args.work, "todo")
    done = os.path.join(args.work, "done")
    os.makedirs(todo, exist_ok=True)
    os.makedirs(done, exist_ok=True)
    index = {}
    if args.converted and args.upscaled:
        index = index_converted(args.converted, os.path.join(args.work, "converted_index.tsv"))

    made = matched = upscaled = waiting = kept = 0
    for name in sorted(os.listdir(args.dump)):
        if not name.lower().endswith(".png"):
            continue
        stem = name[:-4]
        out = os.path.join(args.out, stem + ".dds")
        if os.path.exists(out) and not args.force:
            kept += 1
            continue
        rgba = np.asarray(Image.open(os.path.join(args.dump, name)).convert("RGBA"))
        h, w = rgba.shape[:2]
        rel = None
        if index:
            rel, tw, th = match_dump(rgba, index)
        if rel and os.path.exists(os.path.join(args.upscaled, rel)):
            up = np.asarray(Image.open(os.path.join(args.upscaled, rel)).convert("RGBA"))
            # (the upscale covers the texture's own part; a padded dump keeps its proportions)
            factor = up.shape[1] / tw
            canvas = Image.new("RGBA", (round(w * factor), round(h * factor)), (0, 0, 0, 0))
            canvas.paste(Image.fromarray(up, "RGBA"), (0, 0))
            write_dds(out, finish(np.asarray(canvas), canvas.width, canvas.height, args.max))
            matched += 1
            made += 1
            continue
        result = None
        for ext in (".png", "_out.png", ".jpg"):
            candidate = os.path.join(done, stem + ext)
            if os.path.exists(candidate):
                result = candidate
                break
        if result:
            up = np.asarray(Image.open(result).convert("RGBA"))
            write_dds(out, finish(up, w * args.scale, h * args.scale, args.max))
            upscaled += 1
            made += 1
            continue
        staged = os.path.join(todo, name)
        if not os.path.exists(staged):
            a = rgba.copy()
            a[:, :, 3] = np.minimum(255, a[:, :, 3].astype(np.uint16) * 2).astype(np.uint8)  # (0-128 -> 0-255)
            Image.fromarray(a, "RGBA").save(staged)
        waiting += 1
    print(f"made {made} ({matched} from ready upscales, {upscaled} from {done}), kept {kept}, "
          f"{waiting} waiting in {todo} for an upscaler")


if __name__ == "__main__":
    sys.exit(main())
