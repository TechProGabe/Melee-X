#!/usr/bin/env python3
"""Build and run tests/xbox/test_card_endian.c on the host: card_endian.c's
field tables against the game's GmSaveData and name-tag bank, and the
big-endian <-> native conversion of memory-card files."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_card_endian")
        # The host is LP64, so the size asserts of disc structs that hold
        # pointers (elsewhere in <melee/gm/types.h>'s includes) fail here;
        # they are switched off. The card structs have no pointers, and
        # card_endian.c's own asserts stay on. The test #includes
        # card_endian.c to reach its tables.
        wrapper = os.path.join(tmp, "t.c")
        with open(wrapper, "w") as f:
            f.write('#include "pc/compat.h"\n#include <Runtime/platform.h>\n'
                    '#undef ASSERT_SIZE\n#define ASSERT_SIZE(T, s)\n'
                    '#undef ASSERT_OFFSET\n#define ASSERT_OFFSET(T, m, o)\n'
                    '#undef DISC_ASSERT_SIZE\n#define DISC_ASSERT_SIZE(T, s)\n'
                    '#include "%s"\n#include "%s"\n' % (
                        os.path.join(ROOT, "xbox/src/sdk/card_endian.c"),
                        os.path.join(ROOT, "tests/xbox/test_card_endian.c")))
        # aurora's dolphin/types.h (fixed-width under TARGET_PC) ahead of
        # src/sdk_include's, as in the Xbox build
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
