#!/usr/bin/env python3
"""Build and run tests/xbox/test_net_ring.c on the host against xhw_net.c's
datagram ring (ring_init/ring_push/ring_pop), cut out of the source: one
writer and one reader, against a model and in two threads, with random
sizes and overruns."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

PRELUDE = """#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
"""


def main():
    src = open(os.path.join(ROOT, "xbox/src/hw/xhw_net.c")).read()
    a = src.index("/* ---- datagram ring")
    b = src.index("/* ---- end of the datagram ring ---- */")
    test = open(os.path.join(ROOT, "tests/xbox/test_net_ring.c")).read()
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory() as tmp:
        c = os.path.join(tmp, "t.c")
        exe = os.path.join(tmp, "test_net_ring")
        with open(c, "w") as f:
            f.write(PRELUDE + src[a:b] + test)
        r = subprocess.run([cc, "-O2", "-w", "-pthread", "-o", exe, c])
        if r.returncode:
            return r.returncode
        return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
