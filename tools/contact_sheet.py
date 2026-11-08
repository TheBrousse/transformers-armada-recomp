"""Tiles a folder of frames into one labelled image (debugging aid).

    python tools/contact_sheet.py runs/explore2 runs/explore2_sheet.png [--cols 8] [--width 240]
"""
import argparse
import re
from pathlib import Path

from PIL import Image, ImageDraw


def frame_key(path):
    numbers = re.findall(r"\d+", path.stem)
    return int(numbers[-1]) if numbers else 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("folder")
    parser.add_argument("output")
    parser.add_argument("--cols", type=int, default=8)
    parser.add_argument("--width", type=int, default=240)
    parser.add_argument("--first", type=int, default=0)
    parser.add_argument("--count", type=int, default=64)
    args = parser.parse_args()

    paths = sorted(Path(args.folder).glob("*.png"), key=frame_key)[args.first : args.first + args.count]
    if not paths:
        print("no frames")
        return
    first = Image.open(paths[0])
    thumb_w = args.width
    thumb_h = round(first.height * thumb_w / first.width)
    rows = (len(paths) + args.cols - 1) // args.cols
    sheet = Image.new("RGB", (args.cols * thumb_w, rows * (thumb_h + 14)), (40, 40, 40))
    draw = ImageDraw.Draw(sheet)
    for index, path in enumerate(paths):
        x = (index % args.cols) * thumb_w
        y = (index // args.cols) * (thumb_h + 14)
        sheet.paste(Image.open(path).convert("RGB").resize((thumb_w, thumb_h)), (x, y + 14))
        draw.text((x + 3, y + 1), path.stem, fill=(255, 255, 255))
    sheet.save(args.output)
    print(f"{len(paths)} frames -> {args.output} ({sheet.width}x{sheet.height})")


if __name__ == "__main__":
    main()
