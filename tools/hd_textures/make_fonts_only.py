"""Extract and convert only font assets from the player's disc archive, without GPU work."""
import argparse
from pathlib import Path
import subprocess
import sys
import time
import zlib

from extract_pk_archive import read_central_directory, read_entry_data, safe_relpath
from itf_to_png import convert_file


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--game', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--preview', type=Path)
    ap.add_argument('--controls', type=Path)
    args = ap.parse_args()
    start = time.perf_counter()
    # A single low-priority process, including the generator child.
    if sys.platform == 'win32':
        import ctypes
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)
    archive = args.game / 'TFA2.ZIP'
    archives = args.out / 'archives'
    converted = args.out / 'converted' / 'textures'
    count = 0
    with archive.open('rb') as fh:
        for name, method, crc, csize, usize, lofs in read_central_directory(fh):
            rel = safe_relpath(name)
            if len(rel.parts) != 2 or rel.parts[0].upper() != 'FONTS' or rel.suffix.upper() not in ('.VBN', '.ITF'):
                continue
            data = read_entry_data(fh, method, csize, lofs)
            if zlib.crc32(data) & 0xffffffff != crc:
                raise ValueError(f'{name}: CRC mismatch')
            dest = archives / 'TFA2' / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            if not dest.exists() or dest.read_bytes() != data:
                dest.write_bytes(data)
            if dest.suffix.upper() == '.ITF':
                convert_file(dest, (converted / 'TFA2' / rel).with_suffix('.png'), True)
            count += 1
    if not count:
        raise ValueError('no font assets in TFA2.ZIP')
    cmd = [sys.executable, str(Path(__file__).with_name('make_crisp_fonts.py')),
           '--archives', str(archives), '--converted', str(converted), '--out', str(args.out / 'crisp_fonts')]
    if (args.out / "upscaled").is_dir():
        cmd += ["--upscaled", str(args.out / "upscaled")]
    if args.preview:
        cmd += ['--preview', str(args.preview)]
    if args.controls and args.controls.exists():
        cmd += ['--controls', str(args.controls)]
    subprocess.run(cmd, check=True)
    print(f'Fonts only: {count} assets, {time.perf_counter()-start:.2f} seconds; no GPU upscale')


if __name__ == '__main__':
    main()
