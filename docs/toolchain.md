# Toolchain

## Setup (once)

```sh
tools/xbox/setup.sh
export NXDK_DIR=/opt/nxdk LLVM=/opt/llvm21 DISC_LOWER=/opt/melee-tools/disc_lower
```

Installs LLVM 21.1.8 (GitHub release), nxdk at the commit OpenCrossing-Xbox
pins (built with that LLVM), and builds `disc_lower`. Host packages are
listed at the top of the script.

**macOS / Docker.** `tools/xbox/docker/Dockerfile` builds the same SDK
image OpenCrossing-Xbox uses, plus the LLVM development packages that
`disc_lower` needs. Its first part copies OpenCrossing's Dockerfile line
for line, so the two images share layers.

```sh
docker build -t melee-x:sdk tools/xbox/docker   # once
tools/xbox/docker/build.sh                      # disc_lower if stale, then xbox/build.sh
```

`XBOX_CFLAGS`, `XBOX_CMAKE_ARGS`, `XBOX_NINJA_ARGS`, `XBOX_FORCE` and
`XBOX_KEEP_TEMPS` pass through to the container.

## Game code

```sh
tools/lower/test_lower.py                     # oracle: lowered == GCC scalar_storage_order
tools/xbox/compile_game.py                    # all 1008 units -> build-xbox/game/**.obj
tools/xbox/compile_game.py --source src/melee/ft/ftlib.c
XBOX_KEEP_TEMPS=1 tools/xbox/compile_game.py --source ...   # keep .i / .lowered.c
```

Each unit goes through four steps (`compile_game.py`):

1. `clang -E` with the game triple and `-DMELEE_DISC_LOWERING`, which turns
   `DISC_STRUCT` into `__attribute__((annotate("melee_disc")))`.
2. String literals re-encoded as CP932 (GCC's `-fexec-charset=CP932`).
3. `disc_lower` (LibTooling) rewrites every scalar access to an annotated
   struct into `__os_be_*` loads/stores (`tools/lower/disc_access.h`),
   bit-fields included, using the byte offsets of the game triple's layout.
4. `clang -c` with the same triple. Warnings are off (`-Wno-everything`),
   but implicit function declarations are errors, in the game and in the
   SDK layer alike. An undeclared float function returns `int`, so its
   result would be read from EAX instead of st(0).

### The game triple

`--target=i686-pc-windows-gnu -mno-ms-bitfields -march=pentium3`, not nxdk's
`i386-pc-win32`:

| | i386-pc-win32 (nxdk) | i686-pc-windows-gnu -mno-ms-bitfields |
|---|---|---|
| `struct { u8 a:1; u8 b:3; u16 c:5; u32 d:7; u8 e; }` | 12 bytes (MS bitfields) | 4 bytes, as on GameCube |
| `long long` / `double` in structs | 8-aligned | 8-aligned, as on GameCube |
| struct return, argument passing | MSVC | identical (checked in asm) |
| C symbol names | `_name` | `_name` |

The lowering pass bakes byte offsets from the layout it parses with, and the
final compile must agree, so both use this triple. The disc layout is the
GameCube's (MWCC, MSB-first bit-fields in SysV-style units), which the MS
layout does not reproduce. The objects still link with nxdk's lld-link and
call nxdk-built code, because the call ABI is the same.

Floating point: `-msse -mfpmath=sse` (floats round to single like Gekko;
doubles stay x87), `-ffp-contract=off -fno-fast-math`, `-fno-strict-aliasing
-fwrapv` (load-bearing for the decomp, as in every sibling port).

System headers are nxdk's pdclib. `xbox/include/game/` fills what pdclib
lacks (`<sys/types.h>`, `M_PI`, `va_list` in aurora's `os.h`).

## Platform code

`xbox/` is built by CMake with nxdk's toolchain file. Files that share
structs with the game (the Dolphin SDK implementation) are compiled with the
game triple; files that talk to the kernel, pbkit or USB use nxdk's own and
expose only scalars and pointers to the rest.

## CI

The workflow runs only when started by hand (`workflow_dispatch`); builds
and tests run locally. `.github/workflows/build.yml` runs `tools/xbox/setup.sh` (LLVM and nxdk are
cached, keyed on that script), builds `default.xbe` and runs the three host
tests. The XBE and its link map are uploaded as the `default.xbe` artifact.
