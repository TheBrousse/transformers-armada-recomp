#!/usr/bin/env python3
"""Upscales the HQ's level previews (game/FMV/LEVELS/*.M2V) 4x for the native renderer's HD previews (TODO 159).

The previews are 512x256 MPEG-2, drawn in the HQ's level select across most of the screen. This makes each one a
2048x1024 movie with exactly the same frames, which the native renderer draws in its place (nr_movie.cpp,
TF_HD_PREVIEWS) - frame N for the game's picture N:

  1. decode, deinterlaced (bwdif: the pictures are interlaced, bottom field first, though flagged progressive - the
     combing becomes saw teeth when upscaled) and deblocked (the MPEG-2 8x8 block edges, else sharpened too),
     one picture per frame - the frame count unchanged;
  2. Real-ESRGAN (realesrgan-ncnn-vulkan, Vulkan: any GPU) 4x, model realesrgan-x4plus;
  3. blended with a plain Lanczos 4x of the same frames (--blend: the AI's share, 0.6) - x4plus alone invents
     ragged spikes on fine silhouettes (Greenland's pines); the blend keeps its detail without them;
  4. encoded H.264 (CRF 16) to <out>/LEVELS/<name>.mkv, its frame count checked against the source's.

Game data stays on this machine: the output goes to movies_hd/ (git-ignored), never into a repository.

Usage (from the recomp's root):
  python transformers/tools/upscale_previews.py --realesrgan <folder of realesrgan-ncnn-vulkan.exe> --ffmpeg <ffmpeg.exe>
      [--game game] [--out movies_hd] [--only N02ANT] [--blend 0.6] [--model realesrgan-x4plus] [--force]
Takes a few minutes a movie on a recent GPU; skips movies already done (unless --force).
"""
import argparse
import os
import shutil
import subprocess
import sys


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"failed ({r.returncode}): {' '.join(cmd)}\n{r.stderr[-2000:]}")
    return r.stdout


def frames(ffprobe, path):
    out = run([ffprobe, "-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries", "stream=nb_read_frames,r_frame_rate",
               "-of", "csv=p=0", path]).strip().split(",")
    num, den = out[0].split("/")
    return int(out[1]), float(num) / float(den)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--realesrgan", required=True, help="folder holding realesrgan-ncnn-vulkan.exe and its models")
    ap.add_argument("--ffmpeg", required=True, help="ffmpeg.exe (ffprobe.exe beside it)")
    ap.add_argument("--game", default="game", help="the extracted disc (its FMV/LEVELS)")
    ap.add_argument("--out", default="movies_hd", help="where LEVELS/<name>.mkv go (git-ignored)")
    ap.add_argument("--only", default="", help="one preview, e.g. N02ANT")
    ap.add_argument("--model", default="realesrgan-x4plus")
    ap.add_argument("--blend", type=float, default=0.6, help="the AI image's share against a Lanczos 4x (1: the AI's alone)")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()

    ffprobe = os.path.join(os.path.dirname(a.ffmpeg), "ffprobe.exe")
    esrgan = os.path.join(a.realesrgan, "realesrgan-ncnn-vulkan.exe")
    src_dir = os.path.join(a.game, "FMV", "LEVELS")
    out_dir = os.path.join(a.out, "LEVELS")
    os.makedirs(out_dir, exist_ok=True)
    names = sorted(os.path.splitext(f)[0] for f in os.listdir(src_dir) if f.upper().endswith(".M2V"))
    if a.only:
        names = [n for n in names if n.upper() == a.only.upper()]
    for name in names:
        src = os.path.join(src_dir, name + ".M2V")
        dst = os.path.join(out_dir, name + ".mkv")
        if os.path.exists(dst) and not a.force:
            print(f"{name}: done already")
            continue
        count, fps = frames(ffprobe, src)
        work = os.path.join(a.out, "_work", name)
        shutil.rmtree(work, ignore_errors=True)
        lo, hi = os.path.join(work, "lo"), os.path.join(work, "hi")
        os.makedirs(lo)
        os.makedirs(hi)
        print(f"{name}: {count} pictures at {fps:g}/s - decoding")
        run([a.ffmpeg, "-v", "error", "-i", src, "-vf", "bwdif=mode=send_frame:parity=bff:deint=all,deblock=filter=strong:block=8",
             "-fps_mode", "passthrough", os.path.join(lo, "%06d.png")])
        got = len(os.listdir(lo))
        if got != count:
            sys.exit(f"{name}: {got} frames decoded, {count} in the source")
        print(f"{name}: upscaling ({a.model})")
        run([esrgan, "-i", lo, "-o", hi, "-n", a.model, "-s", "4", "-f", "png"])
        print(f"{name}: encoding")
        blend = f"[1:v]scale=iw*4:ih*4:flags=lanczos[l];[0:v][l]blend=all_expr='A*{a.blend:g}+B*{1.0 - a.blend:g}',format=yuv420p"
        run([a.ffmpeg, "-v", "error", "-y", "-framerate", f"{fps:g}", "-i", os.path.join(hi, "%06d.png"), "-framerate", f"{fps:g}",
             "-i", os.path.join(lo, "%06d.png"), "-filter_complex", blend, "-c:v", "libx264", "-crf", "16", "-preset", "slow",
             "-r", f"{fps:g}", dst])
        n, _ = frames(ffprobe, dst)
        if n != count:
            sys.exit(f"{name}: {n} frames encoded, {count} in the source")
        shutil.rmtree(work, ignore_errors=True)
        print(f"{name}: {dst} ({os.path.getsize(dst) >> 20} MB, {n} frames)")
    shutil.rmtree(os.path.join(a.out, "_work"), ignore_errors=True)


if __name__ == "__main__":
    main()
