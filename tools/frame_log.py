"""Summarises frame logs written with TF_FRAME_LOG=<file> (one line per frame: VBlank tick,
host milliseconds since the last frame, the part of them the EE thread was busy, camera
position).

In turbo mode the time is what a frame cost the host. The game draws a frame every two
VBlanks, so a frame above 33.3 ms cannot keep up in real time: it stays up for three VBlanks
(a hitch) and, when such frames come in a row, the game runs in slow motion.

    python tools\\frame_log.py runs\\frames_drive.csv [more.csv ...] [--skip 60] [--worst 10]
"""
import argparse
import csv
import statistics


def load(path):
    """Rows of (tick, ms, EE busy ms or None, x, y, z)."""
    rows = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            try:
                busy = float(row["ee_ms"]) if row.get("ee_ms") not in (None, "") else None
                rows.append((int(row["tick"]), float(row["ms"]), busy, float(row["x"]), float(row["y"]), float(row["z"])))
            except (KeyError, ValueError):
                continue
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("logs", nargs="+")
    parser.add_argument("--skip", type=int, default=60, help="frames to leave out at the start (state load)")
    parser.add_argument("--worst", type=int, default=8, help="how many of the slowest frames to list")
    parser.add_argument("--budget", type=float, default=33.3, help="ms a frame may take (two VBlanks)")
    args = parser.parse_args()
    for path in args.logs:
        rows = load(path)[args.skip:]
        if not rows:
            print(f"{path}: no frames")
            continue
        times = sorted(ms for _, ms, *_ in rows)

        def pct(p):
            return times[min(len(times) - 1, int(p / 100 * len(times)))]

        over = [r for r in rows if r[1] > args.budget]
        busy = [r[2] for r in rows if r[2] is not None]
        print(f"{path}: {len(rows)} frames, median {statistics.median(times):.1f} ms, p90 {pct(90):.1f}, "
              f"p99 {pct(99):.1f}, max {times[-1]:.1f}; over {args.budget} ms: {len(over)} "
              f"({100 * len(over) / len(rows):.1f}%)"
              + (f"; EE busy {statistics.median(busy):.1f} ms per frame (median)" if busy else ""))
        for tick, ms, ee, x, y, z in sorted(rows, key=lambda r: -r[1])[:args.worst]:
            ee_text = f" (EE busy {ee:.1f})" if ee is not None else ""
            print(f"    tick {tick}: {ms:.1f} ms{ee_text} at ({x:.0f}, {y:.0f}, {z:.0f})")


if __name__ == "__main__":
    main()
