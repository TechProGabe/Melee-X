#!/usr/bin/env python3
"""Symbolize addresses from crash.log with the link map.

    tools/xbox/sym.py crash.log                 # every [CRASH] eip/stack address
    tools/xbox/sym.py 0036c4f0 0014b6c0 ...     # given addresses

The map is build-xbox/melee_x.map (override with --map). It must come from
the same build as the default.xbe that crashed: CI uploads both together.
cxbe keeps the link base (0x10000), so the map's Rva+Base column is the
address the Xbox reports."""
import argparse
import bisect
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LINE = re.compile(r'^\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s+(?:f\s+)?(?:i\s+)?(\S+)\s*$')


def load(path, statics=True):
    """The map's symbols, public and static (lld-link lists the file-local
    ones after the publics, in the same format), plus any functions
    tools/xbox/static_syms.py found missing from it (<map>.statics)."""
    syms = []
    with open(path, errors='replace') as f:
        for line in f:
            m = LINE.match(line)
            if m:
                syms.append((int(m.group(2), 16), m.group(1), m.group(3)))
    if statics and os.path.isfile(path + '.statics'):
        with open(path + '.statics') as f:
            for line in f:
                parts = line.split()
                if len(parts) == 3:
                    syms.append((int(parts[0], 16), parts[1], parts[2]))
    syms.sort()
    return syms


def lookup(syms, keys, addr):
    i = bisect.bisect_right(keys, addr) - 1
    if i < 0:
        return '?'
    va, name, obj = syms[i]
    return f'{name}+0x{addr - va:x} ({obj})'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--map', default=os.path.join(ROOT, 'build-xbox', 'melee_x.map'))
    ap.add_argument('inputs', nargs='+', help='crash.log / boot.log, or hex addresses')
    a = ap.parse_args()
    syms = load(a.map)
    if not syms:
        sys.exit(f'no symbols in {a.map}')
    keys = [s[0] for s in syms]
    addrs = []
    for arg in a.inputs:
        if os.path.isfile(arg):
            with open(arg, errors='replace') as f:
                for line in f:
                    if not line.startswith('[CRASH]'):
                        continue
                    m = re.search(r'\bat ([0-9a-f]{8})', line)
                    if m:
                        addrs.append(('fault', int(m.group(1), 16)))
                    if line.startswith('[CRASH] stack:'):
                        addrs += [('stack', int(x, 16)) for x in line.split(':', 1)[1].split()]
        else:
            addrs.append(('addr', int(arg, 16)))
    for kind, addr in addrs:
        print(f'{kind:5} {addr:08x}  {lookup(syms, keys, addr)}')


if __name__ == '__main__':
    main()
