#!/usr/bin/env python3
"""Build and run tests/xbox/test_net_gov.c on the host against xhw_net.c's
network conduct (conduct_rx/conduct_tx, the governor gov_take and the
broadcast window gov_bcast), cut out of the source: docs/lan-plan.md D14
rules 7-10 against a model."""
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
    a = src.index("/* ---- conduct: filters and the governor")
    b = src.index("/* ---- end of conduct ---- */")
    test = open(os.path.join(ROOT, "tests/xbox/test_net_gov.c")).read()
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory() as tmp:
        c = os.path.join(tmp, "t.c")
        exe = os.path.join(tmp, "test_net_gov")
        with open(c, "w") as f:
            f.write(PRELUDE + src[a:b] + test)
        r = subprocess.run([cc, "-O2", "-w", "-o", exe, c])
        if r.returncode:
            return r.returncode
        return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
