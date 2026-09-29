#!/usr/bin/env bash
# Boot a build in xemu (macOS) and capture COM1.
#   tools/xbox/xemu_run.sh [seconds] [stop-regex]
# Env:
#   MX_ISO    your own GALE01 image to pack next to default.xbe ("none" = omit,
#             to exercise the missing-disc screen). Never committed.
#   MX_XBE    default: build-xbox/xbe/default.xbe
#   MX_RUN    work dir (XISO + logs), default ~/xemu/mx-run
#   MX_GUI=1  leave xemu running (don't kill at timeout)
#   MX_STAGE_EXTRA  dir whose contents are also packed onto the disc
#   MX_XEMU_ARGS    extra xemu arguments (e.g. -config_path <xemu.toml>)
# xemu needs your own MCPX ROM, BIOS and HDD image, set to 64 MB.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
secs="${1:-60}"; stop="${2:-__never__}"
xbe="${MX_XBE:-$root/build-xbox/xbe/default.xbe}"
run="${MX_RUN:-$HOME/xemu/mx-run}"
iso="${MX_ISO:-}"
mkdir -p "$run/stage"
rm -rf "$run/stage/"*
cp "$xbe" "$run/stage/default.xbe"
[ -n "${MX_STAGE_EXTRA:-}" ] && cp -R "$MX_STAGE_EXTRA"/. "$run/stage/"
if [ -n "$iso" ] && [ "$iso" != none ]; then ln -f "$iso" "$run/stage/$(basename "$iso")" 2>/dev/null || cp "$iso" "$run/stage/"; fi
rm -f "$run/game.xiso"
docker run --rm -v "$run":/run melee-x:sdk \
  /usr/src/nxdk/tools/extract-xiso/build/extract-xiso -c /run/stage /run/game.xiso >/dev/null
log="$run/serial.log"; : > "$log"
/Applications/Xemu.app/Contents/MacOS/xemu -dvd_path "$run/game.xiso" \
  -device lpc47m157 -serial "file:$log" ${MX_XEMU_ARGS:-} > "$run/xemu.out" 2>&1 &
pid=$!
for ((i=0; i<secs; i++)); do
  sleep 1
  kill -0 $pid 2>/dev/null || break
  grep -qE "$stop" "$log" 2>/dev/null && break
done
[ "${MX_GUI:-0}" = 1 ] || { kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true; }
echo "--- serial ($i s) ---"; tr -d '\r' < "$log"
