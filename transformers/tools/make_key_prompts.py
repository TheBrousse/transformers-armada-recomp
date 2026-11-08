"""Keyboard prompts: the game's on-screen button icons redrawn as the keyboard and mouse keys.

The game prints its button prompts ("X Select Autobot", "%c Open", "%c Jump", ...) as characters
of its menu font DECKFONT (TFA2/FONTS/DECKFONT.VBN), whose atlas TFA2/FONTS/DECK.ITF holds the
PS2 buttons beside the letters: '~' cross, 127 circle, '|' square, '}' triangle, 128 SELECT, 129 L1,
130 L2, 131 R1, 132 R2, 133 L3, 134 R3, 135-137 and 145 the D-pad's left, right, up, down. This tool
makes a texture-pack replacement of that atlas (replacements\\<its pack name>.dds, the pack's
mechanism, so nothing in the game changes): the letters as they are (from the upscaled atlas), every
button cell redrawn as a key cap of the key bound to it: the port's keyboard and mouse layout
(controls.txt beside the game, the mod menu's Controls page), else the port's defaults. A button's
prompt shows its first key, or its mouse button when the first key is Alt, Ctrl or Shift.

Run it again after changing the controls; the game picks the new file up at its next start.

Usage:
    python make_key_prompts.py --base <upscaled DECK.png> --out <game>\\replacements
        [--controls <game>\\controls.txt] [--fonts <game>\\fonts] [--scale 4] [--preview <png>]

--base: the atlas at its upscaled size (the remaster project's Extracted\\upscaled\\TFA2\\FONTS\\DECK.png;
the decoded one works too, smaller). Needs Pillow and numpy.
"""

import argparse
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_texture_pack import write_dds  # noqa: E402

# The atlas's pack name (the dump of it, 256 x 256, PSMT4/8 with its palette).
PACK_NAME = "99ef5a9eb1ea110-4229cc3f80932a00-00002213"
ATLAS = 256  # the atlas's own size: the cells below are in its pixels (from DECKFONT's glyph table)

# button -> (cell x0, y0, x1, y1), as DECKFONT's glyphs place them (V counted from the bottom)
CELLS = {
    "cross": (4, 230, 28, 254),
    "circle": (28, 230, 51, 254),
    "square": (51, 230, 73, 254),
    "triangle": (73, 230, 97, 254),
    "select": (118, 206, 158, 230),
    "l1": (62, 182, 91, 201),
    "l2": (1, 163, 30, 190),
    "r1": (62, 163, 91, 182),
    "r2": (32, 163, 60, 190),
    "l3": (1, 192, 30, 220),
    "r3": (32, 192, 60, 220),
    "left": (96, 179, 113, 196),
    "right": (96, 163, 113, 179),
    "up": (96, 196, 113, 214),
    "down": (96, 214, 113, 233),
}

# The port's default layout (ps2_pad.cpp's kDefaultKeymap)
DEFAULT_LAYOUT = (
    "up=UP;down=DOWN;left=LEFT;right=RIGHT;cross=X;circle=C,BACKSPACE;square=R,KP_0;"
    "triangle=T,V,F,KP_1;l1=SPACE,Q,MOUSE_MIDDLE;r1=LEFT_ALT,MOUSE_LEFT;l2=Z,LEFT_SHIFT;"
    "r2=E,LEFT_CONTROL,RIGHT_SHIFT;l3=G;r3=B,MOUSE_RIGHT;start=ENTER,ESCAPE,P;select=TAB")

SHORT = {"SPACE": "SPACE", "ENTER": "ENTER", "ESCAPE": "ESC", "TAB": "TAB", "BACKSPACE": "BKSP",
         "INSERT": "INS", "DELETE": "DEL", "HOME": "HOME", "END": "END", "PAGE_UP": "PGUP",
         "PAGE_DOWN": "PGDN", "LEFT_SHIFT": "SHIFT", "RIGHT_SHIFT": "SHIFT", "LEFT_CONTROL": "CTRL",
         "RIGHT_CONTROL": "CTRL", "LEFT_ALT": "ALT", "RIGHT_ALT": "ALT", "CAPS_LOCK": "CAPS",
         "GRAVE": "`", "MINUS": "-", "EQUAL": "=", "LEFT_BRACKET": "[", "RIGHT_BRACKET": "]",
         "BACKSLASH": "\\", "SEMICOLON": ";", "APOSTROPHE": "'", "COMMA": ",", "PERIOD": ".", "SLASH": "/",
         "KP_ENTER": "ENT", "KP_ADD": "+", "KP_SUBTRACT": "-"}
ARROWS = {"UP", "DOWN", "LEFT", "RIGHT"}
MODIFIERS = {"LEFT_SHIFT", "RIGHT_SHIFT", "LEFT_CONTROL", "RIGHT_CONTROL", "LEFT_ALT", "RIGHT_ALT"}


def parse_layout(text):
    out = {}
    for part in text.split(";"):
        if "=" in part:
            k, v = part.split("=", 1)
            out[k.strip().lower()] = [x.strip().upper() for x in v.split(",") if x.strip()]
    return out


def shown_key(keys):
    """The key a prompt shows for a button: its first, or its mouse button behind a modifier."""
    if not keys:
        return None
    mouse = [k for k in keys if k.startswith("MOUSE_")]
    if keys[0] in MODIFIERS and mouse:
        return mouse[0]
    return keys[0]


def font(fonts_dir, size):
    for name in ("Bahnschrift.ttf", "bahnschrift.ttf"):
        for d in (fonts_dir, "C:/Windows/Fonts"):
            if d and os.path.exists(os.path.join(d, name)):
                f = ImageFont.truetype(os.path.join(d, name), size)
                try:
                    f.set_variation_by_name("Bold")
                except Exception:
                    pass
                return f
    return ImageFont.load_default()


KEY_FACE = (236, 239, 244, 255)
KEY_EDGE = (120, 126, 138, 255)
KEY_RIM = (40, 44, 52, 255)
INK = (18, 20, 26, 255)
ACCENT = (214, 32, 47, 255)  # Autobot red: the mouse button that is meant


def draw_cap(d, box, s):
    """A key cap filling the box: dark rim, grey lower edge (its depth), light face."""
    x0, y0, x1, y1 = box
    r = max(2, int(3 * s))
    d.rounded_rectangle((x0, y0, x1, y1), r, fill=KEY_RIM)
    d.rounded_rectangle((x0 + s * 0.6, y0 + s * 0.6, x1 - s * 0.6, y1 - s * 0.6), r, fill=KEY_EDGE)
    d.rounded_rectangle((x0 + s * 0.9, y0 + s * 0.8, x1 - s * 0.9, y1 - s * 2.0), r, fill=KEY_FACE)
    return (x0 + s * 0.9, y0 + s * 0.8, x1 - s * 0.9, y1 - s * 2.0)


def draw_label(d, face, text, fonts_dir):
    fx0, fy0, fx1, fy1 = face
    w, h = fx1 - fx0, fy1 - fy0
    size = int(h * 0.78)
    while size > 6:
        f = font(fonts_dir, size)
        l, t, r, b = d.textbbox((0, 0), text, font=f)
        if r - l <= w * 0.86 and b - t <= h * 0.8:
            break
        size -= 1
    l, t, r, b = d.textbbox((0, 0), text, font=f)
    d.text((fx0 + (w - (r - l)) / 2 - l, fy0 + (h - (b - t)) / 2 - t), text, font=f, fill=INK)


def draw_arrow(d, face, which):
    fx0, fy0, fx1, fy1 = face
    cx, cy = (fx0 + fx1) / 2, (fy0 + fy1) / 2
    k = min(fx1 - fx0, fy1 - fy0) * 0.3
    pts = {"UP": [(cx, cy - k), (cx - k, cy + k * 0.7), (cx + k, cy + k * 0.7)],
           "DOWN": [(cx, cy + k), (cx - k, cy - k * 0.7), (cx + k, cy - k * 0.7)],
           "LEFT": [(cx - k, cy), (cx + k * 0.7, cy - k), (cx + k * 0.7, cy + k)],
           "RIGHT": [(cx + k, cy), (cx - k * 0.7, cy - k), (cx - k * 0.7, cy + k)]}[which]
    d.polygon(pts, fill=INK)


def draw_mouse(d, box, which, s):
    """A mouse, its button that is meant in red (left, right, middle; the side ones: middle)."""
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    mw, mh = min(w * 0.62, h * 0.5), h * 0.86
    mx0, my0 = x0 + (w - mw) / 2, y0 + (h - mh) / 2
    mx1, my1 = mx0 + mw, my0 + mh
    rr = mw * 0.48
    d.rounded_rectangle((mx0, my0, mx1, my1), rr, fill=KEY_FACE, outline=KEY_RIM, width=max(1, int(s * 0.8)))
    split = my0 + mh * 0.42
    cx = (mx0 + mx1) / 2
    if which == "MOUSE_LEFT":
        d.pieslice((mx0, my0, mx1, my0 + mw), 180, 270, fill=ACCENT)
        d.rectangle((mx0 + s * 0.5, my0 + mw / 2, cx, split), fill=ACCENT)
    elif which == "MOUSE_RIGHT":
        d.pieslice((mx0, my0, mx1, my0 + mw), 270, 360, fill=ACCENT)
        d.rectangle((cx, my0 + mw / 2, mx1 - s * 0.5, split), fill=ACCENT)
    else:
        d.rounded_rectangle((cx - mw * 0.11, my0 + mh * 0.1, cx + mw * 0.11, split - mh * 0.04), mw * 0.1, fill=ACCENT)
    d.line((mx0 + s * 0.5, split, mx1 - s * 0.5, split), fill=KEY_RIM, width=max(1, int(s * 0.6)))
    d.line((cx, my0 + s * 0.5, cx, split), fill=KEY_RIM, width=max(1, int(s * 0.6)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--base", required=True, help="the atlas (upscaled DECK.png)")
    ap.add_argument("--out", required=True, help="the game's replacements folder")
    ap.add_argument("--controls", help="controls.txt (the mod menu's layout); else the defaults")
    ap.add_argument("--fonts", help="the game's fonts folder (Bahnschrift.ttf), else Windows'")
    ap.add_argument("--scale", type=int, default=4, help="the replacement's size over the atlas's 256 (default 4)")
    ap.add_argument("--preview", help="also write a preview PNG here")
    args = ap.parse_args()

    layout = parse_layout(DEFAULT_LAYOUT)
    if args.controls and os.path.exists(args.controls):
        with open(args.controls, encoding="utf-8") as f:
            layout.update(parse_layout(f.read().strip()))

    size = ATLAS * args.scale
    base = Image.open(args.base).convert("RGBA").resize((size, size), Image.LANCZOS)
    s = float(args.scale)
    d = ImageDraw.Draw(base)
    # The old icons go first (a pixel or two past their cells at the atlas's size), then the caps.
    for x0, y0, x1, y1 in CELLS.values():
        d.rectangle(((x0 - 1) * s, (y0 - 1) * s, (x1 + 2) * s, (y1 + 1) * s), fill=(0, 0, 0, 0))
    for button, (x0, y0, x1, y1) in CELLS.items():
        box = (x0 * s, y0 * s, x1 * s - 1, y1 * s - 1)
        key = shown_key(layout.get(button, []))
        if not key:
            continue
        if key.startswith("MOUSE_"):
            draw_mouse(d, box, key, s)
        else:
            face = draw_cap(d, box, s)
            if key in ARROWS:
                draw_arrow(d, face, key)
            else:
                draw_label(d, face, SHORT.get(key, key.replace("KP_", "NUM")), args.fonts)
        print(f"{button:9s} -> {key}")

    rgba = np.asarray(base).copy()
    if args.preview:
        Image.fromarray(rgba, "RGBA").save(args.preview)
    rgba[:, :, 3] = np.round(rgba[:, :, 3].astype(np.float32) * (128.0 / 255.0)).astype(np.uint8)
    os.makedirs(args.out, exist_ok=True)
    out = os.path.join(args.out, PACK_NAME + ".dds")
    write_dds(out, rgba)
    print(f"wrote {out} ({size} x {size})")


if __name__ == "__main__":
    main()
