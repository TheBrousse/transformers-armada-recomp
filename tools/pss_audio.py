"""Checks a movie's sound in a sound dump against the movie file (debugging aid).

    python tools/pss_audio.py game/FMV/NATARI.PSS runs/fmv.wav            # find it and compare
    python tools/pss_audio.py game/FMV/NATARI.PSS --wav runs/natari.wav   # just extract the track

A .PSS movie carries its sound as 16-bit PCM in private stream 1 ("SShd" header, then
"SSbd" data, left and right interleaved in blocks). The dump is what PS2X_AUDIO_DUMP (or
run.ps1 -DumpSound) wrote. The movie's loudest second is searched for in the dump, which
gives where the movie started; then every block of 2048 samples of the dump (what the game
hands to the sound driver at a time) is looked up in the movie. A block that is not the next
one of the movie is reported: repeated blocks and gaps are how a starved stream shows.
"""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from wav_stats import read_wav  # noqa: E402

BLOCK = 2048


def pss_pcm(path):
    """Returns (samples[n, channels], rate) of the movie's PCM track."""
    data = Path(path).read_bytes()
    out = bytearray()
    header = None
    pos = 0
    end = len(data)
    while pos + 4 <= end:
        if data[pos:pos + 3] != b"\x00\x00\x01":
            pos += 1
            continue
        code = data[pos + 3]
        if code == 0xBA:  # pack header
            pos += 14 + (data[pos + 13] & 7)
            continue
        if code == 0xB9:  # program end
            break
        length = (data[pos + 4] << 8) | data[pos + 5]
        if code == 0xBD:  # private stream 1
            payload = data[pos + 9 + data[pos + 8]:pos + 6 + length]
            if payload[:1] == b"\xff" and (payload[1] & 0xF0) == 0xA0:
                payload = payload[4:]
                if header is None:
                    if payload[:4] != b"SShd":
                        raise SystemExit("the PCM track does not start with an SShd header")
                    size = struct.unpack_from("<I", payload, 4)[0]
                    header = payload[8:8 + size]
                    payload = payload[8 + size + 8:]  # "SSbd" and its size follow
                out += payload
        pos += 6 + length
    if header is None:
        raise SystemExit("no PCM track in " + str(path))
    kind, rate, channels, interleave = struct.unpack_from("<IIII", header, 0)
    if kind != 1:
        raise SystemExit(f"the sound track is not 16-bit PCM (type {kind})")
    samples = np.frombuffer(bytes(out[:len(out) // 2 * 2]), dtype="<i2")
    per_block = interleave // 2
    blocks = len(samples) // (per_block * channels)
    samples = samples[:blocks * per_block * channels].reshape(blocks, channels, per_block)
    return samples.transpose(0, 2, 1).reshape(-1, channels), rate


def write_wav(path, samples, rate):
    body = samples.astype("<i2").tobytes()
    channels = samples.shape[1]
    with open(path, "wb") as out:
        out.write(b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVEfmt ")
        out.write(struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * channels * 2, channels * 2, 16))
        out.write(b"data" + struct.pack("<I", len(body)) + body)


def find(needle, hay):
    """Best normalised correlation of needle in hay: (score, offset)."""
    size = 1 << int(np.ceil(np.log2(len(hay) + len(needle))))
    cross = np.fft.irfft(np.fft.rfft(hay, size) * np.conj(np.fft.rfft(needle, size)), size)[:len(hay) - len(needle) + 1]
    squares = np.concatenate(([0.0], np.cumsum(hay ** 2)))
    energy = squares[len(needle):len(needle) + len(cross)] - squares[:len(cross)]
    score = cross / np.sqrt(np.maximum(energy, 1e-9) * max(float((needle ** 2).sum()), 1e-9))
    at = int(np.argmax(score))
    return float(score[at]), at


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("movie")
    parser.add_argument("dump", nargs="?")
    parser.add_argument("--wav", help="write the movie's sound track here")
    parser.add_argument("--from", dest="start", type=float, default=0.0, help="search the dump from this second")
    parser.add_argument("--to", dest="stop", type=float, help="search the dump up to this second")
    parser.add_argument("--list", action="store_true", help="print every block, not only the breaks")
    args = parser.parse_args()

    movie, rate = pss_pcm(args.movie)
    print(f"{args.movie}: {len(movie) / rate:.2f} s of sound, {movie.shape[1]} channels at {rate} Hz")
    if args.wav:
        write_wav(args.wav, movie, rate)
        print("wrote", args.wav)
    if not args.dump:
        return

    dump, dump_rate = read_wav(args.dump)
    if dump_rate != rate:
        raise SystemExit(f"the dump is at {dump_rate} Hz, the movie at {rate} Hz")
    left = movie[:, 0].astype(np.float64)
    heard = dump[:, 0].astype(np.float64)
    first = int(args.start * rate)
    last = int(args.stop * rate) if args.stop else len(heard)

    # Where the movie starts: its loudest second, found in the dump.
    seconds = len(left) // rate
    if seconds < 1:
        raise SystemExit("the movie's sound is shorter than a second")
    loudest = int(np.argmax([(left[s * rate:(s + 1) * rate] ** 2).sum() for s in range(seconds)]))
    score, at = find(left[loudest * rate:(loudest + 1) * rate], heard[first:last])
    start = first + at - loudest * rate
    print(f"second {loudest} of the movie is at {(first + at) / rate:.3f} s of the dump (correlation {score:.3f}): "
          f"the movie starts at {start / rate:.3f} s")
    if score < 0.5:
        print("that is no match: the movie's sound is not in this part of the dump")
        return

    squares = np.concatenate(([0.0], np.cumsum(left ** 2)))
    size = 1 << int(np.ceil(np.log2(len(left) + BLOCK)))
    spectrum = np.fft.rfft(left, size)
    blocks = len(left) // BLOCK
    good = silent = wrong = 0
    expected = None
    for block in range(-2, blocks + 2):
        begin = start + block * BLOCK
        if begin < 0 or begin + BLOCK > len(heard):
            continue
        piece = heard[begin:begin + BLOCK]
        power = float((piece ** 2).sum())
        if power < 1e5:
            silent += 1
            expected = None
            if args.list:
                print(f"  block {block:4d}: silent")
            continue
        cross = np.fft.irfft(spectrum * np.conj(np.fft.rfft(piece, size)), size)[:len(left) - BLOCK + 1]
        energy = squares[BLOCK:BLOCK + len(cross)] - squares[:len(cross)]
        match = cross / np.sqrt(np.maximum(energy, 1e-9) * power)
        where = int(np.argmax(match))
        value = float(match[where])
        position = where / BLOCK
        in_order = expected is None or abs(position - expected) < 0.02
        if value >= 0.98 and in_order:
            good += 1
        else:
            wrong += 1
            print(f"  block {block:4d} ({begin / rate:8.3f} s): movie block {position:8.2f}, correlation {value:.2f}"
                  + ("" if in_order else f", expected {expected:.2f}"))
        if args.list and value >= 0.98 and in_order:
            print(f"  block {block:4d}: movie block {position:8.2f}")
        expected = position + 1 if value >= 0.9 else None
    print(f"{good} blocks follow the movie, {wrong} do not, {silent} are silent")


if __name__ == "__main__":
    main()
