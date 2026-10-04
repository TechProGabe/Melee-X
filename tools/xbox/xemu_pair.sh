#!/usr/bin/env bash
# Two xemu instances on one virtual cable (docs/testing.md "Two xemu instances").
#   tools/xbox/xemu_pair.sh <scenario-a> <scenario-b> <secs> [stop-regex]
# A scenario is a folder (as MX_STAGE_EXTRA) or a name under tools/xbox/scenarios.
# Each instance runs through tools/xbox/xemu_run.sh with a work folder, a
# config, an HDD image and an EEPROM of its own; xemu's UDP network back end
# joins them through tools/xbox/xemu_tap.py, which relays every Ethernet
# frame and writes it to $MX_PAIR/pair.pcap (A binds 127.0.0.1:9368 and
# sends to the tap's 9370, B binds 9369 and sends to 9371): a cable with no
# DHCP server on it, so the pair also exercises AutoIP, and a capture for
# tools/xbox/net_audit.py. B's EEPROM is A's with another, locally
# administered MAC (tools/xbox/eeprom_mac.py). Both stop at <secs>, or each
# once its own log matches <stop-regex>; this script starts exactly these two
# (and the tap) and each runner kills only its own instance.
# Env (besides xemu_run.sh's MX_ISO, MX_XEMU, MX_XISO, MX_XBE):
#   MX_PAIR        work folder (default C:/xemu/b): run-a/, run-b/ and their
#                  hdd/eeprom copies, made once from the files below
#   MX_PAIR_TOML   the xemu config to start from (default C:/xemu/xemu.toml;
#                  copied, never changed)
#   MX_PAIR_HDD    HDD image to copy for each instance (default: the config's hdd_path)
#   MX_PAIR_EEPROM EEPROM image to copy (default: the config's eeprom_path)
#   MX_PAIR_ARGS   extra xemu arguments for both (e.g. -icount shift=1; never sleep=off,
#                  docs/lan-plan.md F8)
# Logs: $MX_PAIR/run-a/serial.log, $MX_PAIR/run-b/serial.log.
set -euo pipefail
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) export MSYS_NO_PATHCONV=1;; esac
root="$(cd "$(dirname "$0")/../.." && pwd)"
[ $# -ge 3 ] || { sed -n '2,4p' "$0"; exit 2; }
sa="$1"; sb="$2"; secs="$3"; stop="${4:-__never__}"
pair="${MX_PAIR:-C:/xemu/b}"
toml="${MX_PAIR_TOML:-C:/xemu/xemu.toml}"
py="${PYTHON:-python3}"
scen() { if [ -d "$1" ]; then echo "$1"; else echo "$root/tools/xbox/scenarios/$1"; fi; }
key() { sed -n "s/^$1 *= *'\(.*\)'.*/\1/p" "$toml" | head -1; }
hdd="${MX_PAIR_HDD:-$(key hdd_path)}"
eep="${MX_PAIR_EEPROM:-$(key eeprom_path)}"
[ -f "$hdd" ] && [ -f "$eep" ] || { echo "need the HDD image and EEPROM ($hdd, $eep)"; exit 1; }
mkdir -p "$pair/run-a" "$pair/run-b"
# one-time copies: the user's own files are never opened by the pair
[ -f "$pair/hdd-a.qcow2" ] || cp "$hdd" "$pair/hdd-a.qcow2"
[ -f "$pair/hdd-b.qcow2" ] || cp "$hdd" "$pair/hdd-b.qcow2"
[ -f "$pair/eeprom-a.bin" ] || cp "$eep" "$pair/eeprom-a.bin"
[ -f "$pair/eeprom-b.bin" ] || "$py" "$root/tools/xbox/eeprom_mac.py" "$pair/eeprom-a.bin" "$pair/eeprom-b.bin"
for side in a b; do
  if [ $side = a ]; then bind=9368; remote=9370; else bind=9369; remote=9371; fi   # remote: the tap
  # the config without any [net] section of its own, then the pair's
  awk '/^\[net/{skip=1; next} /^\[/{skip=0} !skip' "$toml" |
    sed -e "s|^hdd_path *=.*|hdd_path = '$pair/hdd-$side.qcow2'|" \
        -e "s|^eeprom_path *=.*|eeprom_path = '$pair/eeprom-$side.bin'|" > "$pair/xemu-$side.toml"
  printf "\n[net]\nenable = true\nbackend = 'udp'\n\n[net.udp]\nbind_addr = '127.0.0.1:%s'\nremote_addr = '127.0.0.1:%s'\n" \
    $bind $remote >> "$pair/xemu-$side.toml"
done
run_one() {   # side scenario
  MX_RUN="$pair/run-$1" MX_STAGE_EXTRA="$(scen "$2")" MX_SHOTS=0 \
  MX_XEMU_ARGS="-config_path $pair/xemu-$1.toml ${MX_PAIR_ARGS:-}" \
    "$root/tools/xbox/xemu_run.sh" "$secs" "$stop" > "$pair/run-$1/xemu_run.out" 2>&1
}
echo "pair: A $(scen "$sa"), B $(scen "$sb"), $secs s, capture $pair/pair.pcap"
"$py" "$root/tools/xbox/xemu_tap.py" --pcap "$pair/pair.pcap" > "$pair/xemu_tap.out" 2>&1 & pt=$!
run_one a "$sa" & pa=$!
run_one b "$sb" & pb=$!
wait $pa || true
wait $pb || true
kill $pt 2>/dev/null || true
wait $pt 2>/dev/null || true
for side in a b; do
  echo "--- $side: $(grep -c . "$pair/run-$side/serial.log" 2>/dev/null || echo 0) lines; [NET]/[NETP]:"
  tr -d '\r' < "$pair/run-$side/serial.log" | grep -E '^\[(NET|NETP)\]' || true
done
