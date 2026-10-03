#!/usr/bin/env python3
"""Build and run tests/xbox/test_pobj_mtx.c on the host: pobj.c's
PObjSetupMtx (the per-DObj envelope memo computed in place, prefetch hints)
and mtx.c's SSE HSD_MtxInverseTranspose against the upstream code
(tests/xbox/pobj_mtx_ref.c), bit for bit.
  tools/xbox/test_pobj_mtx.py"""
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
# what the rest of pobj.c refers to and the test never reaches: stubs in a
# unit of their own (their real types don't matter to the linker), each
# stopping the test if it is called after all
UNUSED = ["GXBegin", "GXCallDisplayList", "GXClearVtxDesc", "GXColor1u16", "GXColor1x16",
          "GXColor1x8", "GXColor3u8", "GXColor4u8", "GXEnd", "GXNormal3f32", "GXPosition3f32",
          "GXSetArray", "GXSetVtxAttrFmt", "GXSetVtxDesc", "GXTexCoord1u8", "GXTexCoord1x16",
          "GXTexCoord1x8", "HSD_AObjInterpretAnim", "HSD_AObjLoadDesc", "HSD_AObjRemove",
          "HSD_AObjReqAnim", "HSD_Free", "HSD_IDGetDataFromTable", "HSD_JObjUnrefThis",
          "HSD_MemAlloc", "HSD_Panic", "HSD_SListAlloc", "HSD_SListRemove",
          "HSD_StateSetCullMode", "OSReport", "hsdAllocMemPiece", "hsdFreeMemPiece",
          "hsdInitClassInfo", "hsdIsDescendantOf", "hsdNew", "hsdSearchClassInfo",
          "pc_vtx_array_scan", "pc_vtx_array_size"]


def main():
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_pobj_mtx")
        # the test #includes pobj.c and mtx.c to reach their static functions
        wrapper = os.path.join(tmp, "t.c")
        with open(wrapper, "w") as f:
            f.write('#include "pc/compat.h"\n#include <Runtime/platform.h>\n'
                    '#include "%s"\n#include "%s"\n#include "%s"\n' % (
                        os.path.join(ROOT, "src/sysdolphin/baselib/pobj.c"),
                        os.path.join(ROOT, "src/sysdolphin/baselib/mtx.c"),
                        os.path.join(ROOT, "tests/xbox/test_pobj_mtx.c")))
        stubs = os.path.join(tmp, "stubs.c")
        with open(stubs, "w") as f:
            f.write('#include <stdio.h>\n#include <stdlib.h>\n'
                    'static void unused(const char* name)\n'
                    '{\n    fprintf(stderr, "unexpected call: %s\\n", name);\n    abort();\n}\n'
                    'char hsdClass[256];\n')
            for name in UNUSED:
                f.write('void %s(void) { unused("%s"); }\n' % (name, name))
        sources = [wrapper, stubs, os.path.join(ROOT, "extern/aurora/lib/dolphin/mtx/mtx.c"),
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
