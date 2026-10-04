#!/usr/bin/env python3
"""Build and run tests/xbox/test_tex_cache.c on the host, twice: around
gx_tex.c and around tests/xbox/gx_tex_ref.c (the texture cache before its
hot/cold entry split and the leaner bind path). Both runs drive the same
seeded sequence of binds, EFB copies, palette and texel changes, scene
changes and frame ends, and print checkpoints of a digest of everything the
rest of the port sees (back-end creates with their texels, destroys, flushes,
log lines, the texture maps and dirty bits after every call, the cache's
logical contents at every frame end). They must print the same. CC and
CFLAGS are honoured, e.g. the Xbox's 32-bit layout (Entry is one 32-byte
line there) on Windows with MSYS2:
  CC="clang --target=i686-w64-mingw32 --sysroot=C:/msys64/mingw32" tools/xbox/test_tex_cache.py"""
import os
import shlex
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FLAGS = ["-O2", "-w", "-fno-strict-aliasing", "-fwrapv", "-DTARGET_PC=1", "-DMELEE_PC=1"]


def build_and_run(tmp, name, source, defines):
    cc = shlex.split(os.environ.get("CC", "cc"))
    exe = os.path.join(tmp, name)
    # the test #includes the file under test to reach its static state
    wrapper = os.path.join(tmp, name + ".c")
    with open(wrapper, "w") as f:
        f.write('#include "%s"\n#include "%s"\n' % (
            os.path.join(ROOT, source), os.path.join(ROOT, "tests/xbox/test_tex_cache.c")))
    cmd = [*cc, *FLAGS, *defines, *shlex.split(os.environ.get("CFLAGS", "")), "-o", exe, wrapper,
           "-I" + os.path.join(ROOT, "xbox/include"),
           "-I" + os.path.join(ROOT, "extern/aurora/include"),
           "-I" + os.path.join(ROOT, "src"),
           "-I" + os.path.join(ROOT, "src/sdk_include"),
           "-I" + os.path.join(ROOT, "xbox/src/sdk"),
           "-I" + os.path.join(ROOT, "xbox/src/sdk/gx")]
    r = subprocess.run(cmd)
    if r.returncode:
        sys.exit(r.returncode)
    r = subprocess.run([exe], stdout=subprocess.PIPE, text=True)
    if r.returncode:
        sys.exit(f"{name} exited with {r.returncode}")
    return r.stdout.splitlines()


def main():
    with tempfile.TemporaryDirectory() as tmp:
        ref = build_and_run(tmp, "tex_cache_ref", "tests/xbox/gx_tex_ref.c", ["-DTEX_REF=1"])
        new = build_and_run(tmp, "tex_cache_new", "xbox/src/sdk/gx/gx_tex.c", [])
    for a, b in zip(ref, new):
        if a != b:
            print(f"FAIL: reference {a!r}, now {b!r}")
            return 1
    if len(ref) != len(new) or not ref:
        print(f"FAIL: {len(ref)} lines from the reference, {len(new)} now")
        return 1
    print(ref[-3])
    print(ref[-2])
    print(f"ok: the same trace as the reference ({ref[-1]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
