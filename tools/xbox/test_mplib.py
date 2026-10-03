#!/usr/bin/env python3
"""Build and run tests/xbox/test_mplib.c on the host: the stage-collision line
tests of src/melee/mp/mplib.c (mpCheckFloor, the wall checks, their Remap
variants, mpLib_800511A4_RightWall and mpLib_800515A0_LeftWall) with the
per-line rejects against the code before them (tests/xbox/mplib_ref.c), bit
for bit, on random and hand-made stages and queries.
  tools/xbox/test_mplib.py [rounds]
The functions are cut out of mplib.c by name (the file as a whole needs the
rest of the game). CC and CFLAGS are honoured, e.g. a 32-bit x87 build:
  CC=i686-w64-mingw32-gcc CFLAGS="-march=pentium3 -mfpmath=sse" tools/xbox/test_mplib.py"""
import os
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# the game's floating-point contract (tools/xbox/compile_game.py), and the
# Xbox paths of the code under test (TARGET_XBOX)
FLAGS = ["-O2", "-w", "-ffp-contract=off", "-fno-fast-math", "-fno-strict-aliasing", "-fwrapv",
         "-ftrivial-auto-var-init=zero", "-DTARGET_PC=1", "-DMELEE_PC=1", "-DTARGET_XBOX=1"]

# what the tests reach, in mplib.c's order (helpers the rejects add are
# picked up by the PORT_HELPERS pattern)
FUNCS = ["mpLineGetNext", "mpLineGetPrev", "mpRemap2d", "mpLineIntersection",
         "mpLineIntersectionH", "mpLineGetCollLine", "mpLib_8004ED5C", "mpCheckFloor",
         "mpCheckFloorRemap", "mpLineIntersectionV", "mpCheckLeftWall", "mpCheckLeftWallRemap",
         "mpCheckRightWall", "mpCheckRightWallRemap", "mpLib_800511A4_RightWall",
         "mpLib_800515A0_LeftWall", "mpCheckedBounding", "mpBoundingCheck", "mpBoundingCheck2",
         "mpBoundingCheck3", "mpUncheckBounding"]
PORT_HELPERS = re.compile(r"^mp(LineBox|EdgeBox|RemapReach)\w*$")

HEAD = re.compile(r"^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w\s\*]*?\b(\w+)\(")
STRUCT = re.compile(r"^typedef struct (\w+) \{$")


def extract(text, wanted):
    """(structs, prototypes, bodies) of the top-level definitions of text whose
    names pass wanted(name), in file order."""
    lines = text.split("\n")
    structs, protos, bodies = [], [], []
    i = 0
    while i < len(lines):
        m = STRUCT.match(lines[i])
        if m and wanted(m.group(1)):
            j = i
            while not lines[j].startswith("}"):
                j += 1
            structs.append("\n".join(lines[i:j + 1]))
            i = j + 1
            continue
        m = HEAD.match(lines[i])
        if not m or lines[i].rstrip().endswith(";") or not wanted(m.group(1)):
            i += 1
            continue
        j = i
        while lines[j] != "{":
            if lines[j].rstrip().endswith(";"):
                break
            j += 1
        if lines[j] != "{":
            i = j + 1
            continue
        depth, k = 0, j
        while True:
            depth += lines[k].count("{") - lines[k].count("}")
            if depth == 0:
                break
            k += 1
        protos.append("\n".join(lines[i:j]) + ";")
        bodies.append("\n".join(lines[i:k + 1]))
        i = k + 1
    return structs, protos, bodies


def wanted(name):
    return name in FUNCS or bool(PORT_HELPERS.match(name))


def generate(text, banner):
    structs, protos, bodies = extract(text, wanted)
    found = {HEAD.match(b).group(1) for b in bodies}
    missing = [f for f in FUNCS if f not in found]
    if missing:
        sys.exit("test_mplib: not found in mplib.c: " + ", ".join(missing))
    return "\n\n".join([banner, *structs, "\n".join(protos), *bodies]) + "\n"


def main():
    cc = shlex.split(os.environ.get("CC", "cc"))
    with open(os.path.join(ROOT, "src/melee/mp/mplib.c"), encoding="utf-8") as f:
        cur = generate(f.read(), "/* cut out of src/melee/mp/mplib.c by tools/xbox/test_mplib.py */")
    with tempfile.TemporaryDirectory() as tmp:
        with open(os.path.join(tmp, "mplib_cur.c"), "w", encoding="utf-8") as f:
            f.write(cur)
        exe = os.path.join(tmp, "test_mplib")
        cmd = [*cc, *FLAGS, *shlex.split(os.environ.get("CFLAGS", "")), "-o", exe,
               os.path.join(ROOT, "tests/xbox/test_mplib.c"),
               os.path.join(ROOT, "extern/aurora/lib/dolphin/mtx/vec.c"),
               "-include", os.path.join(ROOT, "src/pc/compat.h"),
               "-I" + tmp,
               "-I" + os.path.join(ROOT, "tests/xbox"),
               "-I" + os.path.join(ROOT, "xbox/include"),
               "-I" + os.path.join(ROOT, "extern/aurora/include"),
               "-I" + os.path.join(ROOT, "src"),
               "-I" + os.path.join(ROOT, "src/sdk_include"), "-lm"]
        r = subprocess.run(cmd)
        if r.returncode:
            return r.returncode
        return subprocess.run([exe, *sys.argv[1:]]).returncode


if __name__ == "__main__":
    sys.exit(main())
