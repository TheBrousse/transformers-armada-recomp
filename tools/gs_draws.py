"""Groups a PS2X_GS_DRAW_LOG by draw state: one block per run of primitives that share
their state, with the first and last primitive's vertices (debugging aid).

    python tools/gs_draws.py runs/draws.err [--max 40]
"""
import argparse
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log")
    parser.add_argument("--max", type=int, default=40, help="blocks to print")
    args = parser.parse_args()

    lines = [line.rstrip("\n") for line in open(args.log, encoding="utf-8", errors="replace")
             if line.startswith("[gs:draw]")]
    groups = []
    index = 0
    while index < len(lines):
        if " tick=" not in lines[index]:
            index += 1
            continue
        state = re.sub(r"tick=\d+ ", "", lines[index])[len("[gs:draw] "):]
        vertices = []
        index += 1
        while index < len(lines) and " tick=" not in lines[index]:
            vertices.append(lines[index][len("[gs:draw]"):])
            index += 1
        if groups and groups[-1]["state"] == state:
            groups[-1]["count"] += 1
            groups[-1]["last"] = vertices
        else:
            groups.append({"state": state, "count": 1, "first": vertices, "last": vertices})

    for group in groups[: args.max]:
        print(f"x{group['count']}: {group['state']}")
        for vertex in group["first"]:
            print("    first" + vertex)
        if group["count"] > 1:
            for vertex in group["last"]:
                print("    last " + vertex)
    if len(groups) > args.max:
        print(f"... {len(groups) - args.max} more blocks")


if __name__ == "__main__":
    main()
