"""Describes a sound dump (PS2X_AUDIO_DUMP / run.ps1 -DumpSound) without playing it.

Usage: python tools/wav_stats.py runs/<name>.wav [--step 1.0] [--from 0] [--to 0] [--png out.png]

Prints the level of every stretch of `--step` seconds (RMS and peak in dB below full
scale, per channel), the stretches that are silent, how often samples sit at full scale
(clipping), and the strongest frequencies of the whole. --png draws the waveform envelope
and a spectrogram, to see at a glance whether there is music, speech or noise.
"""

import argparse
import struct
import sys

import numpy as np


def read_wav(path):
    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        sys.exit("not a WAV file")
    channels, rate, bits = 2, 48000, 16
    pos = 12
    start = None
    while pos + 8 <= len(data):
        tag, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        if tag == b"fmt ":
            _, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", data, pos + 8)
        elif tag == b"data":
            start = pos + 8
            break
        pos += 8 + size + (size & 1)
    if start is None or bits != 16:
        sys.exit("unsupported WAV file")
    # A dump that was cut off has more data than its header says: take all of it.
    samples = np.frombuffer(data, dtype="<i2", offset=start, count=(len(data) - start) // 2)
    return samples[:len(samples) // channels * channels].reshape(-1, channels), rate


def db(value):
    return -120.0 if value <= 0 else 20.0 * np.log10(value / 32768.0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("wav")
    parser.add_argument("--step", type=float, default=1.0)
    parser.add_argument("--from", dest="start", type=float, default=0.0)
    parser.add_argument("--to", dest="end", type=float, default=0.0)
    parser.add_argument("--png")
    args = parser.parse_args()

    samples, rate = read_wav(args.wav)
    total = len(samples) / rate
    first = int(args.start * rate)
    last = int(args.end * rate) if args.end > 0 else len(samples)
    samples = samples[first:last].astype(np.float64)
    print(f"{args.wav}: {total:.2f} s, {rate} Hz, {samples.shape[1]} channels; looking at "
          f"{args.start:.2f}-{args.start + len(samples) / rate:.2f} s")
    if len(samples) == 0:
        return

    step = max(1, int(args.step * rate))
    silent = 0
    for at in range(0, len(samples), step):
        chunk = samples[at:at + step]
        rms = np.sqrt((chunk ** 2).mean(axis=0))
        peak = np.abs(chunk).max(axis=0)
        if peak.max() == 0:
            silent += 1
        line = "  ".join(f"{db(r):6.1f}/{db(p):6.1f}" for r, p in zip(rms, peak))
        print(f"  {args.start + at / rate:7.2f} s  rms/peak dB  {line}")
    clipped = int((np.abs(samples) >= 32767).sum())
    print(f"silent stretches: {silent}; samples at full scale: {clipped} "
          f"({100.0 * clipped / samples.size:.3f}%)")

    mono = samples.mean(axis=1)
    if len(mono) >= 4096:
        window = np.hanning(len(mono))
        spectrum = np.abs(np.fft.rfft(mono * window))
        freqs = np.fft.rfftfreq(len(mono), 1.0 / rate)
        bands = [(0, 100), (100, 300), (300, 1000), (1000, 3000), (3000, 8000), (8000, 24000)]
        energy = [float((spectrum[(freqs >= lo) & (freqs < hi)] ** 2).sum()) for lo, hi in bands]
        whole = sum(energy) or 1.0
        print("energy by band: " + ", ".join(f"{lo}-{hi} Hz {100.0 * e / whole:.1f}%"
                                             for (lo, hi), e in zip(bands, energy)))

    if args.png:
        draw_png(args.png, samples, mono, rate, args.start)


def draw_png(path, samples, mono, rate, start):
    """Waveform envelope (left channel up, right channel down) over a spectrogram (0-12 kHz),
    drawn with Pillow."""
    from PIL import Image, ImageDraw

    width, envelope_height, spectrum_height, margin = 1400, 220, 360, 40
    image = Image.new("RGB", (width + margin, envelope_height + spectrum_height + 30), (16, 16, 20))
    draw = ImageDraw.Draw(image)
    count = len(mono)
    seconds = count / rate

    # Envelope: the peak of each column's stretch of samples.
    middle = envelope_height // 2
    edges = np.linspace(0, count, width + 1).astype(np.int64)
    for x in range(width):
        chunk = samples[edges[x]:max(edges[x + 1], edges[x] + 1)]
        left = float(np.abs(chunk[:, 0]).max()) / 32768.0
        right = float(np.abs(chunk[:, -1]).max()) / 32768.0
        draw.line([(margin + x, middle), (margin + x, middle - int(left * (middle - 4)))], fill=(90, 170, 255))
        draw.line([(margin + x, middle), (margin + x, middle + int(right * (middle - 4)))], fill=(255, 150, 80))
    draw.line([(margin, middle), (margin + width, middle)], fill=(70, 70, 80))
    draw.text((2, 4), "L", fill=(90, 170, 255))
    draw.text((2, envelope_height - 14), "R", fill=(255, 150, 80))

    # Spectrogram: 2048-sample frames every 1024 samples, in dB, one frame per column.
    top = envelope_height + 10
    nfft, hop = 2048, 1024
    if count >= nfft:
        frames = 1 + (count - nfft) // hop
        window = np.hanning(nfft)
        picks = np.linspace(0, frames - 1, width).astype(np.int64)
        bins = int(12000 * nfft / rate)
        columns = np.empty((spectrum_height, width))
        rows = np.linspace(bins - 1, 0, spectrum_height).astype(np.int64)
        for x, frame in enumerate(picks):
            spectrum = np.abs(np.fft.rfft(mono[frame * hop:frame * hop + nfft] * window))[:bins]
            columns[:, x] = 20.0 * np.log10(spectrum[rows] + 1e-6)
        peak = columns.max()
        level = np.clip((columns - (peak - 90.0)) / 90.0, 0.0, 1.0)
        # Black - purple - orange - pale yellow.
        stops = np.array([[0, 0, 4], [80, 18, 123], [229, 89, 52], [252, 253, 191]], dtype=np.float64)
        position = level * (len(stops) - 1)
        low = np.minimum(position.astype(np.int64), len(stops) - 2)
        frac = (position - low)[..., None]
        rgb = (stops[low] * (1.0 - frac) + stops[low + 1] * frac).astype(np.uint8)
        image.paste(Image.fromarray(rgb, "RGB"), (margin, top))
        for khz in (0, 4, 8, 12):
            y = top + spectrum_height - 1 - int(khz / 12.0 * (spectrum_height - 1))
            draw.text((2, max(top, y - 6)), f"{khz}k", fill=(200, 200, 200))

    # Time ticks.
    for k in range(11):
        x = margin + int(k / 10.0 * (width - 1))
        draw.line([(x, envelope_height + spectrum_height + 10), (x, envelope_height + spectrum_height + 14)], fill=(200, 200, 200))
        draw.text((x - 12, envelope_height + spectrum_height + 15), f"{start + k / 10.0 * seconds:.1f}s", fill=(200, 200, 200))
    image.save(path)
    print("wrote", path)


if __name__ == "__main__":
    main()
