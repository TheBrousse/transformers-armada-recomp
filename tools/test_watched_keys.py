"""Compile the actual patched key collector against deterministic input fixtures.

Use an MSVC developer shell, or pass --compiler for another C++17 compiler.
Output stays in the supplied external build directory. No game or window starts.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def collector(source):
    marker = "void collectWatchedKeyPresses()"
    if source.count(marker) != 1:
        raise ValueError("runtime does not contain one patched watched-key collector")
    begin = source.index(marker)
    brace = source.index("{", begin)
    depth, at = 1, brace+1
    while depth and at < len(source):
        if source[at] == "{":
            depth += 1
        elif source[at] == "}":
            depth -= 1
        at += 1
    if depth:
        raise ValueError("unterminated watched-key collector")
    result = source[begin:at]
    if "GetKeyPressed()" not in result or "IsKeyPressed(" in result:
        raise ValueError("collector is not the expected event-queue implementation")
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--runtime-source", type=Path, required=True, help="patched ps2_pad.cpp")
    ap.add_argument("--build-directory", type=Path, required=True)
    ap.add_argument("--compiler", default="cl")
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = args.build_directory.resolve()
    if build == root or root in build.parents:
        raise ValueError("build directory must be outside the source repository")
    raw = args.runtime_source.read_bytes()
    helper = collector(raw.decode("utf-8-sig").replace("\r\n", "\n"))
    compiler = shutil.which(args.compiler)
    if compiler is None:
        raise ValueError("C++ compiler not found; use a developer shell or --compiler")
    build.mkdir(parents=True, exist_ok=True)
    (build/"watched_key_collector.h").write_text(helper+"\n", encoding="utf-8")
    source = Path(__file__).resolve().parents[1]/"tests/watched_key_queue_tests.cpp"  # (beside tools/: works in port/ and in the working copy)
    is_msvc = Path(compiler).name.lower() in ("cl", "cl.exe")
    executable = build/("watched-key-tests.exe" if is_msvc else "watched-key-tests")
    if is_msvc:
        command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/I"+str(build),
                   "/Fe:"+str(executable), "/Fo:"+str(build)+"\\", str(source)]
    else:
        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-pedantic", "-I", str(build),
                   str(source), "-o", str(executable)]
    subprocess.run(command, cwd=build, check=True)
    subprocess.run([str(executable)], cwd=build, check=True)
    report = dict(status="passed", runtime_source_sha256=hashlib.sha256(raw).hexdigest(),
                  collector_sha256=hashlib.sha256(helper.encode()).hexdigest())
    (build/"watched-key-test-results.json").write_text(json.dumps(report, indent=2)+"\n")


if __name__ == "__main__":
    main()
