#!/usr/bin/env bash
# One-time toolchain setup for Melee-X (Linux x86-64 host).
#
#   LLVM 21   -> $LLVM      (default /opt/llvm21): clang/lld for nxdk and the
#                           game, plus LibTooling for tools/lower/disc_lower
#   nxdk      -> $NXDK_DIR  (default /opt/nxdk), pinned below, built with LLVM 21
#   disc_lower-> $DISC_LOWER (default /opt/melee-tools/disc_lower)
#
# Host packages: build-essential cmake ninja-build git bison flex python3
# libzstd-dev zlib1g-dev libxml2-dev.
#
# LLVM 21, not the distro clang: nxdk warns that 19.x-20.1.2 miscompile
# (llvm/llvm-project#134607), and the prebuilt release ships the clang
# libraries disc_lower links against.
set -euo pipefail

LLVM="${LLVM:-/opt/llvm21}"
NXDK_DIR="${NXDK_DIR:-/opt/nxdk}"
DISC_LOWER="${DISC_LOWER:-/opt/melee-tools/disc_lower}"
LLVM_VERSION=21.1.8
NXDK_SHA=58427c078b4ccb0121f359b6b6c536b3b2914976
root="$(cd "$(dirname "$0")/../.." && pwd)"
jobs="$(nproc)"

if [ ! -x "$LLVM/bin/clang" ]; then
  echo "== LLVM $LLVM_VERSION -> $LLVM"
  mkdir -p "$LLVM"
  curl -sSL "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/LLVM-$LLVM_VERSION-Linux-X64.tar.xz" \
    | tar -xJ -C "$LLVM" --strip-components=1
fi
export PATH="$LLVM/bin:$PATH"

if [ ! -f "$NXDK_DIR/lib/libpdclib.lib" ]; then
  echo "== nxdk $NXDK_SHA -> $NXDK_DIR"
  if [ ! -d "$NXDK_DIR/.git" ]; then
    git clone --recursive https://github.com/XboxDev/nxdk.git "$NXDK_DIR"
  fi
  git -C "$NXDK_DIR" checkout -q "$NXDK_SHA"
  git -C "$NXDK_DIR" submodule update --init --recursive
  make -C "$NXDK_DIR" tools -j"$jobs"
  (export NXDK_DIR; eval "$("$NXDK_DIR/bin/activate" -s)"; \
   make -C "$NXDK_DIR" NXDK_ONLY=y NXDK_SDL=y NXDK_CXX=y -j"$jobs")
fi

echo "== disc_lower -> $DISC_LOWER"
mkdir -p "$(dirname "$DISC_LOWER")"
# shellcheck disable=SC2046
"$LLVM/bin/clang++" -std=c++20 -O1 -fno-rtti "$root/tools/lower/disc_lower.cpp" \
  -I"$LLVM/include" -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -lclang-cpp $("$LLVM/bin/llvm-config" --libs --system-libs) -o "$DISC_LOWER"

echo "done. export NXDK_DIR=$NXDK_DIR LLVM=$LLVM DISC_LOWER=$DISC_LOWER"
