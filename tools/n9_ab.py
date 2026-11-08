# n9_ab.py - N9 stage 2 (docs/TODO.md): an A/B within one run (TF_POSES_ALT=<s>: pictures between frames on and off by
# turns; or stage 3's TF_POSES_REPLAY=alt<s>: a frame's pictures from one recording or each recorded again) - the level's pacing lines (after its last rate change) grouped by on / off: pictures a second, the time
# between them, its spread, the longest 1%, the delay to the screen, the game's frames a second.
# usage: python tools\n9_ab.py <run> [<run> ...]   (runs\<run>.err)
import os
import re
import sys
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'runs') + os.sep
for run in sys.argv[1:]:
    lines = open(ROOT + run + '.err', errors='replace').read().splitlines()
    starts = [i for i, l in enumerate(lines) if 'views made to draw' in l]
    start = starts[-1] if starts else 0
    # the state at the start: the last switch line before it
    state = None
    for l in lines[:start]:
        m = re.search(r"(?:pictures between frames|one recording for a frame's pictures) (on|off)", l)
        if m:
            state = m.group(1) == 'on'
    rows = {True: [], False: []}
    fps = {True: [], False: []}
    skip = 1
    for line in lines[start:]:
        m = re.search(r"(?:pictures between frames|one recording for a frame's pictures) (on|off)", line)
        if m:
            state = m.group(1) == 'on'
            skip = 1
            continue
        if state is None:
            continue
        m = re.search(r'\[tf\] pacing: ([\d.]+) new pictures a second.*?between them ([\d.]+) ms on average, spread ([\d.]+) ms, '
                      r'longest 1% ([\d.]+) ms \(1% low ([\d.]+) a second\).*?to the screen ([\d.]+) ms.*?screen (\d+) Hz, variable refresh (\w+)', line)
        if m:
            if skip:
                skip -= 1  # (the window the switch fell in)
                continue
            rows[state].append(tuple(float(m.group(i)) for i in range(1, 7)) + (m.group(7), m.group(8)))
        m = re.search(r'\[tf\] fps ([\d.]+)', line)
        if m:
            fps[state].append(float(m.group(1)))
    rate = next((l for l in reversed(lines[:start + 1]) if re.match(r'\[tf\] \d+ frames a second$', l)), '?')
    print(run, '-', rate)
    for st in (False, True):
        r = rows[st]
        if not r:
            continue
        avg = lambda i: sum(x[i] for x in r) / len(r)
        f = fps[st]
        print(f"  {'on ' if st else 'off'}: {len(r)} windows; game frames {sum(f) / max(1, len(f)):.1f}/s; pictures {avg(0):.1f}/s, "
              f"between {avg(1):.2f} ms, spread {avg(2):.2f} ms (worst window {max(x[2] for x in r):.2f}), longest 1% {avg(3):.2f} ms "
              f"(1% low {avg(4):.1f}/s), to the screen {avg(5):.1f} ms; screen {r[0][6]} Hz, variable refresh {r[0][7]}")
