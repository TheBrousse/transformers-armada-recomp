# make_inc.py - SMAA.hlsl as C++ raw-string pieces (smaa_hlsl.inc), each under MSVC's 16 KB literal limit.
# Run after replacing SMAA.hlsl:  python make_inc.py
import os
here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, 'SMAA.hlsl'), encoding='latin-1').read().replace('\r\n', '\n')
# two middle dots in a comment diagram are the only non-ASCII bytes; keep the .inc pure ASCII
src = ''.join(c if ord(c) < 128 else '.' for c in src)
assert ')SMAA"' not in src
pieces, cur = [], ''
for line in src.splitlines(keepends=True):
    if len(cur) + len(line) > 12000:
        pieces.append(cur)
        cur = ''
    cur += line
pieces.append(cur)
with open(os.path.join(here, 'smaa_hlsl.inc'), 'w', encoding='ascii', newline='\n') as f:
    f.write('// SMAA.hlsl (MIT, see LICENSE.txt) as string pieces - made by make_inc.py, do not edit\n')
    for p in pieces:
        f.write('R"SMAA(' + p + ')SMAA",\n')
print(len(pieces), 'pieces')
