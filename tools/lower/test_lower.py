#!/usr/bin/env python3
"""Check disc_lower against GCC's scalar_storage_order, for the Xbox triple.

Every tests/lower/disc_*.c prints values and raw bytes. A native GCC build is
the oracle. Each test is then lowered with the triple the game is compiled
for on the Xbox (i686-pc-windows-gnu -mno-ms-bitfields: GameCube bitfield
packing, 8-byte long long, MSVC-compatible calls) and the lowered C must:

  1. compile for that triple (the object that goes into default.xbe), and
  2. print the same values and bytes when built and run on the host.

Usage: tools/lower/test_lower.py   (LLVM=/opt/llvm21 DISC_LOWER=... to override)
"""
import os
import pathlib
import shutil
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from execution_charset import cp932_literals  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
LLVM = pathlib.Path(os.environ.get('LLVM', '/opt/llvm21'))
DISC_LOWER = pathlib.Path(os.environ.get('DISC_LOWER', '/opt/melee-tools/disc_lower'))
OUT = ROOT / 'build-xbox' / 'lower-tests'
XBOX_TRIPLE = ['--target=i686-pc-windows-gnu', '-mno-ms-bitfields']
GCC = next((g for g in map(shutil.which, ('gcc-15', 'gcc-14', 'gcc-13', 'gcc-12', 'gcc')) if g), None)


def run(cmd):
    return subprocess.run(list(map(str, cmd)), cwd=ROOT, check=True, capture_output=True).stdout


def main():
    if not GCC:
        raise SystemExit('GCC 12+ is required as the scalar_storage_order oracle.')
    OUT.mkdir(parents=True, exist_ok=True)
    for source in sorted((ROOT / 'tests/lower').glob('disc_*.c')):
        binary = OUT / source.stem
        run([GCC, '-w', '-fexec-charset=CP932', source, '-o', binary])
        expected = run([binary])
        preprocessed = OUT / f'{source.stem}.i'
        lowered = OUT / f'{source.stem}.lowered.c'
        preprocessed.write_bytes(run([LLVM / 'bin/clang', '-E', '-include', 'tools/lower/disc_access.h',
                                      '-DMELEE_DISC_LOWERING', source]))
        preprocessed.write_text(cp932_literals(preprocessed.read_text()))
        lowered.write_bytes(run([DISC_LOWER, preprocessed, *XBOX_TRIPLE]))
        run([LLVM / 'bin/clang', *XBOX_TRIPLE, '-O2', '-w', '-c', lowered, '-o', OUT / f'{source.stem}.obj'])
        run([LLVM / 'bin/clang', '-O2', '-w', lowered, '-o', binary])
        actual = run([binary])
        if actual != expected:
            raise SystemExit(f'{source.stem}: lowered output differs\n  gcc:     {expected!r}\n  lowered: {actual!r}')
        print(source.stem, 'matches GCC values and bytes', flush=True)


if __name__ == '__main__':
    main()
