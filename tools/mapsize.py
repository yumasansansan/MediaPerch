#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Attribute the bytes of a linked binary to the objects they came from.

"The binary got bigger" is not a finding until it says which part got bigger.
A linker map lists every public symbol with its address and the object file it
arrived in; sorting by address and taking the distance to the next symbol gives
each one an approximate size, and summing those by object gives the answer.

The sizes are approximate on purpose and in a knowable direction: a symbol's
span includes any static functions and padding that follow it, so the total per
object is close and the individual figures are not. That is the right accuracy
for the question, which is "what should I look at first".

Configure with -D MEDIAPERCH_LINK_MAP=ON to get the maps, then:

    python tools/mapsize.py build/vs/bin/Release/*.map
    python tools/mapsize.py --symbols build/vs/bin/Release/mediaperch-probe.map
"""
import argparse
import os
import re
import sys

# " 0001:00000000       ?name@@YAXXZ    0000000140001000     f i lib:object"
#
# Split rather than matched against fixed columns: the flag field between the
# address and the origin is empty, "f", "i" or "f i" depending on the symbol,
# and a regex that assumes one of those quietly attributes half the binary to a
# symbol called "i".
LINE = re.compile(
    r'^\s+([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+(\S+)\s+([0-9a-fA-F]{16})\s+(.*)$')

# " 0003:00000160 0002ef68H .bss   DATA"
SECTION = re.compile(
    r'^\s+([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+([0-9a-fA-F]+)H\s+(\S+)\s+(\S+)\s*$')

# Sections that take up room in the image and none in the file. `.bss` is the
# whole of it in practice, and leaving it in charged 188 KB of zeroed lookup
# tables to a 78 KB DLL -- the AAC decoder's cosine tables, which are computed
# at init and therefore stored nowhere.
NOT_IN_THE_FILE = ('.bss',)


def read_map(path):
    """Every symbol the map lists, as (rva, section, name, origin).

    Symbols in sections that occupy no file bytes are left out, so the total can
    be compared with the size on disk and mean something.
    """
    out = []
    empty = []          # (section, first offset, last offset) of .bss and friends
    started = False
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip()
            if 'Publics by Value' in line or 'Static symbols' in line:
                # Two tables of the same shape. The second one is where the
                # static functions are, which are exactly the ones a
                # publics-only view would miss.
                started = True
                continue
            if not started:
                m = SECTION.match(line)
                if m and m.group(4) in NOT_IN_THE_FILE:
                    section = int(m.group(1), 16)
                    at = int(m.group(2), 16)
                    empty.append((section, at, at + int(m.group(3), 16)))
                continue
            m = LINE.match(line)
            if not m:
                continue
            section = int(m.group(1), 16)
            offset = int(m.group(2), 16)
            name, rva, rest = m.group(3), int(m.group(4), 16), m.group(5).split()
            if rva == 0 or not rest:
                continue  # absolutes: no address, no size, no object
            origin = rest[-1]
            if origin in ('<absolute>', '<linker-defined>', '<common>'):
                continue
            if any(s == section and lo <= offset < hi for s, lo, hi in empty):
                continue
            out.append((rva, section, name, origin))
    return out


def sizes(symbols):
    """Approximate size per symbol, by distance to the next address.

    Only within one section. The distance from the last symbol of `.text` to the
    first of `.rdata` is the alignment padding between them, and charging it to
    whichever function happened to be last put a hundred kilobytes of nothing
    against `aac.obj` the first time this ran.
    """
    ordered = sorted(symbols)
    out = []
    for i, (rva, section, name, origin) in enumerate(ordered):
        span = 0
        if i + 1 < len(ordered) and ordered[i + 1][1] == section:
            span = max(0, ordered[i + 1][0] - rva)
        out.append((span, name, origin))
    return out


def group(rows):
    total = {}
    for span, _name, origin in rows:
        total[origin] = total.get(origin, 0) + span
    return total


def human(n):
    return '%8.1f KB' % (n / 1024.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('maps', nargs='+')
    ap.add_argument('--symbols', action='store_true',
                    help='list the largest individual symbols as well')
    ap.add_argument('--top', type=int, default=15)
    args = ap.parse_args()

    for path in args.maps:
        symbols = read_map(path)
        if not symbols:
            print('%s: no symbol table -- was it linked with /MAP?' % path)
            continue
        rows = sizes(symbols)
        by_object = group(rows)
        counted = sum(by_object.values())
        on_disk = os.path.getsize(os.path.splitext(path)[0] + '.exe') if os.path.exists(
            os.path.splitext(path)[0] + '.exe') else None
        if on_disk is None:
            dll = os.path.splitext(path)[0] + '.dll'
            on_disk = os.path.getsize(dll) if os.path.exists(dll) else 0

        print('=== %s' % os.path.basename(path))
        print('    %s on disk, %s attributed to %d objects'
              % (human(on_disk), human(counted), len(by_object)))
        for origin, n in sorted(by_object.items(), key=lambda kv: -kv[1])[:args.top]:
            print('    %s  %5.1f%%  %s' % (human(n), 100.0 * n / max(counted, 1), origin))
        if args.symbols:
            print('    --- largest symbols')
            for span, name, origin in sorted(rows, key=lambda r: -r[0])[:args.top]:
                print('    %s  %-46s %s' % (human(span), name[:46], origin))
        print()


if __name__ == '__main__':
    sys.exit(main())
