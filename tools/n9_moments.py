# n9_moments.py - N9 stage 2 (docs/TODO.md): a run's pictures between frames checked. Made with TF_N9_SHOTS=<n> and
# TF_NATIVE_SHOT on consecutive frames (runs\remote_<run>\native_<tick>_m<k>_t<t x 1000>.bmp): each frame's t = 0 picture
# against the frame before's t = 1 (they should be the same picture), and the change between consecutive pictures (the
# mean, and the shift by phase correlation in three regions, in pixels) - even steps through a frame and across frames.
# usage: python tools\n9_moments.py <run> [--montage]   (needs numpy, Pillow)
import glob
import os
import re
import sys

import numpy as np
from PIL import Image

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'runs')
run = sys.argv[1]
files = glob.glob(os.path.join(root, 'remote_' + run, 'native_*_m*_t*.bmp'))


def key(f):
    m = re.search(r'native_(\d+)_m(\d+)_t(\d+)', f)
    return (int(m.group(1)), int(m.group(3)))


files.sort(key=key)
imgs = {key(f): np.asarray(Image.open(f).convert('RGB'), dtype=np.float32) for f in files}


def gray(a):
    return a.mean(axis=2)


def shift(a, b, region):
    y0, y1, x0, x1 = region
    A = gray(a)[y0:y1, x0:x1]
    B = gray(b)[y0:y1, x0:x1]
    A = A - A.mean()
    B = B - B.mean()
    win = np.outer(np.hanning(A.shape[0]), np.hanning(A.shape[1]))
    F = np.fft.fft2(A * win) * np.conj(np.fft.fft2(B * win))
    r = np.fft.ifft2(F / (np.abs(F) + 1e-9)).real
    iy, ix = np.unravel_index(np.argmax(r), r.shape)
    if iy > r.shape[0] // 2:
        iy -= r.shape[0]
    if ix > r.shape[1] // 2:
        ix -= r.shape[1]
    return -ix, -iy


keys = sorted(imgs)
if not keys:
    sys.exit('no pictures between frames in runs\\remote_' + run)
h, w = imgs[keys[0]].shape[:2]
regions = {'left': (h // 4, 3 * h // 4, w // 16, w // 16 + w // 4), 'right': (h // 4, 3 * h // 4, w - w // 16 - w // 4, w - w // 16),
           'top': (h // 16, h // 16 + h // 4, w // 3, 2 * w // 3)}
ticks = sorted(set(k[0] for k in keys))
for i, t in enumerate(ticks):
    if i > 0 and (ticks[i - 1], 1000) in imgs and (t, 0) in imgs:
        d = np.abs(imgs[(t, 0)] - imgs[(ticks[i - 1], 1000)])
        print(f'{t} t=0 vs {ticks[i - 1]} t=1: mean {d.mean():.3f}/255, pixels off by >8: {100 * (d.max(axis=2) > 8).mean():.3f}%')
seq = [k for k in keys if not (k[1] == 0 and any((p, 1000) in imgs for p in ticks if p < k[0]))]
for a, b in zip(seq, seq[1:]):
    d = np.abs(imgs[a] - imgs[b])
    s = {n: shift(imgs[a], imgs[b], r) for n, r in regions.items()}
    print(f'{a} -> {b}: mean change {d.mean():.2f}/255; shift ' + ', '.join(f'{n} {v[0]:+d},{v[1]:+d}' for n, v in s.items()))
if '--montage' in sys.argv:
    tiles = [Image.fromarray(imgs[k].astype(np.uint8)).resize((w // 3, h // 3)) for k in seq]
    m = Image.new('RGB', (w // 3, (h // 3) * len(tiles)))
    for i, t in enumerate(tiles):
        m.paste(t, (0, i * (h // 3)))
    m.save(os.path.join(root, f'montage_{run}.png'))
