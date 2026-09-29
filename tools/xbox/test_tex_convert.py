#!/usr/bin/env python3
"""Build and run tests/xbox/test_tex_convert.c on the host: gx_tex.c's
native texture formats (DXT1, AY8, A8Y8, RGB565) against its ARGB decoder."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_tex_convert")
        # the test #includes gx_tex.c to reach its static functions
        wrapper = os.path.join(tmp, "t.c")
        with open(wrapper, "w") as f:
            f.write('#include "%s"\n#include "%s"\n' % (
                os.path.join(ROOT, "xbox/src/sdk/gx/gx_tex.c"),
                os.path.join(ROOT, "tests/xbox/test_tex_convert.c")))
        cmd = [cc, "-O1", "-w", "-DTARGET_PC=1", "-DMELEE_PC=1", "-o", exe, wrapper,
               "-I" + os.path.join(ROOT, "xbox/include"),
               "-I" + os.path.join(ROOT, "extern/aurora/include"),
               "-I" + os.path.join(ROOT, "src"),
               "-I" + os.path.join(ROOT, "src/sdk_include"),
               "-I" + os.path.join(ROOT, "xbox/src/sdk")]
        r = subprocess.run(cmd)
        if r.returncode:
            return r.returncode
        return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
