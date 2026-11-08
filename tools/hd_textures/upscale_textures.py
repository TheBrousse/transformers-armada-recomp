#!/usr/bin/env python3
"""Batch AI-upscale converted PS2 textures (ncnn-Vulkan or ONNX/DirectML).

Purpose: Phase-2 pipeline driver. Walks a tree of RGBA PNGs (output of
itf_to_png.py), classifies each into a tier (tier_rules.py), dedups
byte-identical files by SHA-1, drives the chosen backend in chained 4x
passes, then finalizes to exact per-tier target size (Lanczos
supersample-downscale when overshooting) into --out, mirroring source
relative paths 1:1. Resumable via a JSONL manifest stored with the outputs;
--dry-run plans without touching the GPU; --audit verifies a finished run.

Backends (--backend, default = the model's own backend, else ncnn):
  ncnn      realesrgan-ncnn-vulkan.exe, batch directory mode (AMD via Vulkan).
  onnx-dml  onnxruntime InferenceSession(providers=[DmlExecutionProvider,
            CPUExecutionProvider]) — runs modern CNN SR (SPAN/Compact/
            RealPLKSR) and PS2-era ESRGAN (HDcube/RealisticRescaler) on the
            AMD GPU with NO CUDA. Import-guarded: onnxruntime + the model
            file are the USER's install step (pip install onnxruntime-directml
            + drop .onnx under ThirdParty/onnx_models); absent -> a clear
            message + non-zero exit, never a traceback, and the ncnn path is
            unaffected.
See --list-models for the registry; --model NAME overrides tier routing.

Input : Extracted/converted/textures (PNG tree)
Output: Extracted/upscaled/... (mirrored PNG tree) + upscale_manifest.jsonl
Deps  : Python 3.8+, Pillow; ncnn backend -> realesrgan-ncnn-vulkan.exe;
        onnx-dml backend -> onnxruntime-directml + numpy (both optional,
        import-guarded). None needed for pil-* tiers, --dry-run, --audit,
        --list-models.
"""

import argparse
import hashlib
import io
import json
import math
import os
import random
import shutil
import subprocess
import sys
import time
from collections import Counter, defaultdict
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tier_rules

PROJECT_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SRC = PROJECT_ROOT / "Extracted" / "converted" / "textures"
DEFAULT_OUT = PROJECT_ROOT / "Extracted" / "upscaled"
DEFAULT_WORK = PROJECT_ROOT / "Extracted" / "work" / "upscale"
DEFAULT_EXE = PROJECT_ROOT / "ThirdParty" / "realesrgan" / "realesrgan-ncnn-vulkan.exe"
MANIFEST_NAME = "upscale_manifest.jsonl"
SIZE_BUCKETS = (32, 64, 128, 256, 512, 1024)
# Reference counts from the 2026-07-02 source scan; drift = warn, not fail.
REF_ALPHA = {"opaque": 1410, "binary": 168, "soft": 215}

# User-supplied ONNX models live here (git-ignored, like the rest of ThirdParty).
ONNX_MODEL_DIR = PROJECT_ROOT / "ThirdParty" / "onnx_models"

# Model registry (research MODERNIZATION_RESEARCH.md §1.1-1.2). name -> metadata:
#   arch           network family (documentation only).
#   scale          per-pass upscale factor (all wired models are 4x; the pass
#                  chaining math in plan_jobs assumes 4x).
#   backend        native backend: ncnn (built into ThirdParty/realesrgan) |
#                  onnx-dml (user .onnx under ThirdParty/onnx_models) | pil.
#   class_affinity which texture class it's best on (routing guidance).
#   file           onnx filename under --onnx-model-dir (None = ncnn/pil built-in).
#   note           where to obtain / how to supply it.
# The ncnn/pil models keep the current behavior. The onnx-dml models are
# USER-SUPPLIED (convert a .pth from OpenModelDB/Phhofm/Venomalia to .onnx, or
# grab a prebuilt .onnx) — the code degrades gracefully when they are absent.
MODELS = {
    "realesrgan-x4plus": dict(
        arch="RRDBNet", scale=4, backend="ncnn",
        class_affinity="general / photographic (current default)",
        file=None, note="ncnn built-in (ThirdParty/realesrgan model dir)"),
    "realesr-animevideov3": dict(
        arch="SRVGG", scale=4, backend="ncnn",
        class_affinity="tiny <=32px / soft sources (tier small_model)",
        file=None, note="ncnn built-in"),
    "HDcube4": dict(
        arch="ESRGAN", scale=4, backend="onnx-dml",
        class_affinity="BC1/DXT1 GC/Wii/PS2-era compression artifacts (bulk)",
        file="HDcube4.onnx",
        note="USER-SUPPLIED: Venomalia/HDcube .pth->.onnx into ThirdParty/onnx_models/"),
    "4xRealisticRescaler": dict(
        arch="ESRGAN", scale=4, backend="onnx-dml",
        class_affinity="compressed GameCube/PS2-era BC1/JPEG textures",
        file="4xRealisticRescaler.onnx",
        note="USER-SUPPLIED: OpenModelDB 4x-RealisticRescaler .onnx"),
    "4xNomosUni-SPAN": dict(
        arch="SPAN", scale=4, backend="onnx-dml",
        class_affinity="env diffuse, fast bulk (~21fps)",
        file="4xNomosUni_span.onnx",
        note="USER-SUPPLIED: Phhofm 4xNomosUni SPAN .onnx"),
    "4xCompact": dict(
        arch="Compact/SRVGG", scale=4, backend="onnx-dml",
        class_affinity="fastest bulk CNN (~26fps)",
        file="4xCompact.onnx",
        note="USER-SUPPLIED: a Compact/SRVGG .onnx (e.g. 4xNomos8kSCCompact)"),
    "pil-lanczos": dict(
        arch="lanczos", scale=4, backend="pil",
        class_affinity="tiny geometric UI (never hallucinate)",
        file=None, note="Pillow resample, no GPU"),
    "pil-nearest": dict(
        arch="nearest", scale=4, backend="pil",
        class_affinity="pixel-exact", file=None, note="Pillow resample, no GPU"),
}

ONNX_GUIDANCE = (
    "error: --backend onnx-dml requested but onnxruntime is not installed.\n"
    "  Install (AMD/DirectML, no CUDA):  pip install onnxruntime-directml\n"
    "  Then drop user-supplied .onnx models under ThirdParty/onnx_models/\n"
    "  (see --list-models). The default --backend ncnn path needs none of this."
)


def resolve_backend(model, requested_backend):
    """Effective backend for a model. pil-* is always pil. An explicit
    --backend forces the choice (e.g. HDcube ships both ncnn + onnx forms);
    otherwise the registry's native backend, defaulting to ncnn for unknown
    names so the existing --model-override pilot use is unchanged."""
    if model.startswith("pil-"):
        return "pil"
    if requested_backend:
        return requested_backend
    m = MODELS.get(model)
    return m["backend"] if m else "ncnn"


def onnx_model_path(model, model_dir):
    """Path to the .onnx file for a model (None if the model has no onnx build)."""
    m = MODELS.get(model)
    fname = m["file"] if m else f"{model}.onnx"
    return Path(model_dir) / fname if fname else None


def import_onnxruntime():
    """Import-guard: return the onnxruntime module, or None if unavailable
    (not installed, or a DLL/load failure) so callers can degrade cleanly."""
    try:
        import onnxruntime as ort
        return ort
    except Exception:  # noqa: BLE001 - any import/DLL failure = "not available"
        return None


def list_models():
    print("Upscale model registry  (--model NAME [--backend {ncnn,onnx-dml}])")
    print(f"onnx model dir: {ONNX_MODEL_DIR}\n")
    for name, m in MODELS.items():
        loc = "built-in" if not m["file"] else f"onnx_models/{m['file']}"
        print(f"  {name:22s} {m['backend']:8s} {m['arch']:14s} x{m['scale']}  "
              f"{m['class_affinity']}")
        print(f"      {loc}  -- {m['note']}")


@dataclass
class Src:
    relpath: str  # posix-style, relative to the source root
    path: Path
    sha1: str
    w: int
    h: int
    alpha: str  # opaque | binary | soft
    tier: str


@dataclass
class Job:
    sha1: str
    srcs: list  # all byte-identical Srcs (>=1)
    tier: str
    model: str
    passes: int  # chained 4x passes (0 for pil-* pseudo-models)
    out_w: int
    out_h: int
    alpha: str  # class of the (identical) sources
    alpha_mode: str  # rgb | direct | split | pil
    backend: str = "ncnn"  # ncnn | onnx-dml | pil

    @property
    def relpaths(self):
        return sorted(s.relpath for s in self.srcs)


def alpha_class(img):
    a = img.getchannel("A")
    lo, _hi = a.getextrema()
    if lo == 255:
        return "opaque"
    if sum(a.histogram()[1:255]) == 0:
        return "binary"
    return "soft"


def size_bucket(long_edge):
    for b in SIZE_BUCKETS:
        if long_edge <= b:
            return b
    return SIZE_BUCKETS[-1]


def scan_sources(root, errors):
    srcs = []
    for p in sorted(root.rglob("*.png")):
        rel = p.relative_to(root).as_posix()
        try:
            sha1 = hashlib.sha1(p.read_bytes()).hexdigest()
            with Image.open(p) as im:
                if im.mode != "RGBA":
                    im = im.convert("RGBA")
                w, h = im.size
                ac = alpha_class(im)
            srcs.append(Src(rel, p, sha1, w, h, ac, tier_rules.tier_for(rel)))
        except Exception as e:  # noqa: BLE001 - per-file capture, batch continues
            errors.append((rel, f"scan: {e}"))
    return srcs


def sample_srcs(srcs, n, seed):
    """Seeded per-tier sample, stratified across (size bucket, alpha class)
    so pilots see every content type, not just the dominant 128px-opaque."""
    rnd = random.Random(seed)
    per_tier = defaultdict(lambda: defaultdict(list))
    for s in srcs:
        per_tier[s.tier][(size_bucket(max(s.w, s.h)), s.alpha)].append(s)
    keep = []
    for _tier, strata in sorted(per_tier.items()):
        for lst in strata.values():
            lst.sort(key=lambda s: s.relpath)
            rnd.shuffle(lst)
        order = sorted(strata.keys())
        picked = []
        while len(picked) < n and any(strata[k] for k in order):
            for k in order:
                if strata[k] and len(picked) < n:
                    picked.append(strata[k].pop())
        keep.extend(picked)
    return keep


def plan_jobs(srcs, model_override, alpha_mode, notes, requested_backend=None):
    by_sha = defaultdict(list)
    for s in srcs:
        by_sha[s.sha1].append(s)
    jobs = []
    coerced = False
    for sha1, group in sorted(by_sha.items()):
        tiers = {s.tier for s in group}
        tier = min(tiers, key=lambda t: tier_rules.TIER_RANK[t])
        if len(tiers) > 1:
            notes.append(f"dup spans tiers {sorted(tiers)} -> {tier}: "
                         f"{group[0].relpath} (+{len(group) - 1})")
        cfg = tier_rules.TIERS[tier]
        w, h = group[0].w, group[0].h
        long_edge = max(w, h)
        model = cfg["model"]
        if long_edge <= cfg.get("small_max", 0):
            model = cfg["small_model"]
        model = model_override or model
        backend = resolve_backend(model, requested_backend)
        target = cfg["target_long"]
        if backend == "pil":
            passes = 0
            final_long = min(target, long_edge * 4 ** cfg["passes_cap"])
        else:
            needed = target / long_edge
            passes = 1 if needed <= 1 else min(
                cfg["passes_cap"], max(1, math.ceil(math.log(needed, 4))))
            final_long = min(target, long_edge * 4 ** passes)
        final_long = max(final_long, long_edge)  # never below source size
        f = final_long / long_edge
        out_w = max(1, round(w * f))
        out_h = max(1, round(h * f))
        if backend == "pil":
            amode = "pil"
        elif group[0].alpha == "opaque":
            amode = "rgb"
        elif backend == "onnx-dml" and alpha_mode == "direct":
            # A generic RGB onnx model can't carry a 4th channel through, so
            # non-opaque textures upscale RGB + alpha separately (research §1.3).
            amode = "split"
            coerced = True
        else:
            amode = alpha_mode
        jobs.append(Job(sha1, group, tier, model, passes,
                        out_w, out_h, group[0].alpha, amode, backend))
    if coerced:
        notes.append("onnx-dml: non-opaque textures forced to 'split' "
                     "alpha mode (RGB + alpha upscaled separately)")
    return jobs


def load_manifest(out_root):
    recs = {}
    mf = out_root / MANIFEST_NAME
    if mf.exists():
        for line in mf.read_text(encoding="utf-8").splitlines():
            try:
                r = json.loads(line)
                recs[r["sha1"]] = r  # last record wins
            except (ValueError, KeyError):
                continue
    return recs


def job_done(job, recs, out_root, settings):
    r = recs.get(job.sha1)
    if not r:
        return False
    if (r.get("settings") != settings or r.get("model") != job.model
            or r.get("passes") != job.passes
            or r.get("alpha_mode") != job.alpha_mode
            or r.get("out") != [job.out_w, job.out_h]
            or sorted(r.get("relpaths", [])) != job.relpaths):
        return False
    # Backward-compat: pre-backend manifests lack "backend"; only invalidate
    # when a record explicitly records a different backend (same model name
    # run under both ncnn and onnx, e.g. HDcube's dual build).
    if "backend" in r and r["backend"] != job.backend:
        return False
    return all((out_root / rp).is_file() for rp in r["relpaths"])


def expected_stems(job):
    if job.alpha_mode == "split":
        return [f"{job.sha1}.rgb", f"{job.sha1}.a"]
    return [job.sha1]


def stage_job(job, in_dir):
    with Image.open(job.srcs[0].path) as im:
        im = im.convert("RGBA")
        if job.alpha_mode == "rgb":
            # Opaque: strip alpha so the binary runs its native 3-channel
            # path; constant-255 alpha is re-attached at finalize.
            im.convert("RGB").save(in_dir / f"{job.sha1}.png")
        elif job.alpha_mode == "direct":
            im.save(in_dir / f"{job.sha1}.png")
        else:  # split
            im.convert("RGB").save(in_dir / f"{job.sha1}.rgb.png")
            a = im.getchannel("A")
            Image.merge("RGB", (a, a, a)).save(in_dir / f"{job.sha1}.a.png")


def run_binary(exe, in_dir, out_dir, model, tile, gpu):
    cmd = [str(exe), "-i", str(in_dir), "-o", str(out_dir),
           "-n", model, "-s", "4", "-f", "png"]
    if tile:
        cmd += ["-t", str(tile)]
    if gpu is not None:
        cmd += ["-g", str(gpu)]
    return subprocess.run(cmd, capture_output=True, text=True)


def make_onnx_session(ort, model_path):
    """Load an ONNX model on DirectML (AMD GPU, no CUDA), CPU fallback."""
    return ort.InferenceSession(
        str(model_path),
        providers=["DmlExecutionProvider", "CPUExecutionProvider"])


def _onnx_infer_tiled(session, inp_name, out_name, arr, scale, tile, pad):
    """One image (H,W,3 float32 in [0,1]) -> upscaled (H*scale,W*scale,3).
    Tiled with padded borders so large images fit VRAM and tile seams are
    hidden (each padded tile's valid centre is cropped back out)."""
    import numpy as np
    h, w, _ = arr.shape
    out = np.zeros((h * scale, w * scale, 3), dtype=np.float32)
    step = tile if tile and tile > 0 else max(h, w)
    for y0 in range(0, h, step):
        for x0 in range(0, w, step):
            y1, x1 = min(y0 + step, h), min(x0 + step, w)
            py0, py1 = max(0, y0 - pad), min(h, y1 + pad)
            px0, px1 = max(0, x0 - pad), min(w, x1 + pad)
            patch = arr[py0:py1, px0:px1, :]
            inp = np.transpose(patch, (2, 0, 1))[None].astype(np.float32)
            res = session.run([out_name], {inp_name: inp})[0]
            res = np.clip(res[0].transpose(1, 2, 0), 0.0, 1.0)
            cy0, cx0 = (y0 - py0) * scale, (x0 - px0) * scale
            out[y0 * scale:y1 * scale, x0 * scale:x1 * scale, :] = \
                res[cy0:cy0 + (y1 - y0) * scale, cx0:cx0 + (x1 - x0) * scale, :]
    return out


def run_onnx_dir(session, scale, in_dir, out_dir, tile, pad):
    """ONNX per-image super-res over a staged directory, mirroring
    run_binary's directory contract so the pass-chain + finalize logic stays
    backend-agnostic (same input/output filenames)."""
    import numpy as np
    inp_name = session.get_inputs()[0].name
    out_name = session.get_outputs()[0].name
    n = 0
    for p in sorted(in_dir.glob("*.png")):
        with Image.open(p) as im:
            arr = np.asarray(im.convert("RGB"), dtype=np.float32) / 255.0
        up = _onnx_infer_tiled(session, inp_name, out_name, arr, scale, tile, pad)
        pix = np.clip(up * 255.0 + 0.5, 0, 255).astype("uint8")
        Image.fromarray(pix, "RGB").save(out_dir / p.name)
        n += 1
    return n


def finalize(job, last_dir, out_root, settings):
    """Recombine channels, restore exact target size + alpha semantics,
    write to every duplicate relpath atomically. Returns manifest record."""
    if job.alpha_mode == "pil":
        with Image.open(job.srcs[0].path) as im:
            resample = (Image.NEAREST if job.model == "pil-nearest"
                        else Image.LANCZOS)
            out = im.convert("RGBA").resize((job.out_w, job.out_h), resample)
    else:
        if job.alpha_mode == "split":
            with Image.open(last_dir / f"{job.sha1}.rgb.png") as rgb, \
                 Image.open(last_dir / f"{job.sha1}.a.png") as araw:
                out = rgb.convert("RGBA")
                out.putalpha(araw.convert("L"))
        else:
            with Image.open(last_dir / f"{job.sha1}.png") as im:
                out = im.convert("RGBA")
        if out.size != (job.out_w, job.out_h):
            out = out.resize((job.out_w, job.out_h), Image.LANCZOS)
    if job.alpha == "binary":
        # Model output softens cutout edges; re-threshold restores a hard
        # mask with the higher-res silhouette.
        out.putalpha(out.getchannel("A").point(lambda v: 255 if v >= 128 else 0))
    got = alpha_class(out)
    if got != job.alpha and job.alpha == "soft" and job.alpha_mode == "direct":
        # Some GPUs' upscaler hands soft alpha back flattened to 0/255 (an NVIDIA install,
        # 2026-10-30: 28 of 223 soft textures; never on the AMD card the set was made on). The
        # upscaled colours are kept and the source's own alpha, resized on the CPU, put back:
        # the same on every GPU, and textures that come out soft are untouched.
        with Image.open(job.srcs[0].path) as src:
            out.putalpha(src.convert("RGBA").getchannel("A").resize(out.size, Image.LANCZOS))
        got = alpha_class(out)
    if got != job.alpha:
        raise RuntimeError(f"alpha class changed {job.alpha} -> {got}")

    buf = io.BytesIO()
    out.save(buf, "PNG")
    data = buf.getvalue()
    for rp in job.relpaths:
        dest = out_root / rp
        dest.parent.mkdir(parents=True, exist_ok=True)
        tmp = dest.with_name(dest.name + ".tmp")
        tmp.write_bytes(data)
        os.replace(tmp, dest)
    return {
        "sha1": job.sha1, "relpaths": job.relpaths, "tier": job.tier,
        "model": job.model, "backend": job.backend, "passes": job.passes,
        "alpha": job.alpha, "alpha_mode": job.alpha_mode,
        "src": [job.srcs[0].w, job.srcs[0].h], "out": [job.out_w, job.out_h],
        "out_sha1": hashlib.sha1(data).hexdigest(), "settings": settings,
        "ts": datetime.now(timezone.utc).isoformat(timespec="seconds"),
    }


def print_plan(srcs, jobs):
    tiers = Counter(s.tier for s in srcs)
    alphas = Counter(s.alpha for s in srcs)
    print(f"files: {len(srcs)}  uniques: {len(jobs)}  "
          f"duplicates: {len(srcs) - len(jobs)}")
    print("tiers:", dict(sorted(tiers.items())))
    print("alpha:", dict(sorted(alphas.items())))
    for k, ref in REF_ALPHA.items():
        if alphas.get(k, 0) != ref and len(srcs) > 1700:
            print(f"WARNING: alpha class '{k}' = {alphas.get(k, 0)}, "
                  f"reference scan said {ref}")
    groups = defaultdict(list)
    for j in jobs:
        groups[(j.backend, j.model, j.passes)].append(j)
    print("groups (backend:model, passes -> uniques, out-long-edge histogram):")
    for (backend, model, passes), gj in sorted(groups.items()):
        hist = Counter(max(j.out_w, j.out_h) for j in gj)
        print(f"  {backend}:{model} p{passes}: {len(gj)}  "
              f"{dict(sorted(hist.items()))}")


def audit(src_root, out_root):
    errors = []
    srcs = scan_sources(src_root, errors)
    recs = load_manifest(out_root)
    out_files = {p.relative_to(out_root).as_posix()
                 for p in out_root.rglob("*.png")}
    src_rel = {s.relpath for s in srcs}
    for rp in sorted(src_rel - out_files):
        errors.append((rp, "missing output"))
    for rp in sorted(out_files - src_rel):
        errors.append((rp, "extra output with no source"))
    per_tier = Counter()
    for s in srcs:
        if s.relpath not in out_files:
            continue
        r = recs.get(s.sha1)
        if not r:
            errors.append((s.relpath, "no manifest record for source sha1"))
            continue
        try:
            with Image.open(out_root / s.relpath) as im:
                if list(im.size) != r["out"]:
                    errors.append((s.relpath,
                                   f"dims {list(im.size)} != manifest {r['out']}"))
                elif alpha_class(im.convert("RGBA")) != s.alpha:
                    errors.append((s.relpath, "alpha class mismatch vs source"))
                else:
                    per_tier[s.tier] += 1
        except Exception as e:  # noqa: BLE001
            errors.append((s.relpath, f"audit read: {e}"))
    print("audit OK per tier:", dict(sorted(per_tier.items())))
    for rp, msg in errors[:50]:
        print(f"AUDIT FAIL {rp}: {msg}", file=sys.stderr)
    if len(errors) > 50:
        print(f"... and {len(errors) - 50} more", file=sys.stderr)
    print(f"audit: {sum(per_tier.values())} verified, {len(errors)} problems")
    return 1 if errors else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("src", nargs="?", default=DEFAULT_SRC, type=Path,
                    help="source PNG tree (default Extracted/converted/textures)")
    ap.add_argument("--out", default=DEFAULT_OUT, type=Path)
    ap.add_argument("--work", default=DEFAULT_WORK, type=Path)
    ap.add_argument("--exe", default=DEFAULT_EXE, type=Path)
    ap.add_argument("--tier", help="comma list; only process these tiers")
    ap.add_argument("--backend", choices=("ncnn", "onnx-dml"), default=None,
                    help="inference backend for AI models (default: the model's "
                         "own backend, else ncnn = existing behavior)")
    ap.add_argument("--model",
                    help="model name from the registry (see --list-models); "
                         "overrides tier_rules routing + selects the backend")
    ap.add_argument("--list-models", action="store_true",
                    help="print the model registry and exit")
    ap.add_argument("--onnx-model-dir", default=ONNX_MODEL_DIR, type=Path,
                    help="dir holding user-supplied .onnx files "
                         "(default ThirdParty/onnx_models)")
    ap.add_argument("--onnx-tile", type=int, default=256,
                    help="onnx tile size in px (0 = whole image; lower on OOM)")
    ap.add_argument("--onnx-overlap", type=int, default=16,
                    help="onnx tile padding/overlap px to hide seams")
    ap.add_argument("--model-override",
                    help="force one model for all selected tiers (pilot use); "
                         "arbitrary name allowed, --model prefers the registry")
    ap.add_argument("--alpha-mode", choices=("auto", "direct", "split"),
                    default="auto", help="non-opaque handling (auto = tier_rules)")
    ap.add_argument("--sample", type=int,
                    help="process only N stratified samples per tier")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--tile", type=int, default=0,
                    help="ncnn tile size, 0=auto (try 512/256 on Vulkan OOM)")
    ap.add_argument("--gpu", type=int, help="ncnn GPU index (default auto)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--audit", action="store_true",
                    help="verify outputs against sources + manifest, then exit")
    ap.add_argument("--force", action="store_true",
                    help="redo jobs even if manifest says done")
    ap.add_argument("--keep-work", action="store_true")
    args = ap.parse_args()

    if args.list_models:
        list_models()
        sys.exit(0)
    if args.model and args.model not in MODELS:
        ap.error(f"unknown --model '{args.model}'; run --list-models "
                 f"(or use --model-override for an arbitrary ncnn model name)")
    model_override = args.model or args.model_override

    if args.audit:
        sys.exit(audit(args.src, args.out))

    errors = []
    srcs = scan_sources(args.src, errors)
    if args.tier:
        wanted = set(args.tier.split(","))
        unknown = wanted - set(tier_rules.TIERS)
        if unknown:
            ap.error(f"unknown tier(s): {sorted(unknown)}")
        srcs = [s for s in srcs if s.tier in wanted]
    if args.sample:
        srcs = sample_srcs(srcs, args.sample, args.seed)

    alpha_mode = (tier_rules.ALPHA_MODE if args.alpha_mode == "auto"
                  else args.alpha_mode)
    notes = []
    jobs = plan_jobs(srcs, model_override, alpha_mode, notes, args.backend)
    print_plan(srcs, jobs)
    for n in notes:
        print("note:", n)
    if args.dry_run:
        print("dry run - nothing executed")
        sys.exit(1 if errors else 0)

    settings = tier_rules.settings_hash()
    recs = load_manifest(args.out)
    todo = [j for j in jobs
            if args.force or not job_done(j, recs, args.out, settings)]
    print(f"to do: {len(todo)} uniques ({len(jobs) - len(todo)} already done)")

    needs_exe = any(j.backend == "ncnn" for j in todo)
    if needs_exe and not args.exe.is_file():
        print(f"error: upscaler not found: {args.exe}", file=sys.stderr)
        sys.exit(1)

    # ONNX/DirectML backend: import-guard onnxruntime + the user-supplied model
    # files up front so a missing dep prints a clear message (never a traceback)
    # and never touches the working ncnn path.
    sessions = {}
    onnx_models = sorted({j.model for j in todo if j.backend == "onnx-dml"})
    if onnx_models:
        ort = import_onnxruntime()
        if ort is None:
            print(ONNX_GUIDANCE, file=sys.stderr)
            sys.exit(2)
        if "DmlExecutionProvider" not in ort.get_available_providers():
            print("warning: onnxruntime present but DmlExecutionProvider "
                  "unavailable; using CPUExecutionProvider (slow). Install "
                  "onnxruntime-directml for the AMD GPU.", file=sys.stderr)
        for model in onnx_models:
            mf = onnx_model_path(model, args.onnx_model_dir)
            if mf is None:
                print(f"error: model '{model}' has no onnx build; pick an onnx "
                      f"model (see --list-models) or use --backend ncnn",
                      file=sys.stderr)
                sys.exit(2)
            if not mf.is_file():
                print(f"error: onnx model file not found: {mf}\n"
                      f"  supply it: {MODELS.get(model, {}).get('note', '.onnx')}\n"
                      f"  (see --list-models)", file=sys.stderr)
                sys.exit(2)
            try:
                sessions[model] = make_onnx_session(ort, mf)
            except Exception as e:  # noqa: BLE001
                print(f"error: failed to load onnx model '{model}': {e}",
                      file=sys.stderr)
                sys.exit(2)

    args.out.mkdir(parents=True, exist_ok=True)
    done = 0
    with open(args.out / MANIFEST_NAME, "a", encoding="utf-8") as mf:

        def finish(job, last_dir):
            nonlocal done
            try:
                rec = finalize(job, last_dir, args.out, settings)
                mf.write(json.dumps(rec) + "\n")
                mf.flush()
                done += 1
            except Exception as e:  # noqa: BLE001
                errors.append((job.srcs[0].relpath, f"finalize: {e}"))

        groups = defaultdict(list)
        for j in todo:
            groups[(j.backend, j.model, j.passes)].append(j)
        for (backend, model, passes), gjobs in sorted(groups.items()):
            if backend == "pil":  # pil-* pseudo-models: no GPU involved
                for job in gjobs:
                    finish(job, None)
                print(f"{model}: {len(gjobs)} done in Pillow")
                continue
            gdir = args.work / f"{backend}_{model}_p{passes}"
            if gdir.exists():
                shutil.rmtree(gdir)
            in_dir = gdir / "in"
            in_dir.mkdir(parents=True)
            staged = []
            for job in gjobs:
                try:
                    stage_job(job, in_dir)
                    staged.append(job)
                except Exception as e:  # noqa: BLE001
                    errors.append((job.srcs[0].relpath, f"stage: {e}"))
            cur = in_dir
            for i in range(1, passes + 1):
                nxt = gdir / f"p{i}"
                nxt.mkdir()
                t0 = time.time()
                if backend == "ncnn":
                    proc = run_binary(args.exe, cur, nxt, model,
                                      args.tile, args.gpu)
                    rc = proc.returncode
                    if rc != 0:
                        tail = proc.stderr.strip().splitlines()[-3:]
                        print("  stderr: " + " | ".join(tail), file=sys.stderr)
                else:  # onnx-dml
                    scale = MODELS.get(model, {}).get("scale", 4)
                    run_onnx_dir(sessions[model], scale, cur, nxt,
                                 args.onnx_tile, args.onnx_overlap)
                    rc = 0
                n_out = sum(1 for _ in nxt.glob("*.png"))
                print(f"{backend}:{model} p{passes} pass {i}: {n_out} files "
                      f"in {time.time() - t0:.0f}s (rc={rc})")
                cur = nxt
            for job in staged:
                missing = [s for s in expected_stems(job)
                           if not (cur / f"{s}.png").is_file()]
                if missing:
                    errors.append((job.srcs[0].relpath,
                                   f"{backend} produced no output ({missing})"))
                else:
                    finish(job, cur)
            if not args.keep_work:
                shutil.rmtree(gdir, ignore_errors=True)

    total_outputs = sum(len(j.relpaths) for j in todo) - len(errors)
    print(f"upscaled {done}/{len(todo)} uniques -> ~{total_outputs} files, "
          f"{len(errors)} errors")
    for rp, msg in errors[:20]:
        print(f"ERROR {rp}: {msg}", file=sys.stderr)
    if len(errors) > 20:
        print(f"... and {len(errors) - 20} more", file=sys.stderr)
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
