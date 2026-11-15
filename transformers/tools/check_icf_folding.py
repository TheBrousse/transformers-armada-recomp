"""Checks the game's linker map for guest functions folded into one body (linkflags, the -DTF_LINK_MAP=ON check).

The game is linked with /OPT:NOICF on purpose (transformers/CMakeLists.txt): identical-code folding (/OPT:ICF, which
/OPT:REF turns on by itself) gives several generated guest functions one host body, and code here compares function
pointers (an override checking that a slot still holds its own wrapper or the generated original). This reads a map
the linker wrote (/MAP) and fails when a guest address our code names shares its body with another function - for a
later change that turns folding on again.

    python check_icf_folding.py <TransformersRecomp.map> [<repository root>]

The guest addresses our code names: every 0x00XXXXXX constant in transformers/src, transformers/decomp and
overlay/src. The generated functions are sub_XXXXXXXX_0x... in the map. Exit 1 on a folded named address, else 0.
"""
import collections
import os
import re
import sys

MAP_LINE = re.compile(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s+f\b')
GUEST = re.compile(r'sub_([0-9A-Fa-f]{8})_0x')
CONSTANT = re.compile(r'0x00([1-5][0-9A-Fa-f]{5})')


def named_addresses(root):
    found = set()
    for sub in ('transformers/src', 'transformers/decomp', 'overlay/src'):
        top = os.path.join(root, sub)
        for folder, _, files in os.walk(top):
            for name in files:
                if not name.endswith(('.cpp', '.h', '.hpp', '.inl')):
                    continue
                with open(os.path.join(folder, name), encoding='utf-8', errors='replace') as f:
                    for m in CONSTANT.finditer(f.read()):
                        found.add('00' + m.group(1).upper())
    return found


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    root = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
    groups = collections.defaultdict(list)
    with open(sys.argv[1], encoding='latin-1') as f:
        for line in f:
            m = MAP_LINE.match(line)
            if m:
                groups[m.group(2)].append(m.group(1))
    named = named_addresses(root)
    shared = {a: s for a, s in groups.items() if len(s) > 1}
    guest_groups = 0
    bad = []
    for address, symbols in shared.items():
        guests = {g.group(1).upper() for g in (GUEST.search(s) for s in symbols) if g}
        if len(guests) >= 2:
            guest_groups += 1
        hit = guests & named
        if hit:
            bad.append((address, sorted(hit), len(symbols)))
    print('[icf] %d function symbols at %d addresses; %d addresses hold 2+ symbols, %d of them 2+ guest functions; '
          '%d guest addresses named by our code' % (sum(len(s) for s in groups.values()), len(groups), len(shared),
                                                   guest_groups, len(named)))
    for address, hit, count in bad[:20]:
        print('[icf] FOLDED: %s shares its body (%s) with %d other symbols' % (', '.join(hit), address, count - 1))
    if bad:
        print('[icf] %d named guest address(es) folded: link the game with /OPT:NOICF (transformers/CMakeLists.txt)' % len(bad))
        return 1
    print('[icf] no named guest address shares its body')
    return 0


if __name__ == '__main__':
    sys.exit(main())
