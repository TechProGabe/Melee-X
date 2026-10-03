#!/usr/bin/env bash
# Regenerate xbox/melee.profdata (docs/pgo.md): build the instrumented image,
# play the training scenarios in xemu with -icount, turn each run's [PGOC]
# dump into a raw profile, merge, and write xbox/melee.profdata.txt (where
# and from what it came). Run from the checkout, with xemu_run.sh's
# environment (MX_ISO, MX_XEMU, MX_XISO on Windows, MX_XEMU_ARGS with the
# xemu config, and a memory card staged by the caller, see below):
#
#   MX_CARD=/path/to/card_a tools/xbox/pgo_train.sh [scenario...]
#
# MX_CARD: a folder holding the card_a/*.gci save the scenarios need (the
# 100% save; never committed). Default scenarios: gl fodperf fd2 ps corn.
# One xemu at a time: this refuses to start if one is running.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
scen=("$@")
[ ${#scen[@]} -gt 0 ] || scen=(gl fodperf fd2 ps corn)
py=python3; python3 -c '' >/dev/null 2>&1 || py=py   # Windows: python3 may be the Store's stub
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) build=tools/xbox/msys/build.sh; running() { tasklist | grep -qi '^xemu.exe'; } ;;
  *) build=tools/xbox/docker/build.sh; running() { pgrep -x xemu >/dev/null || pgrep -f Xemu.app >/dev/null; } ;; esac
if running; then echo "an xemu is running: wait for it (docs/fps-plan.md Rules)"; exit 1; fi
[ -n "${MX_CARD:-}" ] || { echo "set MX_CARD to the folder with card_a/ (the save the scenarios need)"; exit 1; }

XBOX_PGO=gen XBOX_CFLAGS=-DXHW_AUTOPAD=1 $build
work="$root/build-xbox/pgo-train"; rm -rf "$work"; mkdir -p "$work"
cp build-xbox/xbe/default.xbe build-xbox/melee_x.exe build-xbox/melee_x.map "$work/"
args="${MX_XEMU_ARGS:-} -icount shift=1,sleep=off"
for s in "${scen[@]}"; do
  st="$work/stage-$s"; mkdir -p "$st"
  cp -R "tools/xbox/scenarios/$s/." "$st/"; cp -R "$MX_CARD/card_a" "$st/"
  sed -i '/^[0-9]* *SHOT/d' "$st/autopad.txt"   # screenshots: not needed, and slow
  MX_XBE="$work/default.xbe" MX_RUN="$work/run" MX_STAGE_EXTRA="$st" MX_XEMU_ARGS="$args" \
    tools/xbox/xemu_run.sh 1500 '\[GAME\] end banner done' > "$work/$s.out" 2>&1 || true
  tr -d '\r' < "$work/run/serial.log" > "$work/$s.log"
  $py tools/xbox/pgo_raw.py --exe "$work/melee_x.exe" --map "$work/melee_x.map" "$work/$s.log" -o "$work/$s.profraw"
done
prof=(); for s in "${scen[@]}"; do prof+=("$work/$s.profraw"); done
llvm="${LLVM:-}"; pd=llvm-profdata; [ -n "$llvm" ] && [ -x "$llvm/bin/llvm-profdata" ] && pd="$llvm/bin/llvm-profdata"
command -v "$pd" >/dev/null 2>&1 || pd=/c/msys64/mingw64/bin/llvm-profdata
"$pd" merge -o xbox/melee.profdata "${prof[@]}"
{
  echo "xbox/melee.profdata: PGO profile for XBOX_PGO=xbox/melee.profdata builds (docs/pgo.md)"
  echo "trained on: $(git rev-parse --short HEAD)$(git diff --quiet || echo ' + local changes') ($(git log -1 --format=%cs))"
  echo "scenarios: ${scen[*]} (xemu -icount shift=1, autopad, MELEE_SEED=1)"
  echo "llvm: $("$pd" --version 2>/dev/null | grep -io 'llvm version [0-9.]*' | head -1)"
  "$pd" show "xbox/melee.profdata" 2>/dev/null | grep -E '^(Total functions|Maximum function count|Total count)'
} > xbox/melee.profdata.txt
cat xbox/melee.profdata.txt
