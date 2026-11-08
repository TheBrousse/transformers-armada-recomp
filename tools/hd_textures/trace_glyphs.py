"""Vector outlines of bitmap glyphs (TODO 119): the game's own letters traced, for fonts with no free twin.

A glyph's shape is a field over its texels (its opacity, or its brightness on the opaque atlases): the
0.5 level of that field, read between texel centres with linear interpolation (marching squares, so the
anti-aliased texels place the edge to a fraction of a texel), is the outline. The outline's texel
staircase is straightened (Douglas-Peucker within under half a texel), its corners - where it turns
sharply - kept sharp, and the stretches between them rounded off (corner-cutting) - as a font's curves
are. The outlines are then filled (even-odd: holes) at any size, anti-aliased by supersampling.

    contours = trace(field, level=0.5)          # list of (N, 2) float arrays, (x, y) in texels
    cov = render(contours, w, h, scale, ss)     # coverage (h*scale, w*scale), float 0..1

Pure numpy + Pillow. Coordinates: texel (x, y) has its centre at (x + 0.5, y + 0.5).
"""

import math

import numpy as np
from PIL import Image, ImageDraw


# --------------------------------------------------------------------------------------------
# Marching squares on the texel-centre grid.

def trace(field, level=0.5):
    """Closed contours of `field` at `level` (the field padded with 0 around: every contour closes)."""
    f = np.pad(np.asarray(field, dtype=np.float64), 1, constant_values=0.0)
    h, w = f.shape
    inside = f >= level

    def point(edge):
        kind, i, j = edge
        if kind == "h":  # between (i, j) and (i, j + 1)
            a, b = f[i, j], f[i, j + 1]
            t = (level - a) / (b - a) if b != a else 0.5
            return (j + t - 0.5, i - 0.5)  # (minus the padding's texel, plus the centre's half)
        a, b = f[i, j], f[i + 1, j]
        t = (level - a) / (b - a) if b != a else 0.5
        return (j - 0.5, i + t - 0.5)

    # Each cell (i, j)..(i + 1, j + 1): the edges it crosses, paired.
    links = {}

    def link(e0, e1):
        links.setdefault(e0, []).append(e1)
        links.setdefault(e1, []).append(e0)

    for i in range(h - 1):
        row0, row1 = inside[i], inside[i + 1]
        for j in range(w - 1):
            c = (1 if row0[j] else 0) | (2 if row0[j + 1] else 0) | (4 if row1[j + 1] else 0) | (8 if row1[j] else 0)
            if c == 0 or c == 15:
                continue
            top, right, bottom, left = ("h", i, j), ("v", i, j + 1), ("h", i + 1, j), ("v", i, j)
            if c in (1, 14):
                link(left, top)
            elif c in (2, 13):
                link(top, right)
            elif c in (4, 11):
                link(right, bottom)
            elif c in (8, 7):
                link(bottom, left)
            elif c in (3, 12):
                link(left, right)
            elif c in (6, 9):
                link(top, bottom)
            else:  # (5, 10: a saddle - the centre's mean decides which corners join)
                centre = (f[i, j] + f[i, j + 1] + f[i + 1, j] + f[i + 1, j + 1]) / 4.0 >= level
                if (c == 5) == centre:
                    link(left, bottom)
                    link(top, right)
                else:
                    link(left, top)
                    link(right, bottom)
    contours = []
    seen = set()
    for start in links:
        if start in seen:
            continue
        loop = [start]
        seen.add(start)
        prev, cur = None, start
        while True:
            nxt = [e for e in links[cur] if e != prev and e not in seen]
            if not nxt:
                break
            prev, cur = cur, nxt[0]
            seen.add(cur)
            loop.append(cur)
        if len(loop) >= 3:
            contours.append(np.array([point(e) for e in loop], dtype=np.float64))
    return contours


# --------------------------------------------------------------------------------------------
# The staircase straightened, corners kept, curves rounded.

def _dp(pts, tol):
    """Douglas-Peucker on an open polyline (its ends kept)."""
    if len(pts) <= 2:
        return pts
    a, b = pts[0], pts[-1]
    d = b - a
    n = math.hypot(d[0], d[1])
    if n < 1e-12:
        dist = np.hypot(pts[:, 0] - a[0], pts[:, 1] - a[1])
    else:
        dist = np.abs(d[0] * (pts[:, 1] - a[1]) - d[1] * (pts[:, 0] - a[0])) / n
    k = int(np.argmax(dist))
    if dist[k] <= tol:
        return np.array([a, b])
    left = _dp(pts[:k + 1], tol)
    right = _dp(pts[k:], tol)
    return np.vstack([left[:-1], right])


def simplify(contour, tol=0.42):
    """The closed contour with its texel staircase taken out: split at its two farthest points, each half
    simplified, so no start point is privileged."""
    p = contour
    if len(p) < 4:
        return p
    c = p.mean(axis=0)
    i0 = int(np.argmax(np.hypot(p[:, 0] - c[0], p[:, 1] - c[1])))
    p = np.roll(p, -i0, axis=0)
    i1 = int(np.argmax(np.hypot(p[:, 0] - p[0, 0], p[:, 1] - p[0, 1])))
    a = _dp(p[:i1 + 1], tol)
    b = _dp(np.vstack([p[i1:], p[:1]]), tol)
    return np.vstack([a[:-1], b[:-1]])


def snap_axes(p, max_deg=12.0):
    """Edges within max_deg of horizontal or vertical made exactly so (both ends on their mean): a font's
    straight stems and bars, which the anti-aliased texels leave a little askew."""
    p = p.copy()
    n = len(p)
    for k in range(n):
        a, b = p[k], p[(k + 1) % n]
        dx, dy = b[0] - a[0], b[1] - a[1]
        if abs(dx) < 1e-9 and abs(dy) < 1e-9:
            continue
        ang = abs(math.degrees(math.atan2(dy, dx))) % 180.0
        if min(ang, 180.0 - ang) <= max_deg:
            m = (a[1] + b[1]) / 2.0
            p[k, 1] = p[(k + 1) % n, 1] = m
        elif abs(ang - 90.0) <= max_deg:
            m = (a[0] + b[0]) / 2.0
            p[k, 0] = p[(k + 1) % n, 0] = m
    return p


def turning(p):
    """Each vertex's turn (degrees, 0 = straight on)."""
    prev = p - np.roll(p, 1, axis=0)
    nxt = np.roll(p, -1, axis=0) - p
    a0 = np.arctan2(prev[:, 1], prev[:, 0])
    a1 = np.arctan2(nxt[:, 1], nxt[:, 0])
    d = np.degrees((a1 - a0 + np.pi) % (2 * np.pi) - np.pi)
    return np.abs(d)


def smooth(p, corner_deg=40.0, iterations=2):
    """Corner-cutting (Chaikin) everywhere but at the corners (a turn of corner_deg or more): curves come out
    round, angular glyphs keep their sharp corners."""
    for _ in range(iterations):
        n = len(p)
        if n < 3:
            return p
        corner = turning(p) >= corner_deg
        out = []
        for k in range(n):
            a, b = p[k], p[(k + 1) % n]
            if corner[k]:
                out.append(a)
            # the segment's quarter points, but none beside a corner (it stays where it is)
            q = 0.75 * a + 0.25 * b
            r = 0.25 * a + 0.75 * b
            if not corner[k]:
                out.append(q)
            if not corner[(k + 1) % n]:
                out.append(r)
        p = np.array(out)
    return p


def outline(field, level=0.5, tol=0.42, corner_deg=40.0):
    """The glyph's vector outline: traced, straightened, its curves rounded."""
    out = []
    for c in trace(field, level):
        s = simplify(c, tol)
        if len(s) >= 3:
            out.append(smooth(snap_axes(s), corner_deg))
    return out


# --------------------------------------------------------------------------------------------

def render(contours, w, h, scale, ss=4, origin=(0.0, 0.0)):
    """Coverage of the contours (even-odd) over a w x h texel area at `scale`, supersampled `ss` times.
    `origin`: the area's top-left texel in the contours' coordinates."""
    W, H = w * scale * ss, h * scale * ss
    acc = np.zeros((H, W), dtype=bool)
    k = scale * ss
    for c in contours:
        img = Image.new("1", (W, H), 0)
        pts = [((x - origin[0]) * k, (y - origin[1]) * k) for x, y in c]
        ImageDraw.Draw(img).polygon(pts, fill=1)
        acc ^= np.asarray(img, dtype=bool)
    return acc.reshape(h * scale, ss, w * scale, ss).mean(axis=(1, 3)).astype(np.float32)
