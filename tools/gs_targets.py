"""Summarizes a PS2X_GS_TARGET_LOG: one line per change of frame buffer, mask, Z buffer or
display pages, with the number of draws and how many of them were textured (debugging aid).

    python tools/gs_targets.py runs/target2.err
"""
import re
import sys

PATTERN = re.compile(
    r"tick=(\d+) after (\d+) draws: frame fbp=(\d+) fbw=(\d+) psm=(\w+) msk=(\w+) \| zbp=(\d+) zpsm=(\w+) "
    r"zmask=(\d) zte/ztst=(\d) \| disp1=(\d+) disp2=(\d+) \| tex=(\d) tbp=(\d+)"
    r"(?: \| scissor (\d+)-(\d+),(\d+)-(\d+) ofs (\d+),(\d+) scanmsk=(\d))?")


def main():
    last = None
    draws = textured = 0
    for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
        m = PATTERN.search(line)
        if not m:
            continue
        tick, count, fbp, fbw, psm, msk, zbp, zpsm, zmask, ztst, d1, d2, tex, tbp = m.groups()[:14]
        x0, x1, y0, y1, ofx, ofy, scanmsk = m.groups()[14:]
        # The count belongs to the state that was current before this line.
        draws += int(count)
        key = (fbp, fbw, psm, msk, zbp, zpsm, zmask, ztst, d1, d2, x0, x1, y0, y1, ofx, ofy, scanmsk)
        if key != last:
            if last is not None:
                print(f"    {draws} draws")
            area = ""
            if x0 is not None:
                # XYOFFSET is in 1/16 pixels; a draw at primitive-space 2048 lands at 2048 - offset.
                area = (f" | scissor {x0}-{x1},{y0}-{y1} offset {int(ofx) / 16:g},{int(ofy) / 16:g}"
                        f"{' scanmsk=' + scanmsk if scanmsk != '0' else ''}")
            print(f"tick {tick}: frame fbp={fbp} fbw={fbw} psm={psm} msk={msk} | z bp={zbp} psm={zpsm} "
                  f"mask={zmask} test={ztst} | display {d1}/{d2}{area}")
            last = key
            draws = 0
    if last is not None:
        print(f"    {draws}+ draws")


if __name__ == "__main__":
    main()
