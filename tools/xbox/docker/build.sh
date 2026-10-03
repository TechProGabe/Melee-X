#!/usr/bin/env bash
# Build default.xbe inside the melee-x:sdk image (macOS, Linux, Windows Git Bash).
#   docker build -t melee-x:sdk tools/xbox/docker      # once
#   tools/xbox/docker/build.sh                          # -> build-xbox/xbe/default.xbe
# XBOX_CFLAGS / XBOX_CMAKE_ARGS / XBOX_NINJA_ARGS / XBOX_FORCE / XBOX_KEEP_TEMPS / XBOX_LTO / XBOX_PGO pass through.
# disc_lower is rebuilt from tools/lower/disc_lower.cpp when that changes.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
vroot="$root"
# Git Bash on Windows: no MSYS path rewriting of /src etc., and a C:/ path for -v
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) export MSYS_NO_PATHCONV=1; vroot="$(cd "$root" && pwd -W)";; esac
tty=; [ -t 1 ] && tty=-t
docker run --rm $tty -v "$vroot":/src -w /src \
  -e XBOX_CFLAGS -e XBOX_CMAKE_ARGS -e XBOX_NINJA_ARGS -e XBOX_FORCE -e XBOX_KEEP_TEMPS -e XBOX_LTO -e XBOX_PGO \
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
# Dashboard icon: a $$XTIMAGE section in the XBE plus default.tbn next to it,
# from xbox/assets/logo.png (tools/xbox/make_logo.py). Needs host python3 +
# Pillow, which the image doesn't have; skipped without them or with XBOX_NO_ICON.
xbe="$root/build-xbox/xbe/default.xbe"
if [ -z "${XBOX_NO_ICON:-}" ] && [ -f "$xbe" ] && python3 -c "import PIL" 2>/dev/null; then
  python3 "$root/tools/xbox/xbe_title_image.py" "$xbe" "$root/xbox/assets/logo.png" "$root/build-xbox/xbe/default.tbn"
fi
