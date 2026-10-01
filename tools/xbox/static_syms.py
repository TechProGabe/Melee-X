#!/usr/bin/env python3
"""Static (file-local) functions for a link map: <map>.statics.

    tools/xbox/static_syms.py [--map build-xbox/melee_x.map] [--nm llvm-nm]

lld-link's map lists only public symbols, so a profile sample in a static
function was credited to the public function before it (v32's
"TObjUpdateFunc" was mostly the static code after it). Every object has a
single .text section, so a static function sits at its offset from the
object's .text start, and that start follows from any public symbol of the
same object (map address minus its offset). Run this right after the build
whose map it is (it reads build-xbox's objects); console.py stage does.
sym.load() reads the .statics file next to a map when there is one."""
import argparse
import os
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
    public = {}
    for va, name, obj in sym.load(a.map, statics=False):
        public.setdefault((name, obj.split(':')[-1].lower()), va)
    lines, missed = [], 0
    for obj in objects():
        syms = list(nm_syms(a.nm, obj))
        base = None
        for off, kind, name in syms:
            va = public.get((name, obj.name.lower()))
            if kind == 'T' and va is not None:
                base = va - off
                break
        if base is None:
            missed += 1
            continue
        lines += [f'{base + off:08x} {name} {obj.name}' for off, kind, name in syms if kind == 't']
    out = pathlib.Path(a.map + '.statics')
    out.write_text('\n'.join(sorted(lines)) + '\n')
    print(f'{out}: {len(lines)} static functions ({missed} objects without a public symbol to place them)')


if __name__ == '__main__':
    main()
