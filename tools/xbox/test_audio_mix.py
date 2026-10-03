#!/usr/bin/env python3
"""Build and run tests/xbox/test_audio_mix.c on the host: src/pc/audio.c's
voice mixer (block decoder, SSE1 mixing) and output clamp against the
per-sample code they replaced (tests/xbox/audio_mix_ref.c), bit for bit.
Built twice: with the SSE1 paths, and as plain C (PC_AUDIO_SCALAR).
  tools/xbox/test_audio_mix.py
CC may carry flags; CFLAGS adds more. The Xbox's code generation (i686, float
math in scalar SSE, no SSE2), on Windows with MSYS2's clang and mingw32:
  CC="clang --target=i686-w64-mingw32 --sysroot=C:/msys64/mingw32"
  CFLAGS="-march=pentium3 -msse -mfpmath=sse" tools/xbox/test_audio_mix.py"""
import os
import shlex
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
    cc = shlex.split(os.environ.get("CC", "cc")) + shlex.split(os.environ.get("CFLAGS", ""))
    with tempfile.TemporaryDirectory() as tmp:
        # the Xbox's SDL3 audio shim, without the rest of xbox/include/sdk
        # (its pthread.h is the Xbox's; the host's is used here)
        os.makedirs(os.path.join(tmp, "inc", "SDL3"))
        shutil.copy(os.path.join(ROOT, "xbox/include/sdk/SDL3/SDL.h"),
                    os.path.join(tmp, "inc", "SDL3", "SDL.h"))
        for variant in ([], ["-DPC_AUDIO_SCALAR=1"]):
            rc = build_and_run(cc, variant, tmp)
            if rc:
                return rc
        return 0


def build_and_run(cc, variant, tmp):
    exe = os.path.join(tmp, "test_audio_mix" + ("_scalar" if variant else ""))
    cmd = [*cc, *FLAGS, *variant, "-o", exe, os.path.join(ROOT, "tests/xbox/test_audio_mix.c"),
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
    print("scalar" if variant else "simd", end=": ", flush=True)
    return subprocess.run([exe, *sys.argv[1:]]).returncode


if __name__ == "__main__":
    sys.exit(main())
