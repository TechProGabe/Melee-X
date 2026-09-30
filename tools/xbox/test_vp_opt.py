#!/usr/bin/env python3
"""Build and run tests/xbox/test_vp_opt.c on the host: the vertex programs
xbox/src/hw/nv2a_vp.c generates and optimizes, against the generator as it
was before the optimizer (tests/xbox/vp_ref.c), through an interpreter of the
NV2A instruction words, for random VpKeys. Every output lane must have the
same bits. An optional argument sets the number of keys (default 200000)."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_vp_opt")
        cmd = [cc, "-O2", "-Wall", "-ffp-contract=off", "-DVP_HOST_TEST", "-o", exe,
               os.path.join(ROOT, "tests/xbox/test_vp_opt.c"),
               os.path.join(ROOT, "tests/xbox/vp_ref.c"),
               os.path.join(ROOT, "xbox/src/hw/nv2a_vp.c"),
               "-I" + os.path.join(ROOT, "xbox/src/hw"), "-lm"]
        r = subprocess.run(cmd)
        if r.returncode:
            return r.returncode
        return subprocess.run([exe] + sys.argv[1:]).returncode


if __name__ == "__main__":
    sys.exit(main())
