#!/usr/bin/env python3
"""Makes the game exe's icon from the player's own disc (TODO 198).

Purpose: the exe gets the head of Optimus Prime from the main menu's backdrop (TITLE_OPT2), steel
         blue, in the sizes Windows uses. The picture comes from YOUR disc at build time: no image of
         the game is ever in the repository. Large sizes show the whole head; 16-32 px lift the head
         off a darkened surround with more contrast, so it still reads in the taskbar and Explorer's small views.
Input:   --game <folder>: the disc's files as build.ps1 extracted them (TFA.ZIP in it).
Output:  --out <file.ico>: a multi-size icon (256, 128, 64, 48, 32, 24, 16; PNG-compressed entries,
         as Windows Vista and later read them). Only written when it changes.
         --preview <file.png>: also a sheet of every size, for a look.
Exit:    0 made (or already the same); 2 no disc data yet (no TFA.ZIP, or the texture is not in it:
         the build then goes on without an icon); 1 any other failure.
Deps:    Python 3.8+, Pillow; tools/hd_textures/extract_pk_archive.py and itf_to_png.py (same repo).
Called:  transformers/icon.cmake, at the game build's configure step.
"""
from __future__ import annotations

import argparse
import io
import pathlib
import struct
import sys
import zlib

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "hd_textures"))

ARCHIVE = "TFA.ZIP"
ENTRY = "USERINTERFACE/TEXTURES/BACKGROUND/TITLE_OPT2.ITF"
SIZES = (256, 128, 64, 48, 32, 24, 16)
# The 512 x 512 backdrop's square crops: the head with its antennae for the large sizes, the helmet's
# crest and face plate for the small ones (where the antennae are a pixel wide and only add noise).
CROP_LARGE = (112, 30, 400, 318)
CROP_SMALL = (140, 48, 380, 288)
HEAD_CENTER, HEAD_RADII = (258, 182), (108, 135)   # the head's oval in the backdrop (small sizes)
AROUND_HEAD = 0.28              # how bright the rest stays there
SMALL = 32                      # this size and below use the small crop
TINT = (0.80, 0.90, 1.05)       # grey to steel blue


def read_texture(game: pathlib.Path) -> bytes | None:
    import extract_pk_archive as pk
    archive = game / ARCHIVE
    if not archive.is_file():
        return None
    with open(archive, "rb") as fh:
        for name, method, crc, csize, _usize, lofs in pk.read_central_directory(fh):
            if name.replace("\\", "/").upper() == ENTRY:
                data = pk.read_entry_data(fh, method, csize, lofs)
                if zlib.crc32(data) & 0xFFFFFFFF != crc:
                    raise ValueError(f"{ARCHIVE}: {ENTRY} fails its CRC check")
                return data
    return None


def decode(itf: bytes):
    import itf_to_png
    fmt, swizzled, width, height, _mips, pals, txtr = itf_to_png.parse_itf(itf)
    return itf_to_png.decode(fmt, width, height, pals, txtr, 0, swizzled)


def steel(grey):
    from PIL import Image
    return Image.merge("RGB", [grey.point(lambda v, k=k: min(255, int(v * k + 0.5))) for k in TINT])


def rounded(img, radius: float):
    """The icon's corners rounded (alpha), drawn 4x larger for a smooth edge."""
    from PIL import Image, ImageDraw
    s = img.width
    if radius <= 0:
        return img.convert("RGBA")
    mask = Image.new("L", (s * 4, s * 4), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, s * 4 - 1, s * 4 - 1), int(s * 4 * radius), fill=255)
    out = img.convert("RGBA")
    out.putalpha(mask.resize((s, s), Image.LANCZOS))
    return out


def frame(backdrop, size: int):
    from PIL import Image, ImageFilter, ImageOps
    grey = backdrop.convert("L")
    if size > SMALL:
        part = grey.crop(CROP_LARGE)
        part = part.resize((size, size), Image.LANCZOS)
        return rounded(steel(part), 0.16)
    # Small: tighter, the busy truck around the head darkened (a soft oval on the head), the contrast
    # stretched, a light sharpen after the resize so the crest and face plate keep their edges.
    from PIL import ImageDraw
    part = grey.crop(CROP_SMALL)
    w = part.width
    oval = Image.new("L", (w, w), 0)
    cx, cy = HEAD_CENTER[0] - CROP_SMALL[0], HEAD_CENTER[1] - CROP_SMALL[1]
    ImageDraw.Draw(oval).ellipse((cx - HEAD_RADII[0], cy - HEAD_RADII[1], cx + HEAD_RADII[0], cy + HEAD_RADII[1]), fill=255)
    oval = oval.filter(ImageFilter.GaussianBlur(w * 0.06))
    part = Image.composite(part, part.point(lambda v: int(v * AROUND_HEAD)), oval)
    part = ImageOps.autocontrast(part, cutoff=1)
    part = part.resize((size, size), Image.LANCZOS)
    part = part.filter(ImageFilter.UnsharpMask(radius=0.8, percent=50, threshold=2))
    return rounded(steel(part), 0.12 if size > 16 else 0.08)


def ico_bytes(images) -> bytes:
    """An .ico of PNG-compressed entries (Windows Vista and later; rc.exe embeds it as it is)."""
    pngs = []
    for img in images:
        buf = io.BytesIO()
        img.save(buf, "PNG", optimize=True)
        pngs.append((img.width, img.height, buf.getvalue()))
    out = struct.pack("<HHH", 0, 1, len(pngs))
    offset = 6 + 16 * len(pngs)
    for w, h, data in pngs:
        out += struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    return out + b"".join(data for _, _, data in pngs)


def preview(images, path: pathlib.Path) -> None:
    from PIL import Image
    pad = 12
    w = sum(i.width for i in images) + pad * (len(images) + 1)
    h = 256 * 2 + pad * 3
    sheet = Image.new("RGBA", (w, h), (255, 255, 255, 255))
    sheet.paste((32, 32, 36, 255), (0, h // 2, w, h))
    for y0 in (pad, h // 2 + pad // 2):
        x = pad
        for img in images:
            sheet.alpha_composite(img, (x, y0))
            x += img.width + pad
    sheet.convert("RGB").save(path)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", required=True, type=pathlib.Path, help="the disc's extracted files (TFA.ZIP in it)")
    ap.add_argument("--out", required=True, type=pathlib.Path, help="the .ico to write")
    ap.add_argument("--preview", type=pathlib.Path, help="also a PNG sheet of every size")
    args = ap.parse_args()
    try:
        itf = read_texture(args.game)
        if itf is None:
            print(f"make_exe_icon: no {ARCHIVE} with {ENTRY} in {args.game} (the disc is not extracted yet): no icon")
            return 2
        backdrop = decode(itf)
        images = [frame(backdrop, s) for s in SIZES]
        data = ico_bytes(images)
        if not (args.out.is_file() and args.out.read_bytes() == data):
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_bytes(data)
        if args.preview:
            preview(images, args.preview)
        print(f"make_exe_icon: {args.out} ({', '.join(str(s) for s in SIZES)} px)")
        return 0
    except Exception as exc:  # (the build goes on without an icon: say why)
        print(f"make_exe_icon: failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
