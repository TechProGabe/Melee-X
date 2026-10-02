#!/usr/bin/env python3
"""Build and run tests/xbox/test_audio_mix.c on the host: src/pc/audio.c's
voice mixer with the block decoder against the per-sample code it replaced
(tests/xbox/audio_mix_ref.c), bit for bit.
  tools/xbox/test_audio_mix.py"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# the SDK's floating-point contract (xbox/CMakeLists.txt SDK_FLAGS), and the
# Xbox paths of the code under test
FLAGS = ["-O2", "-w", "-ffp-contract=off", "-fno-fast-math", "-fno-strict-aliasing", "-fwrapv",
         "-fsigned-char", "-DTARGET_PC=1", "-DMELEE_PC=1", "-DTARGET_XBOX=1"]


def main():
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_audio_mix")
        # the Xbox's SDL3 audio shim, without the rest of xbox/include/sdk
        # (its pthread.h is the Xbox's; the host's is used here)
        os.makedirs(os.path.join(tmp, "inc", "SDL3"))
        shutil.copy(os.path.join(ROOT, "xbox/include/sdk/SDL3/SDL.h"),
                    os.path.join(tmp, "inc", "SDL3", "SDL.h"))
        cmd = [cc, *FLAGS, "-o", exe, os.path.join(ROOT, "tests/xbox/test_audio_mix.c"),
               "-include", os.path.join(ROOT, "src/pc/compat.h"),
               "-I" + os.path.join(tmp, "inc"),
               "-I" + os.path.join(ROOT, "tests/xbox"),
               "-I" + os.path.join(ROOT, "xbox/include"),
               "-I" + os.path.join(ROOT, "extern/aurora/include"),
               "-I" + os.path.join(ROOT, "src"),
               "-I" + os.path.join(ROOT, "src/sdk_include"), "-lm", "-lpthread"]
        r = subprocess.run(cmd)
        if r.returncode:
            return r.returncode
        return subprocess.run([exe, *sys.argv[1:]]).returncode


if __name__ == "__main__":
    sys.exit(main())
