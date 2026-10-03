#!/usr/bin/env bash
# Build default.xbe -> build-xbox/xbe/default.xbe
# Needs the toolchain from tools/xbox/setup.sh (NXDK_DIR, LLVM, DISC_LOWER).
#   XBOX_CFLAGS   extra C flags for the platform code, e.g. "-DXHW_FBDUMP_EVERY=600";
#                 reset on every run, so a debug knob never sticks in the cache
#   XBOX_LTO=1    ThinLTO over the game and sdk code (tools/xbox/thinlto_link.py)
#   XBOX_PGO=gen  instrumented for PGO; XBOX_PGO=<file.profdata> optimized with it
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
export NXDK_DIR="${NXDK_DIR:-/opt/nxdk}"
export LLVM="${LLVM:-/opt/llvm21}"
export DISC_LOWER="${DISC_LOWER:-/opt/melee-tools/disc_lower}"
export PATH="$LLVM/bin:$PATH"
eval "$("$NXDK_DIR/bin/activate" -s)"
cmake -S "$root/xbox" -B "$root/build-xbox" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NXDK_DIR/share/toolchain-nxdk.cmake" -DCMAKE_C_FLAGS="${XBOX_CFLAGS:-}"   -DXBOX_LTO="$([ -n "${XBOX_LTO:-}" ] && echo ON || echo OFF)" -DXBOX_PGO="${XBOX_PGO:-}" \
  ${XBOX_CMAKE_ARGS:-} >/dev/null
ninja -C "$root/build-xbox" ${XBOX_NINJA_ARGS:-}
ls -la "$root/build-xbox/xbe/default.xbe"
