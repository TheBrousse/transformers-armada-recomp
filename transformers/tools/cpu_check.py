"""The processor check before the build (cpucheck): the game needs SSE4.1 (2008 or newer).

The runtime uses SSE4.1 instructions unconditionally (the compilers take its intrinsics without a flag), so on a
processor without it the built game would end at its first such instruction with no message. The game checks at
its own start too (transformers/src/cpu_check.cpp); this says it before the long build.

Reads numpy's own table of the processor's features (numpy is one of the setup's requirements: it tests them with
cpuid itself), else Linux's /proc/cpuinfo. Exit 3 with one plain line when SSE4.1 is known to be missing; 0 when it is
there, on a processor that is not x86-64, or when it cannot be told (the game's own check then still stands).
"""
import platform
import sys

MESSAGE = "This game needs a processor with SSE4.1 (2008 or newer)."


def has_sse41():
    if platform.machine().lower() not in ("amd64", "x86_64", "x64"):
        return None
    try:
        import numpy  # noqa: F401
        try:
            from numpy._core._multiarray_umath import __cpu_features__ as features
        except ImportError:
            from numpy.core._multiarray_umath import __cpu_features__ as features
        if "SSE41" in features:
            return bool(features["SSE41"])
    except Exception:
        pass
    try:
        with open("/proc/cpuinfo", encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("flags"):
                    return "sse4_1" in line.split()
    except OSError:
        pass
    return None


if __name__ == "__main__":
    if has_sse41() is False:
        print(MESSAGE)
        sys.exit(3)
    sys.exit(0)
