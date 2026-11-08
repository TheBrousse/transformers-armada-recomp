"""Draws a level's ground from above, from the game's collision data (testing aid).

    python tools/level_heightmap.py game/TFA.ZIP 01_AMAZON_1 --x 780 980 --y -1790 -1650 -o runs/map.png
    python tools/level_heightmap.py game/TFA.ZIP 01_AMAZON_1 --x 780 980 --y -1790 -1650 --mark 852.6,-1701.8 --mark 759.5,-1666.9
    python tools/level_heightmap.py game/TFA.ZIP 01_AMAZON_1 --height 832.6,-1694     # the ground heights at a place

For finding a way through a level when walking it with scripted input: the picture shows
the height of the ground one can stand on (the highest such surface under --ceiling), with a
contour every --contour units, steep faces in dark grey, and marked places as crosses.

The data: LEVELS/EPISODES/<level>/COLLISION/COLLISIONSCENE.COLLISIONSCENE in the archive -
"FISH", a header with the material names, then ~CollSceneStrips: a version, the range the
coordinates are packed into (256), the strip count and a byte count, and per strip a
material, the centre and half size of its box, a vertex count and that many vertices of
three 16-bit numbers: centre + (n - 32768) / 128. Vertices form a triangle strip; x, y, z
with z up.
"""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from level_route import read_from_zip  # noqa: E402


def read_strips(data):
    """Returns (materials, list of (material index, vertices[n, 3]))."""
    if data[:4] != b"FISH":
        raise SystemExit("not a FISH file")
    count, = struct.unpack_from("<I", data, 12)
    pos = 16
    names = []
    for _ in range(count):
        length, = struct.unpack_from("<I", data, pos)
        names.append(data[pos + 4:pos + 4 + length].decode("latin1"))
        pos += 4 + length + 4  # the text, then four zero bytes
    tag = data.index(b"~CollSceneStrips\0", pos)
    pos = tag + len(b"~CollSceneStrips\0") + 1
    _size, _version, packed_range, strip_count, _bytes = struct.unpack_from("<IIfII", data, pos)
    pos += 20
    scale = packed_range / 32768.0
    strips = []
    for _ in range(strip_count):
        material, cx, cy, cz, _ex, _ey, _ez, vertices = struct.unpack_from("<I6fI", data, pos)
        pos += 32
        packed = np.frombuffer(data, dtype="<u2", count=vertices * 3, offset=pos).reshape(vertices, 3)
        pos += vertices * 6
        strips.append((material, (packed.astype(np.float32) - 32768.0) * scale + np.array([cx, cy, cz], dtype=np.float32)))
    return names, strips


def triangles(strips, names, skip=("InvisibleWall", "WaterNoRipple", "water")):
    out = []
    for material, vertices in strips:
        if names[material] in skip:
            continue
        for i in range(len(vertices) - 2):
            a, b, c = vertices[i], vertices[i + 1], vertices[i + 2]
            if (a == b).all() or (b == c).all() or (a == c).all():
                continue
            out.append((a, b, c))
    return np.array(out, dtype=np.float32)


def raster(tris, x0, x1, y0, y1, cell, ceiling):
    """Height of the highest standable surface under the ceiling, and whether a steep face is there."""
    width = int(round((x1 - x0) / cell))
    height = int(round((y1 - y0) / cell))
    ground = np.full((height, width), np.nan, dtype=np.float32)
    steep = np.zeros((height, width), dtype=bool)
    normals = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
    lengths = np.linalg.norm(normals, axis=1)
    for tri, normal, length in zip(tris, normals, lengths):
        if length == 0:
            continue
        lo = tri.min(axis=0)
        hi = tri.max(axis=0)
        if hi[0] < x0 or lo[0] > x1 or hi[1] < y0 or lo[1] > y1 or lo[2] > ceiling:
            continue
        flat = abs(normal[2]) / length  # 1 = level ground
        i0 = max(int((lo[0] - x0) / cell), 0)
        i1 = min(int((hi[0] - x0) / cell) + 1, width)
        j0 = max(int((lo[1] - y0) / cell), 0)
        j1 = min(int((hi[1] - y0) / cell) + 1, height)
        if i0 >= i1 or j0 >= j1:
            continue
        xs = x0 + (np.arange(i0, i1) + 0.5) * cell
        ys = y0 + (np.arange(j0, j1) + 0.5) * cell
        gx, gy = np.meshgrid(xs, ys)
        (ax, ay, az), (bx, by, bz), (cx, cy, cz) = tri
        det = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        if abs(det) < 1e-9:
            continue
        u = ((by - cy) * (gx - cx) + (cx - bx) * (gy - cy)) / det
        v = ((cy - ay) * (gx - cx) + (ax - cx) * (gy - cy)) / det
        w = 1.0 - u - v
        inside = (u >= -1e-4) & (v >= -1e-4) & (w >= -1e-4)
        if not inside.any():
            continue
        z = u * az + v * bz + w * cz
        if flat < 0.7:  # steeper than 45 degrees: not ground
            steep[j0:j1, i0:i1] |= inside & (z <= ceiling)
            continue
        view = ground[j0:j1, i0:i1]
        better = inside & (z <= ceiling) & (np.isnan(view) | (z > view))
        view[better] = z[better]
    return ground, steep


def heights_at(tris, x, y):
    out = []
    normals = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
    for tri, normal in zip(tris, normals):
        (ax, ay, az), (bx, by, bz), (cx, cy, cz) = tri
        det = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        if abs(det) < 1e-9:
            continue
        u = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) / det
        v = ((cy - ay) * (x - cx) + (ax - cx) * (y - cy)) / det
        w = 1.0 - u - v
        if u < 0 or v < 0 or w < 0:
            continue
        length = float(np.linalg.norm(normal))
        out.append((float(u * az + v * bz + w * cz), abs(float(normal[2])) / length if length else 0.0))
    return sorted(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("archive")
    parser.add_argument("level")
    parser.add_argument("--x", nargs=2, type=float, metavar=("FROM", "TO"))
    parser.add_argument("--y", nargs=2, type=float, metavar=("FROM", "TO"))
    parser.add_argument("--cell", type=float, default=0.5, help="size of a picture element in level units")
    parser.add_argument("--ceiling", type=float, default=1e9, help="ignore surfaces above this height")
    parser.add_argument("--contour", type=float, default=2.0)
    parser.add_argument("--mark", action="append", default=[], help="x,y to mark with a cross (repeatable)")
    parser.add_argument("--height", help="x,y: list the surfaces at that place instead of drawing")
    parser.add_argument("-o", "--output", default="runs/map.png")
    args = parser.parse_args()

    names, strips = read_strips(read_from_zip(args.archive, f"LEVELS/EPISODES/{args.level}/COLLISION/COLLISIONSCENE.COLLISIONSCENE"))
    tris = triangles(strips, names)
    print(f"{len(strips)} strips, {len(tris)} triangles; materials: {', '.join(names[1:])}")
    if args.height:
        x, y = (float(v) for v in args.height.split(","))
        for z, flat in heights_at(tris, x, y):
            print(f"  z {z:8.2f}  {'ground' if flat >= 0.7 else 'steep'} ({flat:.2f})")
        return
    if not args.x or not args.y:
        raise SystemExit("give --x and --y (or --height)")

    from PIL import Image, ImageDraw

    ground, steep = raster(tris, args.x[0], args.x[1], args.y[0], args.y[1], args.cell, args.ceiling)
    known = ~np.isnan(ground)
    low, high = float(np.nanmin(ground)), float(np.nanmax(ground))
    print(f"ground from {low:.1f} to {high:.1f}")
    t = np.where(known, (ground - low) / max(high - low, 1e-6), 0.0)
    # Blue (low) through green and yellow to red (high).
    red = np.clip(1.5 - np.abs(4 * t - 3), 0, 1)
    green = np.clip(1.5 - np.abs(4 * t - 2), 0, 1)
    blue = np.clip(1.5 - np.abs(4 * t - 1), 0, 1)
    image = np.stack([red, green, blue], axis=2)
    lines = known & (np.floor(ground / args.contour) != np.floor(np.roll(ground, 1, axis=0) / args.contour))
    lines |= known & (np.floor(ground / args.contour) != np.floor(np.roll(ground, 1, axis=1) / args.contour))
    image[lines] *= 0.6
    image[~known] = 0.0
    image[steep & ~known] = 0.25
    picture = Image.fromarray((image[::-1] * 255).astype(np.uint8))  # y upwards
    scale = max(1, int(900 / max(picture.size)))
    picture = picture.resize((picture.width * scale, picture.height * scale), Image.NEAREST)
    draw = ImageDraw.Draw(picture)
    for mark in args.mark:
        mx, my = (float(v) for v in mark.split(",")[:2])
        px = (mx - args.x[0]) / args.cell * scale
        py = (args.y[1] - my) / args.cell * scale
        draw.line((px - 6, py - 6, px + 6, py + 6), fill=(255, 255, 255), width=2)
        draw.line((px - 6, py + 6, px + 6, py - 6), fill=(255, 255, 255), width=2)
    # A grid line every 20 units, labelled.
    for gx in range(int(np.ceil(args.x[0] / 20) * 20), int(args.x[1]) + 1, 20):
        px = (gx - args.x[0]) / args.cell * scale
        draw.line((px, 0, px, picture.height), fill=(255, 255, 255, 60), width=1)
        draw.text((px + 2, 2), str(gx), fill=(255, 255, 255))
    for gy in range(int(np.ceil(args.y[0] / 20) * 20), int(args.y[1]) + 1, 20):
        py = (args.y[1] - gy) / args.cell * scale
        draw.line((0, py, picture.width, py), fill=(255, 255, 255, 60), width=1)
        draw.text((2, py + 2), str(gy), fill=(255, 255, 255))
    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    picture.save(args.output)
    print("wrote", args.output, picture.size)


if __name__ == "__main__":
    main()
