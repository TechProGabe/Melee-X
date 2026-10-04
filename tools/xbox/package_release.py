#!/usr/bin/env python3
"""Zip a release build for the Releases page.

    tools/xbox/package_release.py <name>      # e.g. rc1 -> dist/Melee-X-rc1.zip

From build-xbox/xbe (build first with no XBOX_CFLAGS: a release build has no
profiler or autopad; and with XBOX_LTO=1 XBOX_PGO=xbox/melee.profdata). The
zip holds what README.md describes:

    Melee-X/default.xbe     the game
    Melee-X/default.tbn     dashboard icon (XBMC-style dashboards)
    tools/make-xiso         packs it with your disc image into a burnable ISO
    README.md, LICENSE.md
"""
import pathlib
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    xbe = ROOT / 'build-xbox' / 'xbe'
    for need in ('default.xbe', 'default.tbn'):
        if not (xbe / need).exists():
            sys.exit(f'no {xbe / need}: build first (tools/xbox/msys/build.sh or tools/xbox/docker/build.sh)')
    cache = (ROOT / 'build-xbox' / 'CMakeCache.txt').read_bytes()

    def cached(name):
        key = name.encode() + b'='
        return cache.split(key)[1].split(b'\n')[0].strip() if key in cache else b''

    if cached('CMAKE_C_FLAGS:STRING'):
        sys.exit('this build has XBOX_CFLAGS set: rebuild without them for a release')
    # releases are ThinLTO + PGO with the committed profile (docs/toolchain.md "Release")
    if cached('XBOX_LTO:BOOL') != b'ON' or not cached('XBOX_PGO:STRING').endswith(b'melee.profdata'):
        sys.exit('a release is built with XBOX_LTO=1 XBOX_PGO=xbox/melee.profdata')
    out = ROOT / 'dist' / f'Melee-X-{sys.argv[1]}.zip'
    out.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        z.write(xbe / 'default.xbe', 'Melee-X/default.xbe')
        z.write(xbe / 'default.tbn', 'Melee-X/default.tbn')
        info = zipfile.ZipInfo('tools/make-xiso')
        info.external_attr = 0o100755 << 16   # executable when unzipped on Linux/macOS
        z.writestr(info, (ROOT / 'tools' / 'make-xiso').read_bytes(), zipfile.ZIP_DEFLATED)
        z.write(ROOT / 'README.md', 'README.md')
        z.write(ROOT / 'LICENSE.md', 'LICENSE.md')
    print(f'wrote {out} ({out.stat().st_size // 1024} KB)')


if __name__ == '__main__':
    main()
