#!/usr/bin/env python3
"""Build and run tests/xbox/test_dl_cull.c on the host: gx_cull.h, the
display-list box test of gx_dl_culled with the clip planes kept per
projection, against tests/xbox/dl_cull_ref.c (the planes made for every box),
on the game's floating-point flags. CC and CFLAGS are honoured; the Xbox's
code generation (i686, scalar SSE floats) on Windows with MSYS2:
  CC="clang --target=i686-w64-mingw32 --sysroot=C:/msys64/mingw32" CFLAGS="-march=pentium3" tools/xbox/test_dl_cull.py"""
import os
import shlex
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# the SDK's floating-point contract (xbox/CMakeLists.txt)
FLAGS = ["-O2", "-w", "-ffp-contract=off", "-fno-fast-math", "-fno-strict-aliasing", "-fwrapv"]


def main():
    cc = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_dl_cull")
        cmd = [*cc, *FLAGS, *shlex.split(os.environ.get("CFLAGS", "")), "-o", exe,
               os.path.join(ROOT, "tests/xbox/test_dl_cull.c"), os.path.join(ROOT, "tests/xbox/dl_cull_ref.c"),
               "-I" + os.path.join(ROOT, "xbox/src/sdk/gx"), "-lm"]
        r = subprocess.run(cmd)
        if r.returncode:
            return r.returncode
        return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
