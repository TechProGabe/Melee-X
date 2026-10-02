#!/usr/bin/env python3
"""Build and run tests/xbox/test_rc.c on the host: the TEV -> register
combiner compiler (xbox/src/hw/nv2a_rc.c) with swap tables, against the
compiler before them (tests/xbox/rc_ref.c) and a model of the combiners.
Usage: test_rc.py [configurations per test, default 200000]"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_rc")
        # the test #includes nv2a_rc.c, as test_fog.c does nv2a_fog.c; the
        # reference compiler is a translation unit of its own (same statics)
        wrapper = os.path.join(tmp, "t.c")
        with open(wrapper, "w") as f:
            f.write('#include "%s"\n#include "%s"\n' % (
                os.path.join(ROOT, "xbox/src/hw/nv2a_rc.c"),
                os.path.join(ROOT, "tests/xbox/test_rc.c")))
        cmd = [cc, "-O1", "-Wall", "-o", exe, wrapper, os.path.join(ROOT, "tests/xbox/rc_ref.c"),
               "-I" + os.path.join(ROOT, "xbox/src/hw"), "-I" + os.path.join(ROOT, "tests/xbox"), "-lm"]
        r = subprocess.run(cmd)
        if r.returncode:
            return r.returncode
        return subprocess.run([exe] + sys.argv[1:]).returncode


if __name__ == "__main__":
    sys.exit(main())
