"""Decodes one of the game's sound streams (SOUND/**/*.MS) to a WAV file, or compares it
with a sound dump of the running game (to check the emulated sound processor).

Usage: python tools/ms_decode.py game/SOUND/MUSIC/UI_TITLE.MS out.wav
       python tools/ms_decode.py game/SOUND/MUSIC/UI_TITLE.MS --compare runs/<name>.wav [--from 28 --to 60]

An .MS file ("SL3", "MS2") is a 0x28-byte header - channels at 0x14, sample rate at 0x18,
bytes of sound per channel at 0x1C, interleave at 0x20, start of the sound at 0x24 - and
then the channels in turns of `interleave` bytes each, as the console's ADPCM blocks:
16 bytes for 28 samples (shift and filter, loop flags, 28 nibbles).

--compare looks for the stream in the dump: it resamples the decoded stream to 48 kHz and
slides a few seconds of it over the dump. A correlation near 1 means the dump holds that
sound, at the reported time and volume.
"""

import struct
import sys

import numpy as np

FILTERS = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]


def decode_channel(data):
    """ADPCM blocks -> int16 samples."""
    blocks = len(data) // 16
    out = np.zeros(blocks * 28, dtype=np.int16)
    prev1 = prev2 = 0
    at = 0
    for b in range(blocks):
        block = data[16 * b:16 * b + 16]
        shift = (block[0] & 0xF) + 16
        f1, f2 = FILTERS[(block[0] >> 4) & 0xF] if ((block[0] >> 4) & 0xF) < 5 else (0, 0)
        for i in range(28):
            byte = block[2 + (i >> 1)]
            nibble = (byte >> 4) if (i & 1) else (byte & 0xF)
            value = (nibble << 28) & 0xFFFFFFFF
            if value & 0x80000000:
                value -= 1 << 32
            sample = (value >> shift) + ((f1 * prev1 + f2 * prev2 + 32) >> 6)
            sample = max(-32768, min(32767, sample))
            out[at] = sample
            at += 1
            prev2, prev1 = prev1, sample
    return out


def read_ms(path, seconds=None):
    data = open(path, "rb").read()
    if data[:3] != b"SL3" or data[4:7] != b"MS2":
        sys.exit("not an SL3 MS2 stream")
    channels, rate, size, interleave, start = struct.unpack_from("<5I", data, 0x14)
    body = data[start:]
    if seconds:
        blocks = int(seconds * rate / 28) + 1
        body = body[:((blocks * 16 + interleave - 1) // interleave) * interleave * channels]
    per_channel = [bytearray() for _ in range(channels)]
    for at in range(0, len(body), interleave * channels):
        for c in range(channels):
            per_channel[c] += body[at + c * interleave:at + (c + 1) * interleave]
    decoded = [decode_channel(bytes(ch)) for ch in per_channel]
    length = min(len(d) for d in decoded)
    return np.stack([d[:length] for d in decoded], axis=1), rate, size


def write_wav(path, samples, rate):
    samples = np.ascontiguousarray(samples, dtype="<i2")
    with open(path, "wb") as out:
        out.write(b"RIFF" + struct.pack("<I", 36 + samples.nbytes) + b"WAVEfmt ")
        out.write(struct.pack("<IHHIIHH", 16, 1, samples.shape[1], rate, rate * 2 * samples.shape[1],
                              2 * samples.shape[1], 16))
        out.write(b"data" + struct.pack("<I", samples.nbytes) + samples.tobytes())


def resample(signal, source_rate, target_rate):
    positions = np.arange(int(len(signal) * target_rate / source_rate)) * (source_rate / target_rate)
    return np.interp(positions, np.arange(len(signal)), signal)


def compare(ms_path, wav_path, start, end):
    from wav_stats import read_wav

    reference, rate, _ = read_ms(ms_path, seconds=20)
    dump, dump_rate = read_wav(wav_path)
    first = int(start * dump_rate)
    last = int(end * dump_rate) if end > 0 else len(dump)
    dump = dump[first:last].astype(np.float64)
    for channel in range(min(reference.shape[1], dump.shape[1])):
        ref = resample(reference[:, channel].astype(np.float64), rate, dump_rate)
        # A stretch of the stream away from its (possibly quiet) beginning.
        piece = ref[int(2.0 * dump_rate):int(6.0 * dump_rate)]
        if len(piece) < dump_rate or len(dump) < len(piece):
            sys.exit("too short to compare")
        signal = dump[:, channel]
        # Correlation of the piece with every position of the dump, through an FFT.
        size = 1 << int(np.ceil(np.log2(len(signal) + len(piece))))
        cross = np.fft.irfft(np.fft.rfft(signal, size) * np.conj(np.fft.rfft(piece, size)), size)[:len(signal) - len(piece)]
        squares = np.concatenate(([0.0], np.cumsum(signal ** 2)))
        energy = squares[len(piece):len(piece) + len(cross)] - squares[:len(cross)]
        norm = np.sqrt(np.maximum(energy, 1e-9) * (piece ** 2).sum())
        score = cross / norm
        best = int(np.argmax(score))
        window = signal[best:best + len(piece)]
        gain = float((window * piece).sum() / max((piece ** 2).sum(), 1e-9))
        print(f"channel {channel}: best correlation {score[best]:.4f} with the stream's 2-6 s at "
              f"{start + best / dump_rate:.3f} s of the dump (stream starts at "
              f"{start + best / dump_rate - 2.0:.3f} s), volume {gain:.3f} of the stream's")


def main():
    args = sys.argv[1:]
    if len(args) >= 3 and args[1] == "--compare":
        start = float(args[args.index("--from") + 1]) if "--from" in args else 0.0
        end = float(args[args.index("--to") + 1]) if "--to" in args else 0.0
        compare(args[0], args[2], start, end)
        return
    if len(args) != 2:
        sys.exit(__doc__)
    samples, rate, size = read_ms(args[0])
    write_wav(args[1], samples, rate)
    print(f"{args[0]}: {samples.shape[1]} channels, {rate} Hz, {len(samples) / rate:.2f} s -> {args[1]}")


if __name__ == "__main__":
    main()
