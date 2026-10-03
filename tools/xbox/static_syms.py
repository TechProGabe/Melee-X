#!/usr/bin/env python3
"""Check a link map against the objects' function symbols: <map>.statics.

    tools/xbox/static_syms.py [--map build-xbox/melee_x.map] [--nm llvm-nm]

lld-link's map lists the public symbols and, after them, a "Static symbols"
section with the file-local ones; sym.load() reads both. This lists every
function symbol (T and t) of the build's own objects (game, sdk, hw, pbkit)
and checks that the map has it at the right address: an object's single
.text section starts at any of its mapped symbols minus that symbol's offset.
Functions the map lacks (none so far, on lld 21) go to <map>.statics, which
sym.load() also reads. Profile samples that land on the wrong neighbour come
from 64-byte buckets shared by two functions, not from missing symbols:
prof_report.py shares those out.

Run it right after the build whose map it is (it reads build-xbox's
objects); console.py stage does."""
import argparse
import collections
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import sym  # noqa: E402


def objects():
    build = ROOT / 'build-xbox'
    listing = build / 'game' / 'objects.txt'
    objs = [pathlib.Path(l.strip()) for l in listing.read_text().splitlines() if l.strip()] if listing.exists() else []
    for d in ('mx_sdk.dir', 'mx_hw.dir', 'mx_pbkit.dir'):
        objs += sorted((build / 'CMakeFiles' / d).rglob('*.obj'))
    return objs


def nm_syms(nm, obj):
    out = subprocess.run([nm, '--defined-only', str(obj)], capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.match(r'^([0-9a-f]{8}) ([Tt]) (\S+)$', line)
        if m:
            yield int(m[1], 16), m[2], m[3]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--map', default=str(ROOT / 'build-xbox' / 'melee_x.map'))
    ap.add_argument('--nm', default=shutil.which('llvm-nm') or 'C:/msys64/mingw64/bin/llvm-nm.exe')
    a = ap.parse_args()
    mapped = collections.defaultdict(dict)   # object file name -> {symbol: address}, publics and statics
    for va, name, obj in sym.load(a.map, statics=False):
        mapped[obj.split(':')[-1].lower()].setdefault(name, va)
    lines, unplaced, nfun, moved = [], 0, 0, 0
    for obj in objects():
        syms = list(nm_syms(a.nm, obj))
        nfun += len(syms)
        known = mapped.get(obj.name.lower(), {})
        base = next((known[name] - off for off, kind, name in syms if name in known), None)
        if base is None:
            unplaced += bool(syms)
            continue
        for off, kind, name in syms:
            if name not in known:
                lines.append(f'{base + off:08x} {name} {obj.name}')
            elif known[name] != base + off:
                moved += 1
    out = pathlib.Path(a.map + '.statics')
    out.write_text(''.join(l + '\n' for l in sorted(lines)))
    print(f'{nfun} functions in the objects: {len(lines)} missing from the map (-> {out.name}), {moved} at another '
          f'address than the map says, {unplaced} objects not in the map '
          f'(archive members the link left out)')


if __name__ == '__main__':
    main()
