"""Plans a walk through a level from the game's own pathfinding data (testing aid).

    python tools/level_route.py game/TFA.ZIP 01_AMAZON_1 --from 852.6,-1701.8,18.3 --to 759.5,-1666.9,45
    python tools/level_route.py game/TFA.ZIP 01_AMAZON_1 --info            # what the file holds
    python tools/level_route.py game/TFA.ZIP 01_AMAZON_1 --at 852.6,-1701.8,18.3   # the sector there

A level's PATHFINDER.VBIN (inside the game's archive) is what its creatures navigate by: the
walkable space cut into convex sectors - boxes, or polygons with a height range - and the
portals that join two sectors: plain ones to walk through and jump portals. The route is the
shortest chain of portals from the sector under the start to the one under the goal; printed
as places to walk to in turn, in the form tools\\play.ps1 -Goto (TF_GOTO) takes. Where the
chain crosses a jump portal the line is marked: the walk has to jump there.

The file: "FISH", a version, then chunks of a "~Name", two zero bytes and a 32-bit size
(counting itself). ~taPathfindSectors holds a count and ~Sector chunks (id, box corners,
flags; with flag 0x10 a base height, a height, a corner count and that many x,y corners);
~taPathfindPortals holds ~Portal (position, width, axis, the two sector ids),
~Portal_NonAligned (position, direction, two extents, a zero, the ids) and ~JumpPortal
(six points, a zero, the ids).
"""
import argparse
import heapq
import math
import struct
import zlib
from pathlib import Path


def read_from_zip(archive, wanted):
    """The game's ZIPs keep the central directory in front; names compare without case."""
    data = Path(archive).read_bytes()
    wanted = wanted.upper().replace("\\", "/")
    pos = 0
    while True:
        pos = data.find(b"PK\x01\x02", pos)
        if pos < 0:
            raise SystemExit(f"{wanted} is not in {archive}")
        comp, = struct.unpack_from("<H", data, pos + 10)
        csize, usize, nlen, elen, clen = struct.unpack_from("<IIHHH", data, pos + 20)
        offset, = struct.unpack_from("<I", data, pos + 42)
        name = data[pos + 46:pos + 46 + nlen].decode("latin1")
        pos += 46 + nlen + elen + clen
        if name.upper().replace("\\", "/") != wanted:
            continue
        lnlen, lelen = struct.unpack_from("<HH", data, offset + 26)
        body = data[offset + 30 + lnlen + lelen:offset + 30 + lnlen + lelen + csize]
        return zlib.decompress(body, -15) if comp == 8 else body


class Sector:
    def __init__(self, ident, low, high, flags, corners):
        self.id = ident
        self.low = low
        self.high = high
        self.flags = flags
        self.corners = corners  # x,y corners of a polygon sector, or None for a box

    def centre(self):
        if self.corners:
            xs = [c[0] for c in self.corners]
            ys = [c[1] for c in self.corners]
            return (sum(xs) / len(xs), sum(ys) / len(ys), (self.low[2] + self.high[2]) / 2)
        return tuple((a + b) / 2 for a, b in zip(self.low, self.high))

    def holds(self, point, slack=0.0):
        x, y = point[0], point[1]
        if not (self.low[0] - slack <= x <= self.high[0] + slack and self.low[1] - slack <= y <= self.high[1] + slack):
            return False
        if not self.corners:
            return True
        inside = False
        count = len(self.corners)
        for i in range(count):
            ax, ay = self.corners[i]
            bx, by = self.corners[(i + 1) % count]
            if (ay > y) != (by > y) and x < (bx - ax) * (y - ay) / (by - ay) + ax:
                inside = not inside
        return inside

    def height_gap(self, z):
        """How far z is outside the sector's height range."""
        return max(self.low[2] - z, 0.0, z - self.high[2])


class Portal:
    def __init__(self, kind, position, sectors, landing=None):
        self.kind = kind
        self.position = position
        self.sectors = sectors
        self.landing = landing  # jump portals: where the jump ends


def parse(data):
    if data[:4] != b"FISH":
        raise SystemExit("not a FISH file")
    sectors = {}
    portals = []
    pos = 8
    end = len(data)
    while pos < end:
        if data[pos] != 0x7E:
            raise SystemExit(f"no chunk at {pos:#x}")
        name_end = data.index(b"\0", pos)
        name = data[pos + 1:name_end].decode("latin1")
        size, = struct.unpack_from("<I", data, name_end + 2)
        body = name_end + 6
        if name in ("taPathfindSectors", "taPathfindPortals", "taPathfindEventListeners"):
            pos = body + 4  # a count, then the chunks themselves
            continue
        words = (size - 4) // 4
        if name == "Sector":
            ident, = struct.unpack_from("<I", data, body)
            box = struct.unpack_from("<6f", data, body + 4)
            flags, = struct.unpack_from("<I", data, body + 28)
            corners = None
            if flags & 0x10:
                count, = struct.unpack_from("<I", data, body + 40)
                flat = struct.unpack_from(f"<{2 * count}f", data, body + 44)
                corners = [(flat[2 * i], flat[2 * i + 1]) for i in range(count)]
            sectors[ident] = Sector(ident, box[0:3], box[3:6], flags, corners)
        elif name == "Portal":
            x, y, z, _width, _axis, a, b = struct.unpack_from("<4f3I", data, body)
            portals.append(Portal("walk", (x, y, z), (a, b)))
        elif name == "Portal_NonAligned":
            values = struct.unpack_from("<8f3I", data, body)
            portals.append(Portal("walk", values[0:3], (values[9], values[10])))
        elif name == "JumpPortal":
            points = struct.unpack_from("<18f", data, body)
            a, b = struct.unpack_from("<2I", data, body + 76)
            first = [sum(points[i * 3 + k] for i in range(3)) / 3 for k in range(3)]
            last = [sum(points[i * 3 + k] for i in range(3, 6)) / 3 for k in range(3)]
            portals.append(Portal("jump", tuple(first), (a, b), tuple(last)))
        else:
            raise SystemExit(f"unknown chunk ~{name} ({words} words) at {pos:#x}")
        pos = body + size - 4
    return sectors, portals


def sector_at(sectors, point):
    """The sector whose outline holds the point, nearest in height; else the nearest one."""
    inside = [s for s in sectors.values() if s.holds(point)]
    if inside:
        return min(inside, key=lambda s: s.height_gap(point[2]))
    return min(sectors.values(), key=lambda s: math.dist(s.centre(), point))


def plan(sectors, portals, start, goal):
    first = sector_at(sectors, start)
    last = sector_at(sectors, goal)
    links = {}
    for index, portal in enumerate(portals):
        a, b = portal.sectors
        links.setdefault(a, []).append((index, b))
        links.setdefault(b, []).append((index, a))
    # Dijkstra over (sector, the point it was entered at).
    queue = [(0.0, first.id, start, [])]
    seen = {}
    while queue:
        cost, sector, point, path = heapq.heappop(queue)
        if sector == last.id:
            return first, last, path
        if seen.get(sector, math.inf) <= cost:
            continue
        seen[sector] = cost
        for index, other in links.get(sector, []):
            portal = portals[index]
            step = math.dist(point, portal.position)
            arrive = portal.position
            if portal.kind == "jump":
                # Entered from either side: land on the far one.
                near, far = portal.position, portal.landing
                if math.dist(point, far) < math.dist(point, near):
                    near, far = far, near
                step = math.dist(point, near) + math.dist(near, far) + 20.0  # a jump is worth a detour
                arrive = far
                heapq.heappush(queue, (cost + step, other, arrive, path + [("jump", near), ("land", far)]))
            else:
                heapq.heappush(queue, (cost + step, other, arrive, path + [("walk", arrive)]))
    return first, last, None


def point(text):
    values = [float(v) for v in text.split(",")]
    if len(values) == 2:
        values.append(0.0)
    return tuple(values)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("archive", help="the game's TFA.ZIP")
    parser.add_argument("level", help="folder of the level under LEVELS/EPISODES, e.g. 01_AMAZON_1")
    parser.add_argument("--from", dest="start", type=point)
    parser.add_argument("--to", dest="goal", type=point)
    parser.add_argument("--at", type=point, help="report the sector at this place")
    parser.add_argument("--info", action="store_true")
    parser.add_argument("--radius", type=float, default=4.0, help="how close the walk has to come to each place")
    parser.add_argument("--tick", type=int, default=0, help="tick to start the walk at (for -Goto)")
    args = parser.parse_args()

    sectors, portals = parse(read_from_zip(args.archive, f"LEVELS/EPISODES/{args.level}/PATHFINDER.VBIN"))
    if args.info:
        jumps = sum(1 for p in portals if p.kind == "jump")
        boxes = sum(1 for s in sectors.values() if not s.corners)
        print(f"{len(sectors)} sectors ({boxes} boxes), {len(portals)} portals ({jumps} jump portals)")
        lows = [min(s.low[k] for s in sectors.values()) for k in range(3)]
        highs = [max(s.high[k] for s in sectors.values()) for k in range(3)]
        print("covering x %.0f..%.0f, y %.0f..%.0f, z %.0f..%.0f" % (lows[0], highs[0], lows[1], highs[1], lows[2], highs[2]))
    if args.at:
        s = sector_at(sectors, args.at)
        print(f"sector {s.id}: x {s.low[0]:.1f}..{s.high[0]:.1f}, y {s.low[1]:.1f}..{s.high[1]:.1f}, "
              f"z {s.low[2]:.1f}..{s.high[2]:.1f}, flags {s.flags:#x}" + (f", {len(s.corners)} corners" if s.corners else ""))
    if args.start and args.goal:
        first, last, path = plan(sectors, portals, args.start, args.goal)
        print(f"from sector {first.id} to sector {last.id}")
        if path is None:
            print("no chain of portals joins them")
            return
        places = []
        length = 0.0
        here = args.start
        for kind, where in path + [("walk", args.goal)]:
            length += math.dist(here, where)
            here = where
            print(f"  {kind:5s} {where[0]:8.1f} {where[1]:8.1f} {where[2]:7.1f}")
            places.append(f"{args.tick}:{where[0]:.1f},{where[1]:.1f},{args.radius:g}")
        print(f"{len(places)} places, {length:.0f} units")
        print("-Goto \"" + ";".join(places) + "\"")


if __name__ == "__main__":
    main()
