"""The GPU renderer's shader programs through Radeon GPU Analyzer (RGA, AMD's offline compiler):
what each costs a pixel before running anything.

Builds each program from PS2Recomp/ps2xRuntime/src/lib/gs/gs_gpu_shaders.h the way gs_gpu.cpp
links it, compiles it for an AMD GPU (RGA's OpenGL mode, which needs no driver) and prints, per
shader: the vector and scalar registers it takes, the instructions, the loads from memory and how
often it stops to wait for them, the texture samples, the exports. The ISA and the live-register
analysis of each are kept in the output folder for a closer look.

    python tools/rga_stats.py                      # every program, for gfx1201 (RX 9070 XT)
    python tools/rga_stats.py native native-pack   # some of them
    python tools/rga_stats.py --asic gfx1100 --define GS_COUNT_FRAGMENTS draw

RGA: https://github.com/GPUOpen-Tools/radeon_gpu_analyzer/releases (the Windows zip, extracted);
--rga or the RGA_EXE environment variable say where rga.exe is, else C:\\Tools\\RGA\\rga.exe.
Not the driver's own compiler: its numbers guide, the timing runs decide.
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHADERS = os.path.join(ROOT, "PS2Recomp", "ps2xRuntime", "src", "lib", "gs", "gs_gpu_shaders.h")

# name: (vertex pieces, fragment pieces) as gs_gpu.cpp links them; a piece that starts with '#'
# is a line of its own (a define), else the name of a string in gs_gpu_shaders.h.
NATIVE_VS = ["kVersion", "kDrawStruct", "kNativeVertex"]
DRAW_VS = ["kDrawVertex"]
PROGRAMS = {
    "native": (NATIVE_VS, ["kVersion", "kMemory", "kDrawStruct", "kDrawFragmentBody", "kNativeFragmentMain"]),
    "native-pack": (NATIVE_VS, ["kVersion", "#define GS_PACK_ONLY", "kMemory", "kDrawStruct", "kDrawFragmentBody", "kNativeFragmentMain"]),
    "draw": (DRAW_VS, ["kDrawFragmentHead", "kMemory", "kDrawStruct", "kDrawFragmentBody", "kDrawFragmentMain"]),
    "draw-pack": (DRAW_VS, ["kDrawFragmentHead", "#define GS_PACK_ONLY", "kMemory", "kDrawStruct", "kDrawFragmentBody", "kDrawFragmentMain"]),
    "draw-nodepth": (DRAW_VS, ["kDrawFragmentHead", "#define GS_NO_DEPTH", "kMemory", "kDrawStruct", "kDrawFragmentBody", "kDrawFragmentMain"]),
    "dual": (DRAW_VS, ["kDrawFragmentHead", "#define GS_NO_DEPTH", "#define GS_DUAL_FRAME", "kMemory", "kDrawStruct", "kDrawFragmentBody",
                       "kDrawFragmentMain"]),
}


def pieces():
    text = open(SHADERS, encoding="utf-8").read().replace("\r\n", "\n")
    found = {}
    for m in re.finditer(r'inline const char \*(k\w+) = R"\((.*?)\)";', text, re.S):
        found[m.group(1)] = m.group(2)
    for m in re.finditer(r'inline const char \*(k\w+) = "((?:[^"\\]|\\.)*)";', text):
        found[m.group(1)] = m.group(2).encode().decode("unicode_escape")
    return found


def source(parts, found, defines):
    out = []
    for i, part in enumerate(parts):
        if part.startswith("#"):
            out.append(part + "\n")
            continue
        if part not in found:
            sys.exit(f"no {part} in {SHADERS}")
        out.append(found[part])
        if i == 0:
            # (extra defines after the #version line, which is the first piece's first line)
            out.extend(f"#define {d.replace('=', ' ', 1)}\n" for d in defines)
    return "".join(out)


def isa_counts(path):
    counts = {"instructions": 0, "vector loads": 0, "load waits": 0, "samples": 0, "scalar loads": 0, "branches": 0}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\s+([a-z_0-9]+)\s", line)
        if not m or line.lstrip().startswith(("//", ";")):
            continue
        op = m.group(1)
        if op.startswith("label_"):
            continue
        counts["instructions"] += 1
        if re.match(r"(t?buffer_load|global_load|image_load|scratch_load|flat_load)", op):
            counts["vector loads"] += 1
        elif op.startswith("image_sample") or op.startswith("image_gather"):
            counts["samples"] += 1
        elif op.startswith(("s_load", "s_buffer_load")):
            counts["scalar loads"] += 1
        elif op in ("s_wait_loadcnt", "s_waitcnt_vmcnt", "s_wait_samplecnt") or (op == "s_waitcnt" and "vmcnt" in line):
            counts["load waits"] += 1
        elif op.startswith("s_cbranch"):
            counts["branches"] += 1
    return counts


def stats(path):
    values = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\s*-\s*(\w+)\s*=\s*(\d+)", line)
        if m:
            values[m.group(1)] = int(m.group(2))
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("programs", nargs="*", help="programs (default: all): " + ", ".join(PROGRAMS))
    parser.add_argument("--rga", default=os.environ.get("RGA_EXE", r"C:\Tools\RGA\rga.exe"))
    parser.add_argument("--asic", default="gfx1201")
    parser.add_argument("--define", action="append", default=[], help="NAME or NAME=VALUE, into every shader")
    parser.add_argument("--out", default=os.path.join(ROOT, "runs", "rga"))
    args = parser.parse_args()
    if not os.path.exists(args.rga):
        sys.exit(f"no rga.exe at {args.rga} (--rga or RGA_EXE)")
    names = args.programs or list(PROGRAMS)
    found = pieces()
    os.makedirs(args.out, exist_ok=True)
    print(f"{'program':<18} {'stage':<5} {'VGPRs':>5} {'SGPRs':>5} {'scratch':>7} {'instr':>6} {'loads':>5} {'waits':>5} "
          f"{'samples':>7} {'sloads':>6} {'branches':>8}")
    for name in names:
        if name not in PROGRAMS:
            sys.exit(f"no program {name}: " + ", ".join(PROGRAMS))
        vs_parts, fs_parts = PROGRAMS[name]
        folder = os.path.join(args.out, name)
        os.makedirs(folder, exist_ok=True)
        vert = os.path.join(folder, "shader.vert")
        frag = os.path.join(folder, "shader.frag")
        open(vert, "w", encoding="utf-8").write(source(vs_parts, found, args.define))
        open(frag, "w", encoding="utf-8").write(source(fs_parts, found, args.define))
        cmd = [args.rga, "-s", "opengl", "-c", args.asic, "--isa", os.path.join(folder, "isa.txt"), "--livereg",
               os.path.join(folder, "livereg.txt"), "-a", os.path.join(folder, "stats.csv"), "--vert", vert, "--frag", frag]
        run = subprocess.run(cmd, capture_output=True, text=True)
        if "succeeded" not in run.stdout and "SUCCESS" not in run.stdout:
            print(f"{name}: did not compile\n{run.stdout[-4000:]}\n{run.stderr[-2000:]}")
            continue
        for stage in ("vert", "frag"):
            isa = os.path.join(folder, f"{args.asic}_isa_{stage}.txt")
            st = os.path.join(folder, f"{args.asic}_stats_{stage}.csv")
            if not os.path.exists(isa) or not os.path.exists(st):
                print(f"{name:<18} {stage:<5} (no output)")
                continue
            s = stats(st)
            c = isa_counts(isa)
            print(f"{name:<18} {stage:<5} {s.get('USED_VGPRs', -1):>5} {s.get('USED_SGPRs', -1):>5} {s.get('SCRATCH_SIZE', -1):>7} "
                  f"{c['instructions']:>6} {c['vector loads']:>5} {c['load waits']:>5} {c['samples']:>7} {c['scalar loads']:>6} "
                  f"{c['branches']:>8}")
    print(f"(ISA, live registers and statistics in {args.out})")


if __name__ == "__main__":
    main()
