#!/usr/bin/env python3
"""Build and run tests/xbox/test_pool.c on the host against nv2a.c's pool
allocator (pool_init/pool_alloc/pool_free), cut out of the source."""
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
#define MAXRAM 0
#define PAGE_READWRITE 0
#define PAGE_WRITECOMBINE 0
static void* MmAllocateContiguousMemoryEx(uint32_t n, int a, int b, int c, int d) { return malloc(n); }
static void MmFreeContiguousMemory(void* p) { free(p); }
"""


def main():
    src = open(os.path.join(ROOT, "xbox/src/hw/nv2a.c")).read()
    a = src.index("#define POOL_ALIGN")
    b = src.index("uint32_t xgx_tex_pool_free_kb(void)")
    test = open(os.path.join(ROOT, "tests/xbox/test_pool.c")).read()
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory() as tmp:
        c = os.path.join(tmp, "t.c")
        exe = os.path.join(tmp, "test_pool")
        with open(c, "w") as f:
            f.write(PRELUDE + src[a:b] + test)
        r = subprocess.run([cc, "-O2", "-w", "-o", exe, c])
        if r.returncode:
            return r.returncode
        return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
