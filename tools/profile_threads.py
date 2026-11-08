"""Summarizes runs/profile.txt (TF_PROFILE): the busy threads with their hottest functions,
inclusive times and stacks; --busy gives one line per thread instead: how much of the time it
was working (not waiting) and what it runs.

    python tools/profile_threads.py [runs/profile.txt] [--flat 14] [--inclusive 22] [--stacks 10]
    python tools/profile_threads.py runs/profile_bench.txt --busy
"""
import argparse
import re

WAITS = ("ZwWait", "NtWait", "NtDelayExecution", "ZwDelayExecution", "NtRemoveIoCompletion", "ZwRemoveIoCompletion",
         "RtlSleepConditionVariable", "SleepConditionVariable", "NtWaitForAlertByThreadId", "ZwWaitForAlertByThreadId",
         "NtUserMsgWaitForMultipleObjectsEx", "NtYieldExecution", "ZwYieldExecution", "SwitchToThread")

# What a thread is, from the functions it spends its time in (first match wins).
ROLES = (
    ("EE: game code, IOP, scheduler", ("sub_", "EeScheduler", "dispatchGuestBranch", "IopCpuCore", "IopEmulator")),
    ("VIF1 / VU1 / GS front end", ("ps2vif1", "Vif1", "vu1", "Vu1", "VU1", "gs_frontend", "GSFrontend", "GifArbiter")),
    ("GS thread / rasterizer", ("GSThreadedBackend", "Raster", "raster", "Span", "span", "GsCpu", "drawTriangle", "Texture")),
    ("Window, GPU driver", ("gl", "GL", "Present", "raylib", "Swap", "atio6axx", "amdxc", "nvoglv", "ig", "Amd", "Dxgk", "DDDI")),
    ("Sound", ("audio", "Audio", "Spu", "SPU", "mixVoices", "XAudio", "wasapi", "Wasapi")),
)


def busy(text):
    rows = []
    for part in re.split(r"\n(?=thread \d+: )", text)[1:]:
        lines = part.split("\n")
        head = re.match(r"thread (\d+): (\d+) samples", lines[0])
        if not head:
            continue
        flat = []
        for line in lines[1:]:
            if line.strip() == "inclusive:":
                break
            m = re.match(r"\s*([\d.]+)%\s+\d+\s+(.*)", line)
            if m:
                flat.append((float(m.group(1)), m.group(2).strip()))
        waiting = sum(share for share, name in flat if name.startswith(WAITS))
        working = [(share, name) for share, name in flat if not name.startswith(WAITS)]
        role = "?"
        for label, keys in ROLES:
            if any(any(k in name for k in keys) for _, name in working[:12]):
                role = label
                break
        rows.append((100.0 - waiting, head.group(1), role, ", ".join(n.split("(")[0][:40] for _, n in working[:3])))
    rows.sort(reverse=True)
    total = sum(r[0] for r in rows) / 100.0
    print(f"{'busy':>7}  thread  {'what':32}  hottest")
    for share, tid, role, names in rows:
        if share < 0.5:
            continue
        print(f"{share:6.1f}%  {tid:>6}  {role:32}  {names}")
    print(f"total: {total:.2f} cores busy")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("profile", nargs="?", default="runs/profile.txt")
    parser.add_argument("--flat", type=int, default=14)
    parser.add_argument("--inclusive", type=int, default=22)
    parser.add_argument("--stacks", type=int, default=10)
    parser.add_argument("--idle", type=float, default=97.0, help="skip threads whose top entry is a wait above this share")
    parser.add_argument("--busy", action="store_true", help="one line per thread: share of time working, and what it runs")
    args = parser.parse_args()

    text = open(args.profile, encoding="utf-8", errors="replace").read()
    if args.busy:
        busy(text)
        return
    waits = ("ZwWait", "NtWait", "NtDelayExecution", "ZwDelayExecution", "NtRemoveIoCompletion", "ZwRemoveIoCompletion")
    for part in re.split(r"\n(?=thread \d+: )", text)[1:]:
        lines = part.split("\n")
        entries = [line for line in lines[1:] if line.strip()]
        top = re.match(r"\s*([\d.]+)%\s+\d+\s+(.*)", entries[0]) if entries else None
        if top and float(top.group(1)) >= args.idle and top.group(2).startswith(waits):
            continue
        limits = {"flat": args.flat, "inclusive": args.inclusive, "stacks": args.stacks}
        section = "flat"
        seen = dict.fromkeys(limits, 0)
        print(lines[0])
        for line in lines[1:]:
            stripped = line.strip()
            if stripped in ("inclusive:", "stacks:"):
                section = stripped[:-1]
                print("  " + stripped)
                continue
            if not stripped:
                continue
            seen[section] += 1
            if seen[section] <= limits[section]:
                print(line[:200])
        print()


if __name__ == "__main__":
    main()
