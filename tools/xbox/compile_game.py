#!/usr/bin/env python3
"""Compile the game's C (src/melee, src/sysdolphin, src/pc game-side units) to
i386 COFF objects for default.xbe.

Per unit (docs/toolchain.md):
  1. clang -E with the Xbox game triple and DISC_STRUCT as an annotation,
  2. string literals converted to CP932 (what GCC's -fexec-charset does),
  3. tools/lower/disc_lower rewrites every DISC_STRUCT scalar access into an
     explicit big-endian load/store (clang has no scalar_storage_order),
  4. clang -c with the same triple.

The triple is i686-pc-windows-gnu -mno-ms-bitfields: GameCube/GCC bitfield
packing, 8-byte aligned long long/double, and the same call and struct-return
ABI as nxdk's i386-pc-win32, so these objects link with nxdk's lld-link.

Objects land in build-xbox/game; xbox/CMakeLists.txt links them.
  tools/xbox/compile_game.py [--jobs N] [--source src/melee/x.c ...]
"""
import argparse
import concurrent.futures
import json
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/lower'))
from execution_charset import cp932_literals  # noqa: E402

LLVM = pathlib.Path(os.environ.get('LLVM', '/opt/llvm21'))
NXDK = pathlib.Path(os.environ.get('NXDK_DIR', '/opt/nxdk'))
DISC_LOWER = pathlib.Path(os.environ.get('DISC_LOWER', '/opt/melee-tools/disc_lower'))
OUT = ROOT / os.environ.get('XBOX_GAME_OUT', 'build-xbox/game')

TRIPLE = ['--target=i686-pc-windows-gnu', '-mno-ms-bitfields', '-march=pentium3']

# nxdk-cc's system headers (pdclib), minus its i386-pc-win32 target.
SYSTEM_FLAGS = [
    '-ffreestanding', '-nostdlibinc',
    f'-isystem{NXDK}/lib/pdclib/include',
    f'-isystem{NXDK}/lib/pdclib/platform/xbox/include',
    f'-isystem{NXDK}/lib/xboxrt/libc_extensions',
    '-DNXDK', '-D__STDC__=1', '-U__STDC_NO_THREADS__',
    # The mingw triple predefines these; nothing in the game may think it is
    # running on Windows.
    '-U_WIN32', '-U__MINGW32__', '-U__MINGW64__', '-UWIN32', '-U__WIN32', '-U__WIN32__',
]

# __FILE__ (HSD's asserts) relative to the checkout: no builder's home
# directory in the XBE
PREFIX_MAP = [f'-ffile-prefix-map={ROOT.as_posix()}/=', f'-ffile-prefix-map={ROOT}{os.sep}=']

PREPROCESS_FLAGS = [
    *TRIPLE, *SYSTEM_FLAGS, *PREFIX_MAP, '-fsigned-char',
    '-DTARGET_PC=1', '-DMELEE_PC=1', '-DTARGET_XBOX=1', '-DMELEE_DISC_LOWERING=1',
    '-Ixbox/include/game', '-Iextern/aurora/include', '-Isrc', '-Isrc/sdk_include',
    '-include', 'xbox/include/game/xbox_game_prelude.h',
    '-include', 'dolphin/gx.h',
    '-include', 'src/pc/compat.h',
    '-include', 'tools/lower/disc_access.h',
    '-Wno-everything', '-ferror-limit=5',
]

# Same floating-point contract as melee-pc's melee_game (the simulation must
# round like every other build): no contraction, no fast-math. SSE for float
# so single-precision math rounds to single like Gekko instead of x87's 80 bits.
COMPILE_FLAGS = [
    *TRIPLE, '-ffreestanding', '-O2', '-fsigned-char', '-Wno-everything', '-ferror-limit=5',
    # An implicitly declared function returns int: a float result would be
    # read from EAX instead of st(0). Never silently.
    '-Werror=implicit-function-declaration', '-Werror=implicit-int',
    '-msse', '-mfpmath=sse', '-mno-stack-arg-probe',
    '-ffp-contract=off', '-fno-fast-math',
    '-fno-builtin-sinf', '-fno-builtin-cosf', '-fno-builtin-tanf', '-fno-builtin-atanf',
    '-ftrivial-auto-var-init=zero', '-fno-strict-aliasing', '-fwrapv',
    '-fno-asynchronous-unwind-tables', '-fno-exceptions',
    # Everything links statically: data goes straight to its symbol, not
    # through a mingw .refptr stub (an extra load per access).
    '-fno-auto-import',
    # Each function its own section, so the link can lay the code out by
    # profile (xbox/order.txt, tools/xbox/make_order.py).
    '-ffunction-sections',
]


# XBOX_LTO=1 (xbox/CMakeLists.txt): bitcode for ThinLTO (tools/xbox/thinlto_link.py)
if os.environ.get('XBOX_LTO'):
    COMPILE_FLAGS.append('-flto=thin')
# XBOX_PGO=gen: instrumented (xbox/src/hw/xhw_pgo.c); XBOX_PGO=<.profdata>: optimized
# with it. Static functions are named by file name only, not the lowered
# file's path, which differs between the two builds' object folders.
PGO = os.environ.get('XBOX_PGO', '')
PGO_FLAGS = {'': [], 'gen': ['-fprofile-generate', '-mllvm', '-disable-vp']}.get(PGO, [f'-fprofile-use={PGO}'])
if PGO:
    PGO_FLAGS += ['-mllvm', '-static-func-full-module-prefix=false']
COMPILE_FLAGS += PGO_FLAGS


def game_sources():
    # sorted case-sensitively, as on Linux (link order = code layout)
    posix = lambda p: p.as_posix()
    sources = sorted([*(ROOT / 'src/melee').rglob('*.c'), *(ROOT / 'src/sysdolphin').rglob('*.c')], key=posix)
    sources += [ROOT / 'src/pc' / n for n in ('vtxarray.c', 'widescreen.c', 'region.c', 'discfont.c')]
    sources += sorted((ROOT / 'src/pc/libm').glob('pc_*.c'), key=posix)
    # PowerPC/MetroTRK debugger integration and the netplay determinism probe.
    skip = {'debugconsole_main.c', 'pc_perturb.c'}
    return [s for s in sources if s.name not in skip]


def up_to_date(obj, dep, source):
    """The object is newer than the source, every header it included (from
    the preprocessor's depfile), and this script and the lowering tool."""
    if not obj.exists() or not dep.exists():
        return False
    stamp = obj.stat().st_mtime
    # ': ' ends the target (a Windows target has a drive colon of its own)
    deps = dep.read_text().replace('\\\n', ' ').split(': ', 1)[-1].split()
    for d in [str(source), __file__, str(DISC_LOWER), *deps, *([PGO] if PGO not in ('', 'gen') else [])]:
        path = pathlib.Path(d) if os.path.isabs(d) else ROOT / d
        try:
            if path.stat().st_mtime > stamp:
                return False
        except FileNotFoundError:
            return False
    return True


def build(source, cmd_log):
    rel = source.relative_to(ROOT)
    base = OUT / rel
    base.parent.mkdir(parents=True, exist_ok=True)
    preprocessed, lowered = base.with_suffix('.i'), base.with_suffix('.lowered.c')
    obj, log, dep = base.with_suffix('.obj'), base.with_suffix('.log'), base.with_suffix('.d')
    if up_to_date(obj, dep, source) and not os.environ.get('XBOX_FORCE'):
        return {'source': str(rel), 'status': 'passed', 'cached': True}

    def run(stage, cmd, stdout):
        if subprocess.run(list(map(str, cmd)), cwd=ROOT, stdout=stdout, stderr=err).returncode:
            return {'source': str(rel), 'stage': stage, 'status': 'failed', 'log': str(log.relative_to(ROOT))}
        return None

    with log.open('w') as err:
        failed = run('preprocess', [LLVM / 'bin/clang', *PREPROCESS_FLAGS, '-E', '-MD', '-MF', dep, '-MT', obj,
                                    source, '-o', preprocessed], err)
        if failed:
            return failed
        preprocessed.write_text(cp932_literals(preprocessed.read_text(errors='surrogateescape')),
                                errors='surrogateescape')
        with lowered.open('w') as out:
            failed = run('lower', [DISC_LOWER, preprocessed, *TRIPLE], out)
        if failed:
            return failed
        failed = run('compile', [LLVM / 'bin/clang', *COMPILE_FLAGS, '-x', 'c', '-c', lowered, '-o', obj], err)
        if not failed and not os.environ.get('XBOX_KEEP_TEMPS'):
            preprocessed.unlink()
            lowered.unlink()
        return failed or {'source': str(rel), 'status': 'passed'}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--jobs', type=int, default=os.cpu_count())
    parser.add_argument('--source', action='append', help='compile only this unit (repeatable)')
    args = parser.parse_args()

    sources = [ROOT / s for s in args.source] if args.source else game_sources()
    OUT.mkdir(parents=True, exist_ok=True)
    if not args.source:
        expected = {OUT / s.relative_to(ROOT).with_suffix('.obj') for s in sources}
        for stale in OUT.rglob('*.obj'):
            if stale not in expected:
                stale.unlink()

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for result in pool.map(lambda s: build(s, None), sources):
            results.append(result)
            if result['status'] == 'failed':
                print(f"{result['source']}: FAILED at {result['stage']} (see {result['log']})", flush=True)
            elif len(results) % 100 == 0:
                print(len(results), 'units', flush=True)

    report = OUT / ('report-partial.json' if args.source else 'report.json')
    report.write_text(json.dumps(results, indent=2) + '\n')
    if not args.source:
        listing = ''.join(str((OUT / r['source']).with_suffix('.obj')) + '\n' for r in results if r['status'] == 'passed')
        objects = OUT / 'objects.txt'
        changed = not objects.exists() or objects.read_text() != listing
        if changed:
            objects.write_text(listing)
        # the link depends on this stamp (xbox/CMakeLists.txt LINK_DEPENDS)
        if changed or any(r['status'] == 'passed' and not r.get('cached') for r in results):
            (OUT / 'objects.stamp').touch()
    passed = sum(r['status'] == 'passed' for r in results)
    print(f'{passed} / {len(results)} compiled')
    raise SystemExit(passed != len(results))


if __name__ == '__main__':
    main()
