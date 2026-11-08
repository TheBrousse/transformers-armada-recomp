"""Finds a way over a level's ground for the scripted walk (testing aid).

    python tools/level_ground.py game/TFA.ZIP 01_AMAZON_1 --from 832.6,-1694,10 --to 806,-1680,33
    python tools/level_ground.py game/TFA.ZIP 01_AMAZON_1 --from ... --to ... --png runs/way.png

The level's collision mesh (see tools/level_heightmap.py) is cut into squares of one unit;
every square knows the heights of the surfaces one can stand on there, and which heights are
taken up by steep faces. A step to the next square is a walk when the ground rises or falls
no faster than a slope one can walk, a jump when it is a ledge of up to --jump units, or a
drop; not possible when a steep face stands where the body would be, or the ledge is too
high. The cheapest chain of steps (A*; jumps and drops cost extra, squares next to walls
too) is thinned out to the places where it turns or jumps and printed in the form
tools\\play.ps1 -Goto takes: "tick:x,y,radius", with a "j" after the radius where the
walk has to jump to get there.

Heights are those of the ground. The player's own position (the "[tf:goto]" log) is that of
its body, 3.85 units above its feet: give --from with the ground's height, or with --body to
have that taken off.
"""
import argparse
import heapq
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from level_heightmap import read_strips, triangles  # noqa: E402
from level_route import read_from_zip  # noqa: E402

BODY_ABOVE_FEET = 3.85


class Ground:
    """Standable heights and steep faces per square of a region."""

    def __init__(self, tris, x0, x1, y0, y1, cell=1.0):
        self.x0, self.y0, self.cell = x0, y0, cell
        self.width = int(math.ceil((x1 - x0) / cell))
        self.height = int(math.ceil((y1 - y0) / cell))
        self.floors = [[[] for _ in range(self.width)] for _ in range(self.height)]
        self.walls = [[[] for _ in range(self.width)] for _ in range(self.height)]  # (low, high) of steep faces
        normals = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
        lengths = np.linalg.norm(normals, axis=1)
        for tri, normal, length in zip(tris, normals, lengths):
            if length == 0:
                continue
            lo = tri.min(axis=0)
            hi = tri.max(axis=0)
            if hi[0] < x0 or lo[0] > x1 or hi[1] < y0 or lo[1] > y1:
                continue
            flat = abs(normal[2]) / length
            i0 = max(int((lo[0] - x0) / cell), 0)
            i1 = min(int((hi[0] - x0) / cell) + 1, self.width)
            j0 = max(int((lo[1] - y0) / cell), 0)
            j1 = min(int((hi[1] - y0) / cell) + 1, self.height)
            (ax, ay, az), (bx, by, bz), (cx, cy, cz) = tri
            det = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
            steep = flat < 0.7
            if abs(det) < 1e-9:
                # Seen edge-on from above: a wall. It fills the squares its outline crosses.
                if steep:
                    for j in range(j0, j1):
                        for i in range(i0, i1):
                            self.walls[j][i].append((float(lo[2]), float(hi[2])))
                continue
            for j in range(j0, j1):
                py = y0 + (j + 0.5) * cell
                for i in range(i0, i1):
                    px = x0 + (i + 0.5) * cell
                    u = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / det
                    v = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / det
                    w = 1.0 - u - v
                    if u < -0.02 or v < -0.02 or w < -0.02:
                        continue
                    z = float(u * az + v * bz + w * cz)
                    if steep:
                        # The part of the face over this square, roughly: half a square's rise each way.
                        rise = 0.5 * cell * math.sqrt(max(1.0 - flat * flat, 0.0)) / max(flat, 0.05)
                        self.walls[j][i].append((max(z - rise, float(lo[2])), min(z + rise, float(hi[2]))))
                    else:
                        self.floors[j][i].append(z)
        for row in self.floors:
            for k, heights in enumerate(row):
                heights.sort()
                merged = []
                for z in heights:
                    if not merged or z - merged[-1] > 0.75:
                        merged.append(z)
                    else:
                        merged[-1] = z
                row[k] = merged

    def square(self, x, y):
        return int((x - self.x0) / self.cell), int((y - self.y0) / self.cell)

    def centre(self, i, j):
        return self.x0 + (i + 0.5) * self.cell, self.y0 + (j + 0.5) * self.cell

    def inside(self, i, j):
        return 0 <= i < self.width and 0 <= j < self.height

    def blocked(self, i, j, z, body=7.0):
        """Something where the body would be when standing at height z: a steep face, or
        another surface closely above (the ground found under a boulder is inside it)."""
        if any(low < z + body and high > z + 1.0 for low, high in self.walls[j][i]):
            return True
        return any(z + 0.75 < floor < z + body for floor in self.floors[j][i])

    def floor_below(self, i, j, z, slack=1.0):
        """The highest floor at or below z + slack."""
        best = None
        for floor in self.floors[j][i]:
            if floor <= z + slack:
                best = floor
        return best


def plan(ground, start, goal, jump=5.0, drop=30.0, climb=1.1):
    si, sj = ground.square(start[0], start[1])
    gi, gj = ground.square(goal[0], goal[1])
    if not ground.inside(si, sj) or not ground.inside(gi, gj):
        raise SystemExit("start or goal outside the region")
    sz = ground.floor_below(si, sj, start[2], 2.0)
    gz = ground.floor_below(gi, gj, goal[2], 2.0)
    if sz is None or gz is None:
        raise SystemExit(f"no ground under the {'start' if sz is None else 'goal'}")
    first = (si, sj, sz)
    queue = [(0.0, 0.0, first, None, "start")]
    came = {}
    best = {first: 0.0}
    steps = ((1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1))
    last = None
    while queue:
        _, cost, node, parent, how = heapq.heappop(queue)
        if node in came:
            continue
        came[node] = (parent, how)
        i, j, z = node
        if (i, j) == (gi, gj) and abs(z - gz) < 1.5:
            last = node
            break
        for di, dj in steps:
            ni, nj = i + di, j + dj
            if not ground.inside(ni, nj):
                continue
            length = math.hypot(di, dj) * ground.cell
            # Where one ends up there: the highest floor a jump reaches.
            floors = [f for f in ground.floors[nj][ni] if f <= z + jump]
            if not floors:
                continue
            nz = floors[-1]
            if z - nz > drop:
                continue
            rise = nz - z
            if rise > climb * length:
                kind, extra = "jump", 6.0 + rise
            elif rise < -climb * length:
                kind, extra = "drop", 2.0
            else:
                kind, extra = "walk", 0.0
            if ground.blocked(ni, nj, nz):
                continue
            if kind == "walk" and di and dj and (ground.blocked(ni, j, z) or ground.blocked(i, nj, z)):
                continue  # no cutting corners of walls
            # Keep off the walls: a square with a wall next to it costs more.
            near = sum(1 for a, b in steps[:4] if ground.inside(ni + a, nj + b) and ground.blocked(ni + a, nj + b, nz))
            new = cost + length + extra + 1.5 * near
            other = (ni, nj, nz)
            if new < best.get(other, math.inf):
                best[other] = new
                heuristic = math.hypot(gi - ni, gj - nj) * ground.cell
                heapq.heappush(queue, (new + heuristic, new, other, node, kind))
    if last is None:
        return None
    path = []
    node = last
    while node is not None:
        parent, how = came[node]
        path.append((node, how))
        node = parent
    path.reverse()
    return path


def thin(ground, path, spacing=6.0):
    """The places worth walking to: every jump and drop, turns, and one every `spacing` units."""
    places = []
    since = 0.0
    for index, (node, how) in enumerate(path):
        i, j, z = node
        x, y = ground.centre(i, j)
        if index == 0:
            continue
        pi, pj, _ = path[index - 1][0]
        since += math.hypot(i - pi, j - pj) * ground.cell
        final = index == len(path) - 1
        turn = False
        if index + 1 < len(path):
            ni, nj, _ = path[index + 1][0]
            turn = (ni - i, nj - j) != (i - pi, j - pj)
        if how == "jump":
            # Stand at the foot of the ledge first, then jump onto it.
            px, py = ground.centre(pi, pj)
            if not places or places[-1][:2] != (px, py):
                places.append((px, py, path[index - 1][0][2], "walk"))
            places.append((x, y, z, "jump"))
            since = 0.0
        elif final or how == "drop" or (turn and since >= 3.0) or since >= spacing:
            places.append((x, y, z, how))
            since = 0.0
    return places


def point(text):
    values = [float(v) for v in text.split(",")]
    if len(values) != 3:
        raise argparse.ArgumentTypeError("x,y,z")
    return tuple(values)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("archive")
    parser.add_argument("level")
    parser.add_argument("--from", dest="start", type=point, required=True, help="x,y,z of the ground to start from")
    parser.add_argument("--to", dest="goal", type=point, required=True, help="x,y,z of the ground to get to")
    parser.add_argument("--body", action="store_true", help="--from is the player's own position (above its feet)")
    parser.add_argument("--jump", type=float, default=5.0, help="highest ledge a jump gets onto")
    parser.add_argument("--margin", type=float, default=50.0, help="how far around start and goal to look")
    parser.add_argument("--tick", type=int, default=0, help="tick to start the walk at (for -Goto)")
    parser.add_argument("--radius", type=float, default=2.0)
    parser.add_argument("--png", help="draw the way on a map of the region")
    args = parser.parse_args()

    start = args.start
    if args.body:
        start = (start[0], start[1], start[2] - BODY_ABOVE_FEET)
    names, strips = read_strips(read_from_zip(args.archive, f"LEVELS/EPISODES/{args.level}/COLLISION/COLLISIONSCENE.COLLISIONSCENE"))
    tris = triangles(strips, names, skip=("WaterNoRipple", "water"))  # invisible walls stand in the way too
    x0 = min(start[0], args.goal[0]) - args.margin
    x1 = max(start[0], args.goal[0]) + args.margin
    y0 = min(start[1], args.goal[1]) - args.margin
    y1 = max(start[1], args.goal[1]) + args.margin
    ground = Ground(tris, x0, x1, y0, y1)
    path = plan(ground, start, args.goal, jump=args.jump)
    if path is None:
        print("no way found (try a larger --margin or --jump)")
        return
    places = thin(ground, path)
    length = sum(math.hypot(b[0][0] - a[0][0], b[0][1] - a[0][1]) for a, b in zip(path, path[1:]))
    jumps = sum(1 for _, how in path if how == "jump")
    print(f"{length:.0f} units, {jumps} jumps, {len(places)} places")
    items = []
    for x, y, z, how in places:
        print(f"  {how:5s} {x:8.1f} {y:8.1f}  ground {z:6.1f}")
        items.append(f"{args.tick}:{x:.1f},{y:.1f},{args.radius:g}" + ("j" if how == "jump" else ""))
    print("-Goto \"" + ";".join(items) + "\"")
    if args.png:
        from PIL import Image, ImageDraw

        scale = max(1, int(900 / max(ground.width, ground.height)))
        heights = np.full((ground.height, ground.width), np.nan, dtype=np.float32)
        for j in range(ground.height):
            for i in range(ground.width):
                floors = [f for f in ground.floors[j][i] if f <= max(start[2], args.goal[2]) + 20]
                if floors:
                    heights[j, i] = floors[-1]
        low, high = float(np.nanmin(heights)), float(np.nanmax(heights))
        t = np.where(np.isnan(heights), 0.0, (heights - low) / max(high - low, 1e-6))
        image = np.stack([np.clip(1.5 - np.abs(4 * t - 3), 0, 1), np.clip(1.5 - np.abs(4 * t - 2), 0, 1),
                          np.clip(1.5 - np.abs(4 * t - 1), 0, 1)], axis=2)
        image[np.isnan(heights)] = 0.0
        picture = Image.fromarray((image[::-1] * 255).astype(np.uint8)).resize((ground.width * scale, ground.height * scale), Image.NEAREST)
        draw = ImageDraw.Draw(picture)

        def pixel(x, y):
            return (x - ground.x0) / ground.cell * scale, (ground.y0 + ground.height * ground.cell - y) / ground.cell * scale

        points = [pixel(*ground.centre(n[0], n[1])) for n, _ in path]
        draw.line(points, fill=(255, 255, 255), width=2)
        for x, y, _, how in places:
            px, py = pixel(x, y)
            colour = (255, 0, 255) if how == "jump" else (0, 0, 0)
            draw.ellipse((px - 3, py - 3, px + 3, py + 3), outline=colour, width=2)
        picture.save(args.png)
        print("wrote", args.png)


if __name__ == "__main__":
    main()
