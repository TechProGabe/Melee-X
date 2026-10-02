#!/usr/bin/env python3
"""Build and run tests/xbox/test_anim_mtx.c on the host: sysdolphin's
keyframe interpreter (fobj.c), HSD_MtxSRT with pc_sincosf and the fused
envelope blend (mtx.c, mtx.h) against the code as it was before the
Pentium III rewrites (tests/xbox/anim_mtx_ref.c), bit for bit.
  tools/xbox/test_anim_mtx.py [--full]   (--full: pc_sincosf on all 2^32 floats)"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# the game's floating-point contract (tools/xbox/compile_game.py), and the
# Xbox paths of the code under test (TARGET_XBOX, SSE)
FLAGS = ["-O2", "-w", "-ffp-contract=off", "-fno-fast-math", "-fno-strict-aliasing", "-fwrapv",
         "-ftrivial-auto-var-init=zero", "-DTARGET_PC=1", "-DMELEE_PC=1", "-DTARGET_XBOX=1",
         # xbox_game_prelude.h's, which the host build doesn't include
         "-DHSD_PREFETCH(p)=__builtin_prefetch((const void*)(p))"]
LIBM = ["pc_sinf.c", "pc_cosf.c", "pc_sindf.c", "pc_cosdf.c", "pc_rem_pio2f.c",
        "pc_rem_pio2_large.c", "pc_sincosf.c", "pc_tanf.c", "pc_tandf.c"]


def main():
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_anim_mtx")
        # the test #includes fobj.c and mtx.c to reach their static functions
        wrapper = os.path.join(tmp, "t.c")
        with open(wrapper, "w") as f:
            f.write('#include "pc/compat.h"\n#include <Runtime/platform.h>\n'
                    '#include "%s"\n#include "%s"\n#include "%s"\n' % (
                        os.path.join(ROOT, "src/sysdolphin/baselib/fobj.c"),
                        os.path.join(ROOT, "src/sysdolphin/baselib/mtx.c"),
                        os.path.join(ROOT, "tests/xbox/test_anim_mtx.c")))
        sources = [wrapper, os.path.join(ROOT, "extern/aurora/lib/dolphin/mtx/mtx.c"),
                   os.path.join(ROOT, "extern/aurora/lib/dolphin/mtx/vec.c"),
                   os.path.join(ROOT, "extern/aurora/lib/dolphin/mtx/quat.c")]
        sources += [os.path.join(ROOT, "src/pc/libm", n) for n in LIBM]
        cmd = [cc, *FLAGS, "-o", exe, *sources,
               "-include", os.path.join(ROOT, "src/pc/libm/pc_trig.h"),
               "-I" + os.path.join(ROOT, "tests/xbox"),
               "-I" + os.path.join(ROOT, "xbox/include"),
               "-I" + os.path.join(ROOT, "extern/aurora/include"),
               "-I" + os.path.join(ROOT, "src"),
               "-I" + os.path.join(ROOT, "src/sdk_include"),
               "-I" + os.path.join(ROOT, "src/sysdolphin/baselib"), "-lm"]
        r = subprocess.run(cmd)
        if r.returncode:
            return r.returncode
        return subprocess.run([exe, *sys.argv[1:]]).returncode


if __name__ == "__main__":
    sys.exit(main())
