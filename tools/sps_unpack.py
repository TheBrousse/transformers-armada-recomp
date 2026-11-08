r"""Unpacks a SharkPort 2 / X-Port save (.sps, .xps: GameFAQs' PS2 saves) into a memory card
folder the runtime reads (TF_MC_ROOT, run.ps1 -CardFrom): <out>/<the save's folder>/<its files>.
The layout: a length-prefixed "SharkPortSave", a version word, the title, subtitle and comment as
length-prefixed strings, the entries' total size, then the folder's entry and each file's (a
16-bit header length; the name in 64 bytes, the size at +0x40, the mode's bytes at +0x4C, the
dates), each file's bytes after its header, and a 4-byte checksum at the end (not checked).

python tools\sps_unpack.py <file> <out>"""
import os, struct, sys
with open(sys.argv[1], 'rb') as f:
    data = f.read()
out = sys.argv[2]
at = 0
def u32():
    global at
    (v,) = struct.unpack_from('<I', data, at)
    at += 4
    return v
def string():
    global at
    n = u32()
    s = data[at:at + n]
    at += n
    return s.decode('latin1')
magic = string()
assert magic == 'SharkPortSave', magic
version = u32()
title, subtitle, comment = string(), string(), string()
total = u32()
print(f'{magic} v{version}: {title!r} / {subtitle!r} / {comment!r}, {total} bytes of entries (file {len(data)})')
def entry():
    global at
    (hlen,) = struct.unpack_from('<H', data, at)
    header = data[at + 2:at + hlen]
    at += hlen
    name = header[:64].split(b'\0')[0].decode('latin1')
    (size,) = struct.unpack_from('<I', header, 0x40)
    mode = header[0x4C:0x4E]
    return name, size, mode
folder, count, mode = entry()
print(f'folder {folder!r}: {count} entries, mode bytes {mode.hex()}')
target = os.path.join(out, folder)
os.makedirs(target, exist_ok=True)
for _ in range(count - 2):
    name, size, mode = entry()
    body = data[at:at + size]
    at += size
    with open(os.path.join(target, name), 'wb') as f:
        f.write(body)
    print(f'  {name!r}: {size} bytes, mode bytes {mode.hex()}')
print(f'end at {at} of {len(data)}; left: {data[at:].hex()}')
