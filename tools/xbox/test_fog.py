#!/usr/bin/env python3
"""Build and run tests/xbox/test_fog.c on the host: GX fog as the back end
maps it onto the NV2A (xbox/src/hw/nv2a_fog.c) against a reference GX fog
factor, for every fog type over a range of depths."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_fog")
        # the test #includes nv2a_fog.c, as test_tex_convert.c does gx_tex.c
        wrapper = os.path.join(tmp, "t.c")
        with open(wrapper, "w") as f:
            f.write('#include "%s"\n#include "%s"\n' % (
                os.path.join(ROOT, "xbox/src/hw/nv2a_fog.c"),
                os.path.join(ROOT, "tests/xbox/test_fog.c")))
        cmd = [cc, "-O1", "-Wall", "-o", exe, wrapper,
               "-I" + os.path.join(ROOT, "xbox/src/hw"), "-lm"]
        r = subprocess.run(cmd)
        if r.returncode:
            return r.returncode
        return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
