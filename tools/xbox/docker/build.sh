#!/usr/bin/env bash
# Build default.xbe inside the melee-x:sdk image (macOS / any Docker host).
#   docker build -t melee-x:sdk tools/xbox/docker      # once
#   tools/xbox/docker/build.sh                          # -> build-xbox/xbe/default.xbe
# XBOX_CFLAGS / XBOX_CMAKE_ARGS / XBOX_NINJA_ARGS / XBOX_FORCE / XBOX_KEEP_TEMPS pass through.
# disc_lower is rebuilt from tools/lower/disc_lower.cpp when that changes.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
tty=; [ -t 1 ] && tty=-t
docker run --rm $tty -v "$root":/src -w /src \
  -e XBOX_CFLAGS -e XBOX_CMAKE_ARGS -e XBOX_NINJA_ARGS -e XBOX_FORCE -e XBOX_KEEP_TEMPS \
  melee-x:sdk bash -c '
  set -e
  export NXDK_DIR=/usr/src/nxdk DISC_LOWER=/src/build-xbox/tools/disc_lower
  mkdir -p build-xbox/tools
  if [ ! -x "$DISC_LOWER" ] || [ tools/lower/disc_lower.cpp -nt "$DISC_LOWER" ]; then
    echo "== disc_lower"
    clang++ -std=c++20 -O1 -fno-rtti tools/lower/disc_lower.cpp \
      -I"$LLVM/include" -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
      -lclang-cpp $(llvm-config --libs --system-libs) -o "$DISC_LOWER"
  fi
  xbox/build.sh'
