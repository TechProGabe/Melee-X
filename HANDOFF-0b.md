# Phase 0B handoff (paused 2026-10-04, lan-0b agent)

Branch `lan-0b-net` (off dev 3e565e4, dev's `docs/lan-plan.md` from d55bd5b checked out into the
worktree, uncommitted). Worktree: `C:\Users\Bingo\Documents\GitHub\Melee-X\.claude\worktrees\agent-af04da348472788d7`.
Nothing committed. No xemu instance of mine running; my lock slots freed.

## Done (files, all uncommitted)
- `xbox/src/hw/xhw_net.c` (new): the boundary of lan-plan "Architecture" plus D14: lazy start on a worker,
  `tcpip_init` (+1 prio via `KeSetBasePriorityThread` in the init callback), `netifapi_netif_add(nvnetif_init,
  net_input)`; manual address with our own ACD (`acd_add/acd_start`); DHCP; AutoIP after 4 s only while DHCP
  has no offer in hand; conflict detection -> `XHW_NET_CONFLICT` 10 s; link poll 4/s handed to
  `netifapi_netif_set_link_up/down`; `net_input` drops IPv6 + multicast frames; TTL 1 per pcb; per-socket
  64 KB SPSC ring; `conduct_rx/conduct_tx/gov_take/gov_bcast` (pure, host-tested); `[NET]` counters;
  `xhw_net_pause/shutdown/ident`; MX_LOG_UDP stream from UDP 41050, paced 200/s, quiet socket.
- `xbox/src/hw/xhw_netprobe.c` (new): MX_NETPROBE, MX_NETPROBE_FLOOD (2000/s for 10 s), MX_LOG_UDP parsing;
  beacons to 255.255.255.255:41001 only, PONG = same bytes, peers silent 3 s skipped, 60 Hz pings 60 s in a match.
- `xbox/include/xhw.h` (appended network section, `XHW_NET_CONFLICT`, `XHW_UDP_MAX_SEND 1200`,
  `XHW_NET_DISCOVERY_PORT`), `xhw_internal.h` (appended block), `xhw_main.c` (`xhw_netprobe_boot` after
  autopad load; `xhw_net_shutdown` in `xhw_quit_to_dashboard` and `launch`), `xhw_sys.c` (log tee
  `xhw_log_tee`, under the log lock), `xhw_autopad.c` (`xhw_autopad_match_tick`, outside the file list),
  `xbox/CMakeLists.txt` (link `libnxdk_net.lib`; lwIP include dirs on `xhw_net.c` only).
- Tests: `tests/xbox/test_net_ring.c` + `tools/xbox/test_net_ring.py`, `tests/xbox/test_net_gov.c` +
  `tools/xbox/test_net_gov.py`, `tools/xbox/test_net_audit.py` (new, not in the file list), CI list in
  `.github/workflows/build.yml`.
- Tools: `net_audit.py`, `xemu_tap.py`, `xemu_pair.sh` (through the tap, own HDD/EEPROM copies),
  `eeprom_mac.py` (locally administered MAC), `lan_probe.py`, `lan_logd.py`, `console2.py`,
  `scenarios/netprobe`.
- Docs written: `platform.md` "Network", `testing.md` (host tests, "Network" section, 3 switch rows at the
  table's end, log tags), `decisions.md` "Network layer" subsection, `toolchain.md`, `architecture.md`
  memory row, `LICENSE.md` section 4 (lwIP BSD, nvnetdrv MIT). `lan-plan.md` NOT yet updated (Status, F4, F8).

## Verified
- Builds clean (`XBOX_CFLAGS=-DXHW_AUTOPAD=1 tools/xbox/msys/build.sh`); autopad image 4,900,864 -> 5,154,816
  bytes (+248 KB, lwIP+nvnetdrv). Plain release size not measured yet.
- Host: `test_net_ring.py` ok (mutation-checked: 4/4 ring bugs caught after adding a guard zone),
  `test_net_gov.py` ok, `test_net_audit.py` ok (clean capture passes; 26 cases, every rule 1-11 caught).
- xemu NAT (`C:\xemu\run-0b\nat.toml`, logs `C:\xemu\run-0b\serial-nat2.log`, `serial-nat3.log`): NIC up
  ~30 ms, "144 KB for the stack", link up ~300 ms, `[NETP] dhcp 10.0.2.15 after 10-12 s`,
  `[NETP] beacon from 10.0.2.2:41002`, pinging starts in the match. NOT passing: `rtt ... 0 back`,
  lan_probe/lan_logd receive nothing.
- G0 base runs done (dev 3e565e4, autopad): `C:\xemu\b\g0\base-gl.log`, `base-fod.log`, `base-lock.log` +
  `shots-base-lock\s_000..003.png`; base xbe/map `C:\xemu\b\g0\base-autopad.*`. Base worktree `C:\xemu\b\base`.

## Left
- Fix/decide the NAT gate (below), rerun; pair run (`C:\xemu\b\run_pair.sh`, scenarios in `C:\xemu\b\scen\`);
  offline-through-tap (`--silent`), flood run, relaunch (`C:\xemu\b\run_relaunch.sh`), same-seed ACD
  conflict pair (optional); G0 new side (`C:\xemu\b\g0.sh`: gl/fodperf icount, lock, plus a
  `-DXHW_AUTOPAD=1 -DXHW_TEST_BUILD=0` build to the results), `icount_report.py`, `cmp` shots, BEAT deltas,
  no `[NET]` offline; plain build size; lan-plan.md updates; S1 staged folder (PC LAN IP 192.168.158.95,
  `env MX_LOG_UDP=192.168.158.95:41050`, folder `F:\Applications\Melee-X-net\`), pktmon instruction
  (`pktmon start --capture --comp nics --pkt-size 0 -f C:\xemu\hw\s1-net.etl`, `pktmon stop`,
  `pktmon etl2pcap C:\xemu\hw\s1-net.etl --out C:\xemu\hw\s1-net.pcapng`, syntax checked on this PC),
  audio-check list (NIC IRQ 4 + DPC, tcpip thread +1, worker 0, probe +1, log sender -1, ~144 KB pool/
  contiguous + 64 KB per socket, +248 KB image, nvnetdrv_stop before XLaunchXBE).

## Findings for lan-plan.md
- lwIP 2.2.1 `autoip_stop` leaves the AutoIP ACD running; it then binds 169.254.x over a DHCP lease (seen:
  10.0.2.15 -> 169.254.25.13). Worked around (acd_stop first; lease put back).
- `dhcp_supplied_address` checks state only. DHCP's ACD check delays a lease ~6-10 s after the ACK
  (xemu: 10-12.7 s from link up). AutoIP binds >= 4 + 6..10 s after link up: the pair gate's "beacon from
  169.254. within 5 s of link up" cannot hold; Goal 1 (5 s) needs the NIC started before the lobby.
- RA -> lwIP sends an RS from :: even with no IPv6 address (`nd6.c:604`); fixed by `net_input`.
- xemu slirp NAT: hostfwd delivers the PC's datagrams from 127.0.0.1 after the first (from 10.0.2.2), which
  the on-link filter drops; guest -> 10.0.2.2 datagrams never reach the PC (TTL 1 through slirp-as-router
  suspected, unconfirmed: the scratch TTL-64 run was killed). The NAT gate's rtt line conflicts with D14
  rule 7 / rule 9: decision for the main session (gate's rtt from the pair instead, or a test-only exception).
- lwIP keeps one IPv4 per netif: a lease arriving mid-session on AutoIP changes the address (D14 rule 5):
  phase 3 needs e.g. `xhw_net_hold` (dhcp_stop while a link-local session runs).

## Do not redo
- Base G0 runs, EEPROM checksum algorithm (verified against `C:\xemu\eeprom.bin`), pktmon syntax check,
  the lwIP source reading above. Do not use `C:\xemu\b\scratch-ttl64.xbe` for anything but that diagnosis.
