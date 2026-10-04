# LAN play plan (2026-10-04, dev at 10953d5; console B 480i only and two players a console added the same day)

Written as the plan for executing sessions (one phase each, on a branch of
its own) and kept as their record, like `docs/fps-plan.md`. Nothing in it is
built yet. Findings carry the evidence, Decisions the choices and why, the
Status table near the end is the per-phase summary. Claims that were inferred
and not checked are marked **unverified**, with how to check them.

## Goal

Two to four players on two original Xboxes, one or two on each console
(controller ports 1 and 2), both running the same Melee-X build from the
same disc image, play VS matches over a LAN: 1v1, 2v2 (teams or free for
all) and 1+2 three-player games. Console A runs 720p or 480p, console B
only 480i (F9), so every two-console match pairs A's mode with B's 480i.
Done means all of this, on the two consoles, with A at 720p and at 480p:

1. With LAN Play open on both, each lists the other within 5 s, with its
   player count. No IP typed.
2. START on either elects one host; both leave the lobby for the character
   select on the same frame.
3. Both enter the same stage with the same characters and seed on the same
   frame.
4. A 4-stock match runs to GAME! with no desync flag and the same results
   screen on both, as 1v1 (1+1), as 2v2 teams (2+2) and as a three-player
   game (2+1 and 1+2: either console can be the one with two).
5. A rematch (results -> character select -> match) works.
6. Cable pulled, peer powered off, peer quit: a readable message within
   10 s, back in the lobby (the B button leaves it for the menu). No dashboard, no hang screen.
7. A forced desync ends the match on both consoles with a message.
8. Single-player play is unchanged: `[SIMH]` and lockstep shots byte for
   byte the same with LAN compiled in.

Out of scope: internet play (Direct Connect, Ranked, Unranked, Profile stay
off), rollback, more than two players per console or more than two
consoles (D5), cross-play with melee-pc on a PC (the two-pad wire format
ends v9 compatibility, D1). What is kept open at no cost is said in each
decision.

## Ground rules

- One phase per session, own worktree and branch (`lan-0a-probes`, ...).
  Nothing goes on `dev` except through the merge gate in `docs/handoff.md`
  (code review, host tests, before/after shots, `[SIMH]`).
- No commit and no push without the user's go-ahead. A phase ends with its
  changes on its branch's worktree, a summary and a request.
- **No behaviour change outside netplay.** Gate G0 below runs in every
  phase that touches game, sdk or hw code.
- 64 MB. A match keeps at least 5 MB free; quote `[BEAT]` free memory and
  the image size for every change that allocates or links new code.
- Every edit to imported code (`src/melee`, `src/sysdolphin`, `src/pc`,
  and the netplay sources this plan imports into `src/pc`): a `PORT:`
  comment and a line in `docs/decisions.md`. Every new switch:
  `docs/testing.md`'s table and `docs/decisions.md`.
- `xhw.h` stays the only way across the two triples: scalars, pointers,
  byte buffers. No lwIP type and no socket struct crosses it.
- xemu: one instance, except the pair runner of phase 0B, which starts
  exactly two and kills only those two. No `sleep=off` in pair runs (F8).
- Audio check before every console build (handoff.md): this work adds an
  interrupt source (the NIC), threads and memory, so every LAN console
  build's hand-over lists them.
- Console sessions are the user's time: S1-S4 below, one deploy each, a
  five-line instruction, logs pulled before the next boot.
- Never capture the desktop. No game data, EEPROM, BIOS or HDD image in the
  repository (the pair setup lives in `C:\xemu`).
- Stop and ask the user before: any offline behaviour change (also a fix
  for roadmap item 10 that would change offline play), going under 5 MB
  free, vendoring third-party code, moving the nxdk pin, more than two
  players per console or more than two consoles, anything internet-facing.

**G0, the no-change gate** (base = `dev` at the branch point, new = the
branch; both `XBOX_CFLAGS=-DXHW_AUTOPAD=1`):

```sh
# gl and fodperf without SHOT lines, -icount: [SIMH] equal, instructions within 0.6%
# (MX_RUN, MX_ISO, MX_XEMU, MX_XISO, MX_STAGE_EXTRA as in CLAUDE.md and docs/testing.md)
MX_XEMU_ARGS="-config_path C:/xemu/xemu.toml -icount shift=1,sleep=off" \
  tools/xbox/xemu_run.sh 1500 '\[GAME\] end banner done'
python3 tools/xbox/icount_report.py base-gl.log new-gl.log      # exit 0; same for fodperf
# lockstep shots: env MX_LOCKSTEP=1 with gl's four SHOT lines, then
cmp base/shot-N.png new/shot-N.png                              # all four
# a release-default build boots and plays: -DXHW_AUTOPAD=1 -DXHW_TEST_BUILD=0, gl to the results
```

plus every host test in `.github/workflows/build.yml`, and `[BEAT]` free
memory in the match within 64 KB of the base (or the difference explained).

## Findings

### F1. What the tree has

- The frame loop already calls the netplay hooks: `pc_net_sync()` before
  each tick, `pc_net_after_tick()` after it (re-runs a tick when it returns
  true), the scene-exit hold, and a scene end when the peer is gone
  (`src/melee/gm/gmscene.c:360-375`, `:422-429`, `:440-447`).
- `GM_ONLINE` is imported whole: lobby -> CSS -> SSS -> VS -> sudden death
  -> results -> CSS, on a private `VsModeData` with two human doors
  (`src/melee/gm/gmonlinemode.c:36-45`, `:159-179`), the LAN lobby's frame
  function (`:960-988`) and its failure words (`:689-692`). Under
  `TARGET_XBOX` it leaves at once (`:816-825`, the `PORT:` bail).
- The lobby screen is SIS text over the main menu's background
  (`src/melee/mn/mnonlinelobby.c:139-165`); the SIS ASCII encoder knows
  only space `' " , - . :` digits and letters (`:25-27`). The ONLINE
  submenu has five entries, all live (`src/melee/mn/mnonline.c:16`,
  `:67-94`).
- Other hooks in imported code, all present: sound starts and "is it still
  playing" (`src/sysdolphin/baselib/axdriver.c:640-650`, `:981-999`), the
  off-screen damage test (`src/melee/if/ifmagnify.c:645-705`), disc
  requests (`src/sysdolphin/baselib/devcom.c:458`,
  `src/melee/lb/lbfile.c:152-166`), the online stage pick
  (`src/melee/mn/mnstagesel.c:201-244`), the HUD line
  (`src/melee/if/ifnet.c:40-64`), the unlock state as one scalar
  (`src/melee/gm/gmmain_lib.c:973-1002`), `pc_net_deterministic()` users
  (`gmtitle.c:181`, `ftwobble.c:47`, `grpstadium.c:1414`, ...).
- Only headers are imported (`src/pc/net*.h`); `xbox/src/sdk/stubs.c`
  answers every hook with "off". `PC_NET_PROTO_VERSION` is 9
  (`src/pc/net.h:35`).
- `MELEE_BOOT_SCENE` knows `unranked|direct|ranked` but not `lan`
  (`src/melee/gm/gmboot.c:49-83`).

### F2. Upstream: which commit, and what the engine is

- **`src/UPSTREAM_COMMIT` is not a melee-pc commit.** It holds
  `1e4b3b5addc74e52e420a86b3dd0da4bb67867cee` (41 hex digits), the
  doldecomp/melee commit melee-pc had synced to; melee-pc carries the same
  file with the same 41 characters. `git fetch` of it from
  `999sian/melee-pc` fails, and none of the repository's 446 commits
  (branches and pull-request heads) starts with `1e4b3b5`. The imported
  `net.h`, `net_internal.h` and `net_lan.h` are blob-identical to melee-pc
  `master` at **`e833835`** (`e83383539a4fd06e9bd8b28c270b3d1ecc7d3be3`,
  2026-09-29, "sdl: build SDL main on Windows, Android and iOS") and the two commits before it (`d8f2384` is "sync:
  port 28 upstream decomp commits (430c286ba -> 1e4b3b5ad)"). The net
  sources are taken from `e833835`. Below, `net*.c`, `vi.c`,
  `launcher.cpp` and `netcode-plan.md` without a path are melee-pc's at
  that commit (`src/pc/`, `docs/`).
- Sizes at `e833835` (`src/pc`, lines): `net.c` 3541, `net_lan.c` 1452,
  `net_snapshot.c` 1042, `net_handshake.c` 757, `net_sync.c` 504,
  `net_sfx.c` 426, `net_wire.c` 314, `net_reliable.c` 231, `net_sim.c` 158,
  `net_watchdog.c` 124, `net_chat.c` 86, `mdns/mdns.h` 1621. Internet-only
  and not needed: `net_match.c`, `net_dht*.c`, `net_identity.c`,
  `net_rank*.c`.
- The engine: two peers, UDP, one input packet per tick carrying every
  unacked frame (up to 32), acks, a stop-and-wait reliable lane for the
  handshake and lobby messages, frame checksums in every input packet
  (pads, seed, and in a fight each fighter's position, facing, percent,
  action, stocks and both velocities: `net_snapshot.c:266-293`), a
  RULES/READY handshake that ships the seed, start frame, `GameRules`, item
  and stage switches and a hash of the forced unlock state, a scene-exit
  hand-off (`net.c:1695-1765`), a resume phase after 3 s of silence, a link
  simulator, a keyed-BLAKE2b tag on every datagram.
- **It has a lockstep-only mode.** A build whose linker cannot bracket the
  game's statics runs the whole session without prediction:
  `s_lockstep = snapshot_state_region_missing() != NULL` at session start
  (`net.c:1787-1790`, `net_snapshot.c:536-542`), then every tick waits for
  the peer's real input (`net.c:3047-3063`). `MELEE_NET_ROLLBACK=off` does
  the same by hand (`net.c:2117-2127`).
- **It runs one tick per present.** `pc_net_pace_adjust_ns()` pins the raw
  pad queue to one sample per frame boundary (`net_sync.c:361-378`,
  `:401-408`), "the loop runs exactly one tick per present". Fine at 60 fps
  on a PC; on the console any stage under 60 fps would play in slow motion.
  The pin is called by the platform's frame boundary (upstream
  `vi.c:349`), so the Xbox decides whether it happens.
- Local input is read with `PADRead` at tick time and always from physical
  port 0 (`net.c:2964-2970`); ports 3 and 4 are "no controller"
  (`net.c:2662-2684`). One player per machine is built into the wire
  format (`WirePad pads[REDUNDANCY]`, `net_internal.h:174-190`) and into
  `net.local`/`net.remote` (21 uses: 15 in `net.c`, the rest in
  `net_handshake.c`, `net_sync.c`, `net_wire.c`), which do two jobs: the
  peer's index (host 0, guest 1: session id, handshake roles, the datagram
  header's `player` byte) and the game port its pad goes to (`write_head`,
  `net.c:2662-2684`; the ring dump, `:1251`). D5 splits the two.
- Game side, one player per side is assumed in three places only:
  `onEnterLobby`'s two human doors (`gmonlinemode.c:173-178`), the stage
  pick ordered by `pc_net_local_player()` (`mnstagesel.c:229-250`, `:753`:
  one pick per peer, which stays right with two players a console) and the
  HUD line's `P%d vs P%d` (`ifnet.c:59-62`). The CSS, teams, results and
  sudden death are vanilla VS code driven by the four queue ports, so four
  humans need no change there (**unverified** for the online CSS's door
  handling; phase 3 checks it).
- Autopad scripts drive port 1 only (`xhw_autopad.c:169-172`).
- In a fight on a LAN the automatic delay is 1 frame, on the assumption
  that rollback covers the rest (`net_sync.c:206-213`). Without rollback
  that is wrong; menus (lockstep) get trip + 1.
- A desync is logged and shown in the HUD line; play goes on
  (`net.c:1280-1304`). `PC_NET_PEER_DESYNC` is reserved and never set.
- Dependencies (what the port has to supply or drop):

  | upstream uses | where | on the Xbox |
  |---|---|---|
  | receive thread (`SDL_CreateThread`) | `net.c:1162-1174`, `:2175-2180` | none: the fallback path receives on the game thread when the thread is NULL |
  | 4 ms transmit timer (`SDL_AddTimer`): resend, keep-alive during loads, reliable resend | `net.c:404-439` | one timer thread from `xhw_thread_start`; needed, a load must not look like a dead peer |
  | mutexes, `stdatomic.h` (bool/int only) | `net.c:149-158` | `xhw_mutex`; clang's own header |
  | BSD/Winsock: `socket bind sendto recvfrom select/poll getaddrinfo getsockname setsockopt ioctlsocket` | `net_internal.h:83-116`, `net.c:1997-2100` | a shim header over `xhw_udp_*` (IPv4, numeric addresses); TOS dropped |
  | IPv6 (`sockaddr_storage`, `AF_INET6`) | `net.c:1981-2053`, `net_lan.c` | compiled out |
  | clocks: `SDL_GetTicksNS`, `SDL_Delay*`, `SDL_GetPerformanceCounter` | throughout | `xhw_time_ns`, `xhw_sleep_ms`, the socket's event wait |
  | C++ | none in `net*.c`; `pc_install_id`/`pc_app_rev` live in `launcher.cpp:1856-1859` | C, in the sdk |
  | allocation | snapshots (`realloc`, `net_snapshot.c:644`), interface lists in `net_lan.c` | none on the path that is kept; ~0.3 MB of static rings |
  | file I/O | record/replay/state log (`net_snapshot.c:78-112`) | dropped; state lines go to the log on a desync |
  | BLAKE2b (Monocypher) | session key and 8-byte tag (`net_wire.c:240-282`) | no key in v1: the tag field stays, zero, as upstream's own pre-handshake state (D10) |
  | xxHash | disc identity (`net_lan.c:234`), sync test | FNV-1a over the same bytes |
  | CSPRNG (`BCryptGenRandom`, `getrandom`) | handshake nonces (`net_handshake.c:57-113`) | a tick/TSC/MAC mix (freshness only) |
  | mDNS (`mdns.h`, multicast, `getifaddrs`) | `net_lan.c` | replaced by UDP broadcast (D3) |
  | `signal`, `pthread` | `net_watchdog.c` | the Xbox watchdog |
  | `getenv` (about 35 knobs) | throughout | autopad `env` lines in test builds, NULL in a release (`xhw_autopad.c:104`) |
  | `aurora_dvd_inflight`, `aurora_arq_inflight` | `net.c:2878-2911` | exist (`xbox/src/sdk/dvd.c:328`, `ar.c:267`) |

- Upstream's own host fixtures exist and build without the game:
  `tools/test_net_reliable.c`, `test_net_handshake.c`, `test_net_sfx.c`,
  `test_net_resume.c`, `test_net_magnify.c`.

### F3. The Xbox platform, as it bears on netplay

- Ticks: one per queued pad sample, up to 5, then one render
  (`gmscene.c:407-432`). The pad alarm is an `OSAlarm` on the SDK clock;
  alarms run on the game thread whenever it re-enables interrupts, at most
  every ~0.3 ms by TSC, and at the frame boundary
  (`xbox/src/sdk/os.c:338-350`, `:446-479`); a late 1/60 s alarm catches up
  to 10 periods (`os.c:455-466`). The frame boundary paces presents to
  60 Hz and never slows the tick clock (`xbox/src/sdk/vi.c:67-98`).
- **Disc completions run on the worker thread**, under the interrupt lock,
  at whatever moment the read finishes (`xbox/src/sdk/dvd.c:237-278`;
  `xsdk_dvd_deliver()` is empty, `:331`). ARAM completions run on the ARQ
  worker or on the game thread, whichever comes first
  (`xbox/src/sdk/ar.c:248-254`, `:311-324`). The game's blocking waits poll
  through `pc_os_yield()` (`src/melee/lb/lbfile.c:39`, `lbdvd.c:30`).
- Alarm users in game code are three files: `lb_0195.c` (the pad alarm),
  `lbmthp.c` (movies), `lbmemory.c`.
- `aurora_heap_extent()` returns the whole allocation arena for any heap
  (`os.c:295-300`): enough to link, useless for a snapshot.
- `PADRead` polls all four ports and handles the dashboard-reset combo
  (`xbox/src/sdk/pad.c:75-127`); `pc_net_rumble_command` lives in `pad.c:147`.
- Leaving the XBE goes through two functions that already stop the LED,
  the AC97 and the pads before `XLaunchXBE` (`xbox/src/hw/xhw_main.c:140-160`);
  a device left running across a relaunch has broken the next boot before
  (decisions.md, "The AC97 is left idle before a relaunch"; `xhw_pad.c:141`).
- The watchdog shows its hang screen after 6 s without a frame boundary
  and reports after 10 s without a present; `xhw_watchdog_busy(1)` holds
  both off (`xbox/src/hw/xhw_watchdog.c:204`, `:239-265`). A netplay wait
  can last 3 + 3 s.
- CPU text into the finished frame exists (`xgx_set_overlay`,
  `xbox/include/xgx.h:180-196`, `xhw_overlay.c`): no GPU frame needed.
- `XBOX_CFLAGS` reaches the sdk and hw targets, not the game objects
  (`tools/xbox/compile_game.py:54-63`). `src/pc/audio.c` and `misc.c` are
  built in the sdk target (`xbox/CMakeLists.txt:63-72`); the SDL3 shim
  there is audio only (`xbox/include/sdk/SDL3/SDL.h`). The structs the net
  sources read (`GameRules`, `GamePrefs`, `src/melee/gm/types.h:142`,
  `:230`; the `Fighter` fields `simhash.c` already reads) are not
  `DISC_STRUCT`, so they need no lowering.

### F4. nxdk's network stack at the pin (58427c0)

- `lib/libnxdk_net.lib` is built by the standard `make NXDK_ONLY=y ...`
  (nxdk `Makefile:78`), so the Docker image, CI and this PC have it. lwIP
  2.2.1 plus `nvnetdrv`.
- `lib/net/nforceif/include/lwipopts.h`: `LWIP_IPV6 1` (`:44`),
  `LWIP_DEBUG 1` (`:46`), `NO_SYS 0` (`:60`, so a tcpip thread),
  `LWIP_TCPIP_CORE_LOCKING 1` (`:76`), the heap is the kernel pool
  (`MEM_CUSTOM_ALLOCATOR`, `:117`; `MEMP_MEM_MALLOC 1`, `:131`;
  `MEM_SIZE 16000`, `:144`, unused with that), `LWIP_DHCP 1` (`:250`),
  `LWIP_AUTOIP 1` (`:260`), **`LWIP_IGMP 0`** (`:283`), TCP, netconn and
  sockets on (`:324`, `:420`, `:430`). Not set, so lwIP's defaults:
  `PBUF_POOL_SIZE` 16, `LWIP_DHCP_AUTOIP_COOP` 0, **`LWIP_MDNS_RESPONDER`
  0**. The brief's "IGMP, the mDNS responder, MEM_SIZE 10240,
  PBUF_POOL_SIZE 120" do not describe this pin.
- **The NIC drops multicast in hardware.** `nvnetdrv_init` writes all-ones
  to both multicast address and mask registers and
  `NVREG_PFF_ALWAYS_MYADDR` to the filter (`lib/net/nvnetdrv/nvnetdrv.c:394-399`):
  own address and broadcast only. mDNS would need a patched driver and a
  rebuilt lwIP.
- Receive: ISR -> DPC -> `tcpip_input` (`nvnetdrv.c:117-130`,
  `nvnetdrv_lwip.c:70-86`); 64 receive buffers of 2 KB in contiguous
  memory (`nvnetdrv_lwip.c:27-33`, `nvnetdrv.h:15`). `sys_thread_new`
  ignores the priority (`nforceif/src/sys_arch.c:68-74`), so the tcpip
  thread runs at the game thread's priority.
- `nxNetInit` blocks up to 10 s for DHCP and returns -2 without a lease
  (`lib/nxdk/net.c:118-129`); nothing starts AutoIP. `nxNetShutdown` is
  empty (`:157-160`), but `nvnetdrv_stop()` disconnects the interrupt,
  resets the NIC and frees its memory (`nvnetdrv.c:469-500`), and
  `nvnetdrv_stop_txrx()`/`start_txrx()` pause it.
- lwIP's threads don't run under the port's crash guard, so they must
  never touch MEM1 or ARAM (committed on fault, architecture.md).

### F5. Numbers for rollback

- Game statics: `.data` 194 KB + `.bss` 1768 KB over 1009 objects
  (`llvm-size` on `build-xbox/game`, the 2026-10-01 build). The units
  upstream leaves out of a snapshot (`melee_state.ld`: audio, perf, video,
  the pad alarm, `devcom.c`) hold 1177 KB of that (`devcom.obj`'s `.bss`
  alone is 1,048,833 bytes), leaving ~0.77 MB.
- Heaps: upstream measured 5.6 MB a snapshot on a PC, 2.2 MB of it statics
  (melee-pc's `netcode-plan.md:50`, `:155`), with 8-byte pointers. On the Xbox 2-3 MB
  of live heap was the guess; phase 0A's `[NETM]` in xemu measured about
  6.7 MB (2026-10-04). So ~7.5 MB a snapshot, 8 slots (`SNAPS`,
  `net_internal.h:125`): ~60 MB against 6.8-9 MB free in a match
  (`fps-plan.md:397`, `architecture.md:74-76`); D2 (no rollback) holds by
  a wider margin than planned.
- Copying 3-4 MB on the console: 10-25 ms (**unverified** estimate from
  150-300 MB/s for uncached copies; phase 0A times it). A tick period is
  16.7 ms.
- A tick costs 1.84 ms on Final Destination 1v1 and about 3.3 ms on
  Fountain with four fighters (`fps-plan.md:339-349`: 4.9 ms a frame at
  ~1.5 ticks). Seven re-run ticks: 13-23 ms, on a machine that already
  renders busy stages at 40 fps.

### F6. Determinism (roadmap item 10)

- On the console, runs of one build part at tick 4440-4500 on Fountain
  4-CPU seed 1 into the same three outcomes, the seed first
  (`roadmap.md:119-131`, `fps-plan.md:263-267`). xemu under `-icount`
  was thought never to part; a real-time xemu run parted once
  (`fps-plan.md:698-702`). **Phase 0A (2026-10-04): `det` twice under
  `-icount` parts at tick 4447** (first `[SIMH]` difference at 4500), so
  item 10 reproduces in xemu. One run has 8 more HUD damage-shake draws
  and no RNG difference before them: the render-written off-screen flag
  (item 3 below) is the first suspect, ahead of the audio answers.
- What upstream found on PCs, each a timing input to the simulation, each
  already behind a hook in the imported game code:
  1. "is that voice still playing" decides RNG draws in `crowdsfx.c` and
     `ground.c`; each machine's mixer clock answers differently
     (`netcode-plan.md:2074-2092`). In a session the answer is "finished"
     (`axdriver.c:994-998`).
  2. A sound start returns -1 when the voice pool is full, and the game
     branches on it; in a session it returns a handle made of frame and
     call index (`src/pc/net_sfx.h:8-16`).
  3. The off-screen damage tick read a flag a render callback writes
     (`ifmagnify.c:645-651`). On the Xbox ticks per render vary with
     timing, so that flag is stale by a varying number of ticks.
  4. The title screen stirs the RNG by wall clock; scene-entry draws land
     between ticks (`net.c:2817-2862`).
  5. How many ticks a load takes depends on the disc; upstream settles
     every transfer before each tick (`net.c:2878-2911`) and agrees the
     scene-exit frame.
- Xbox-specific on top: completions that run on worker threads mid-tick
  (F3) and reads of heap memory that was never written (stack locals are
  zeroed, heap cells are not).
- Ranking for item 10, before measuring: (1) the audio answers (the seed
  parts first, and three outcomes fit a voice ending one tick earlier or
  later), (2) completion timing, (3) the render-written flag, (4) heap
  garbage. Phase 0A's trace names the first differing draw.

### F7. Corrections to the brief, in one place

| the brief | what was found |
|---|---|
| upstream pinned in `src/UPSTREAM_COMMIT` | that is doldecomp's commit; melee-pc `e833835` matches the imported headers (F2) |
| nxdk's lwIP has IGMP and the mDNS responder on | both off; the NIC filters multicast (F4) |
| `MEM_SIZE 10240`, `PBUF_POOL_SIZE 120` | 16000 (unused: kernel pool), default 16 (F4) |
| pacing hooks only need calling from the Xbox frame boundary | calling them pins one tick per present, which the console can't afford (F2) |
| LAN discovery "mDNS over IPv4 and IPv6" can be ported | not on this driver without patching it (F4, D3) |

### F8. xemu

- The xemu on this PC has the UDP-tunnel and pcap network back ends (the
  config keys `bind_addr`, `remote_addr`, `netif` are in `xemu.exe`);
  `C:\xemu\xemu.toml` has no `[net]` section, so networking is off today.
  Two instances with `[net] backend = 'udp'` and swapped
  `[net.udp] bind_addr`/`remote_addr` on 127.0.0.1 make a virtual cable
  (**unverified**; phase 0B's first step). There is no DHCP server on that
  cable, so it also exercises AutoIP.
- Each instance needs its own HDD image and its own EEPROM: the EEPROM
  holds the MAC, and two equal MACs break ARP and the host election.
- The PC has 16 logical CPUs; one instance runs the standard match at
  ~33 fps. Two at once is **unverified** (phase 0B measures; the watchdog's
  "frames stopped" during slow loads is not a hang, handoff.md).
- `-icount sleep=off` skips idle guest time, so a 3 s network timeout
  passes in no host time while the other instance is still computing: pair
  runs use real time or `-icount shift=1` with sleep on.
- Where xemu lies: timing (TCG, softfloat; ~2 ticks per render where the
  console may have 1), determinism (it hides item 10 under `-icount`), the
  NIC (no hardware multicast filter, no cable to pull, link always up),
  disc speed (instant), audio (APU path), memory headroom. Cable pull,
  power-off, link timing, DHCP on the real LAN and the audio check are
  console-only.

### F9. The two consoles (the user, 2026-10-04)

- Console A: the development console (`MX_FTP_HOST`, UnleashX), 720p,
  480p and 480i.
- Console B: **480i only**. Its address comes from the user when the first
  two-console step needs it (S1 part 2); its dashboard is still open; it
  plays the same disc image file as A. The two consoles meet through an
  ordinary network switch (DHCP from the router, D3's first path); a
  crossover cable (AutoIP) is tested later, in S4.
- B therefore always draws on the 640x480x32 path, interlaced
  (`xhw_video.c:60-66`); A at 720p draws R5G6B5 + Z16 (`architecture.md:96`).
  Every two-console match pairs two render paths, so anything the render
  writes that a tick reads (F6 item 3) shows as a desync between A and B
  even when two runs on one console agree. Phase 2's proof therefore
  includes a 720p-against-480i comparison, not only 16:9 against 4:3.
- 480i costs what 480p costs to draw (the same 640x480 target); B's frame
  rate is A's at 480p. Four fighters on busy stages run about 40 fps at
  720p on A (`fps-plan.md`), so 2v2 runs more ticks per present than 1v1
  on both.
- Thin SIS text flickers on an interlaced picture; the lobby has to be
  readable at 480i on B's TV (phase 3 shot, S3 by eye).
- Two players a console means two controllers on each: four for 2v2 in S3
  and S4.

## Decisions

**D1. Port melee-pc's engine, lockstep only; replace its discovery,
snapshot and watchdog files with Xbox ones.** Imported into `src/pc` from
`e833835`: `net.c`, `net_wire.c`, `net_reliable.c`, `net_handshake.c`,
`net_sync.c`, `net_sfx.c`, `net_sim.c` (5931 lines), built in the sdk target
next to `audio.c`. Not imported: `net_lan.c` (mDNS), `net_snapshot.c`
(snapshots, record/replay), `net_watchdog.c`, `net_chat.c`, everything
internet. About 120 lines of `PORT:` edits, about 3000 new lines (socket and
SDL shims ~400, checksum/state file ~250, glue ~350, LAN lobby ~600, hw net
layer ~450, tools and tests ~900).
Why not a smaller engine of our own (~2500-3000 lines plus the same 2000 of
platform, lobby and tools): the game-side hooks are written against this
engine's exact semantics (frame numbering, scene hand-off sequence numbers,
sound handles, reliable message types for the stage pick, peer status
codes), its lockstep path is a supported mode, upstream's document records a
dozen desync causes it already fixed (pad-queue slips, seed draws between
ticks, handshake staleness, BYE races), and its host fixtures come with it.
A rewrite saves ~3000 imported lines and gives all of that up. The two-pad
format of D5 ends compatibility with melee-pc's v9 peers (out of scope
anyway); the code stays close enough that a later melee-pc sync is a merge,
not a rewrite.

**D2. Delay-based lockstep, no rollback.** F5: a snapshot ring needs ~60 (measured heap; 24-32 planned)
MB where 7-9 are free, one snapshot copy costs most of a tick, and a
seven-frame re-run costs more than a frame on a CPU that already misses 60
fps. The engine's lockstep path needs none of it. Default input delay 2
frames (33 ms), `[lan] delay = auto|1|2|3` in `settings.ini`, the host's
value wins (`REL_DELAY`). The LAN round trip is under 1 ms; what the delay
has to cover is the two consoles' tick bursts (two or three ticks, then a
25 ms render) and their unsynchronised pad clocks. Phase 4 measures stalls
at 1, 2 and 3 and may bring the default to 1 by sending each sample when
the pad alarm takes it instead of when its tick runs. Ticks per present
stay as offline (D12). Kept open: nothing here blocks rollback later
(upstream's PE section scheme, `melee_state_pe.c`, fits `coff_text_plain.py`'s
pattern), it just isn't planned.

**D3. Discovery by IPv4 UDP broadcast; DHCP with AutoIP fallback; the
network starts when the LAN lobby opens.** F4 rules mDNS out on the stock
driver and library. Each console in the lobby broadcasts one announce a
second, and at once on entering, to 255.255.255.255 on UDP 41001 (the
limited broadcast only, D14 rule 8); the game socket is UDP 41000
(upstream's default, `net.c:2038`). The announce is upstream's TXT record as one text
line (`v rev disc id name port players state gen host peer offer`;
`players` is D5's 1 or 2), parsed by the
same strict rules; the election is upstream's (lowest id among the ready
hosts, proposal acknowledged before the session opens,
`net_lan.c:1141-1247`). Address: a manual address from the console's
configuration sector if one is set (as `nxNetInit` does), else DHCP; no
lease after 4 s: AutoIP (169.254.x.y) while DHCP keeps trying. Two consoles
on one crossover cable therefore find each other with no router. The NIC
and lwIP start on first entry to the lobby, on a worker so the lobby keeps
drawing ("Starting network", "No network cable", "Getting an address",
"Searching"); leaving the lobby pauses the NIC; every way out of the XBE
stops it. Offline boots never touch it. `MELEE_LAN_DIRECT=ip:port` (test
builds) skips discovery, as upstream.

**D4. Determinism first, proved on one console before any two-console
match.** Phase 0A instruments (RNG-draw trace per tick, timing jitter in
test builds, heap poison, upstream's render audit). Phase 1 gives a
**reflect session**: the socket shim hands every datagram back with the
player byte flipped, so one console runs the whole engine (lockstep waits,
handles, "finished" answers, I/O settle, scene hand-off) against a mirror of
itself. Phase 2 makes the simulation a function of seed and inputs while a
session is up: the upstream answers (F6 1-5), completions delivered only
before a tick and inside the game's own blocking waits, whatever the trace
finds. The proof: eight console runs of Fountain 4-CPU seed 1 in a reflect
session give the same `[SIMH]` lines to the match's end, while the same
build without the session still parts (the control: a run without the fix
has to fail before a fixed run counts). In xemu the same under injected
jitter. Whether the console's lines also equal xemu's is recorded, not
required: double-precision math through x87 instructions (`fsin`, `fpatan`,
`fyl2x` in pdclib) need not round alike in xemu and on a Pentium III.
Offline play is not changed by any of it; a cause that turns out to be an
offline port bug goes to the user first.

**D5. One or two players per console, on controller ports 1 and 2 (the
user's decision, 2026-10-04).** Built in from phase 1, not added after:
the wire format and the port map are cheapest to get right before the
lobby, the tests and the console sessions are built on them.
- Local players: the controllers in a console's ports 1 and 2 when its
  START is pressed in the lobby (port 1 must have one). The count, 1 or 2,
  is in the lobby announce (`players`, D3), fixed for the session, and
  checked again in the handshake (RULES carries both counts, READY echoes
  them; a mismatch fails the connect). A controller plugged in later joins
  at the next lobby; one pulled mid-match sends neutral input, as an
  offline unplug does.
- Game ports: the host's players first, then the guest's, packed: 1+1 is
  ports 1-2 (upstream's layout), 2+2 is 1-2 against 3-4, 2+1 is 1-2 and
  3, 1+2 is 1 and 2-3. Unused ports are "no controller", as upstream.
- Engine: `net.local`/`net.remote` keep the peer index (host 0, guest 1);
  new `net.port_base[2]`, `net.count[2]` carry the port map. The input rings
  hold two pads a frame per side; `write_head` fills ports from the map;
  `capture_local_sample` reads physical ports 1 and 2 (`PADRead` already
  polls all four). Physical port n of a console is its n-th local player
  everywhere, rumble included (`pc_net_rumble_command` maps game port to
  physical port).
- Wire: every frame entry carries two `WirePad`s (the second neutral for a
  one-player console), each delta-coded as today (`pads_encode`, at most 9
  bytes): `PACKET_WIRE_MAX` goes from 315 to 603 bytes (`net_internal.h:292`),
  under one Ethernet frame; the link simulator's `Held` buffers grow with it
  (128 x ~0.6 KB). `Rules` and `Ready` gain the two counts. Their
  `_Static_assert`s (`net_internal.h:278-288`) are updated, not removed.
  `PC_NET_PROTO_VERSION` becomes 109 (v9's engine, the Xbox's two-pad
  layout), so a melee-pc v9 peer is refused as "other version", never
  misparsed.
- Game side (`PORT:` edits): the CSS's human doors from the port map at
  session start instead of `onEnterLobby`'s fixed two; the stage pick stays
  one per console (the console's first port picks, `pc_net_local_player()`
  returns that port); the HUD line names port sets (`P1-2 vs P3-4`); the
  lobby lists each peer with its count. Teams on/off, colours and
  free-for-all are the vanilla CSS. CPU players on free ports would be
  deterministic simulation like any other, but are not required; phase 3
  records whether the online CSS lets one be set, and they stay off if not.
- Cost: ~300 more lines than one player (engine ~150 in `PORT:` edits,
  game ~60, lobby ~40, autopad port 2 ~50), a protocol of our own, twice
  the pair matrix (1+1, 2+2, 2+1, 1+2), and four fighters on screen more
  often (F9: more ticks per present, more exposure to stalls; phase 4's
  delay table is measured with four).

**D6. Identity and compatibility.** Name: `XBOX-` plus the last four hex
digits of the MAC, or `[lan] name` in `settings.ini` (15 characters of the
SIS set). Id (the election key): a 64-bit FNV-1a of the MAC and the EEPROM
serial, stable without a file. Build (`rev`): a hash of the running image's
code, computed when the lobby opens, so two consoles play only with the
same XBE. Disc (`disc`): upstream's inputs (disc header, FST entry count,
the DOL's bytes; `net_lan.c:210-237`) through FNV. A peer that differs in
protocol, build or disc is listed as "other version" and never elected
(`mnonlinelobby.c:251-254`), with one log line naming both values. The FP
control words are logged at connect (`net.c:2181-2198`).

**D7. Disc I/O.** Three mechanisms, all upstream's, with Xbox work in the
second: the scene-exit hand-off (both leave on max of the two requests +
20 frames), a settle before every tick (no tick starts with a transfer the
game issued still in flight), and the keep-alive timer so a console inside
a long load is "loading", not "gone" (120 s cap). On the Xbox the settle
must run the completions itself on the game thread, must not wait for the
music stream's reads (issued by the mixer thread; they touch no simulation
state), and worker-thread delivery is off while a session is up (D4). The
preload cache and the disc-backed ARAM pages change how long a load takes,
not what it loads; a cold console against a warm one is a phase 3 test.
`pc_net_note_io` and the "pure load" list only matter to rollback.

**D8. Failure handling.** All of these end in the lobby (the B button goes to the
menu), with upstream's "Failed: ..." line there:

| event | noticed by | on screen | log |
|---|---|---|---|
| cable pulled | the link state, polled; 3 s of silence opens the 3 s resume window (a replug inside it resumes the match) | during the wait "Waiting for the other Xbox" (CPU overlay), then "Failed: network cable unplugged" | `[NET] link down`, `net: peer silent for 3000 ms`, `net: disconnected ... (status 2)` |
| peer powered off | the same silence, about 6 s | "Failed: connection lost - Connection timed out" | as above on the survivor |
| peer quit (reset combo, dashboard) | its BYE | "Failed: peer left - Peer left", at once | `net: peer left (reason 1)` |
| desync | frame checksums | "Failed: ... - Desync" on both (the Xbox ends the session; upstream plays on) | `net: DESYNC at frame N` and the state dump on both |
| other build or disc | the announce | peer listed as "other version", START does nothing | `lan:` line with both values |
| no cable / no address at lobby entry | link state, DHCP/AutoIP | the lobby's status line | `[NET]` lines |

Waits pet the watchdog (`xhw_watchdog_busy`) and draw the overlay after
0.5 s. Every exit path stops the NIC before `XLaunchXBE`.

**D9. Gameplay features in a LAN session.** UCF off, free camera off,
frozen stadium off: the build stays vanilla (decisions.md "Vanilla
gameplay"), both consoles run the same XBE by D6 so nothing has to be
forced to agree, and UCF answers GameCube-stick variance, which says nothing
about Duke and S sticks. Unlock-all **is** forced, in RAM, for the session,
and put back at disconnect, as upstream does (`net_handshake.c:286-303`):
unlock predicates gate RNG draws, so two different saves would desync on
the first frame. It must never reach the card: phase 4 adds a guard in the
card writer and a before/after hash of the save. Kept open:
`pc_is_ucf_enabled()` is one function (`xbox/src/sdk/features.c:10`).

**D10. No datagram authentication in v1.** The 8-byte tag stays in the
format, zero, which is upstream's own state before its handshake derives a
key; the Xbox never derives one. Two consoles on a living-room LAN gain
nothing from it, keyed BLAKE2b in 32-bit code is an estimated 2-3% of a
frame on this CPU (**unverified**), and Monocypher would be new vendored
code. Kept open: the field is there.

**D11. nxdk's prebuilt lwIP, driven directly.** Link `libnxdk_net.lib`,
don't call `nxNetInit` (it blocks and has no AutoIP): `tcpip_init`,
`netifapi_netif_add(nvnetif_init)`, DHCP and AutoIP by hand, lwIP's raw UDP
API under `LOCK_TCPIP_CORE`, received datagrams copied into a ring in the
XBE's own memory, the tcpip thread's priority raised from a `tcpip_callback`.
No sockets layer in the path. Build lwIP ourselves (own `lwipopts.h`, as
`mx_pbkit` is built from nxdk's sources) only if S1 shows one of: under
5 MB free with the stack up, receive latency over 4 ms that the priority
doesn't cure, the 128 KB contiguous allocation failing.

**D12. Ticks per present stay as offline.** The Xbox frame boundary does
not call the queue pin; a tick consumes one queued pad sample and one frame
of synced input, up to five a present. Time-sync skips and advances are
off by default (they assume the pin); lockstep's own back-pressure keeps
two consoles together, and their clocks differ by parts per million.

**D13. On the VS. Mode menu, ONLINE is replaced by LAN PLAY** (the user,
2026-10-04). The entry's label becomes "LAN PLAY" (`mnonline.c:30-33`
answers "ONLINE" today) and selecting it enters the LAN lobby at once
(`enterOnline(ONLINE_KIND_LAN)`) instead of opening the ONLINE submenu
(`mnmain.c:2627-2629`): Direct Connect, Ranked, Unranked and Profile are
never shown on the Xbox, and nothing else enters `GM_ONLINE`. Both are
`PORT:` edits in phase 3. Until phase 3 lands, the ONLINE entry on `dev`
leads nowhere (the `PORT:` bail, F1); hiding it before then is an offline
menu change, so it is the user's call (open question 3).

**D14. Network conduct: what the console sends on someone's LAN, and the
standards it keeps** (the user, 2026-10-04: "airtight", no harm to the
network). Every rule below is enforced in code, checked by a host test or
by `net_audit.py` on a capture, and any departure is a stop-and-ask.

1. *Off means silent.* An offline boot never starts the NIC: no DHCP, no
   ARP, nothing (G0's "no `[NET]` line" plus a capture of an offline boot
   with no frame from the console's MAC). Every way out of the XBE stops
   the NIC before `XLaunchXBE` (R4).
2. *Link layer.* The console's own MAC from the EEPROM. The second xemu
   EEPROM gets a locally administered unicast MAC (IEEE 802: U/L bit set,
   I/G bit clear; `eeprom_mac.py`).
3. *ARP and address conflicts* (RFC 826, RFC 5227): lwIP's ACD is on at
   the pin for DHCP addresses and AutoIP (`LWIP_DHCP_DOES_ACD_CHECK`,
   `LWIP_ACD` defaults, `opt.h:955`, `:1059`). An address that fails the
   check is never used: a DHCP address is declined (DHCPDECLINE, RFC 2131
   §3.1.5), a link-local one replaced. A conflict found later (another
   host claims our address) is defended or given up as RFC 5227 §2.4 says
   (lwIP's ACD), and the lobby shows "Address conflict".
4. *DHCP* (RFC 2131, RFC 2132): lwIP's client, unchanged, with nxdk's
   XDK-style client identifier (`lwipopts.h:474-479`); its own
   retransmission backoff. Link down then up: `dhcp_network_changed`
   (INIT-REBOOT, RFC 2131 §3.2). A manual address from the console's
   configuration sector is used as is, with the ACD check first. The lease
   is not released at exit (RFC 2131 allows it; the dashboard reuses it).
5. *Link-local* (RFC 3927): AutoIP only without a manual address and with
   no lease after 4 s; lwIP's probe and announce timing untouched;
   addresses in 169.254.1.0-169.254.254.255 only. DHCP keeps trying; a
   lease that arrives is used for every new session and announce, a
   running session ends on the address it started on (RFC 3927 §1.9).
6. *IPv4 only* (the user: IPv6 is out of scope). The netif is never given
   an IPv6 address, which is lwIP's default when nobody asks for one, so
   the console sends no IPv6; no extra work beyond not enabling it. No
   IGMP, no multicast.
7. *UDP* (RFC 768, RFC 8085): checksums generated and checked (lwIP
   defaults, `opt.h:2386`, `:2421`). Fixed ports, the same for source and
   destination: 41000 game, 41001 discovery, both in the IANA registered
   range and unassigned (40854-41110 is free in the registry, checked
   2026-10-04). No datagram over 1200 bytes (`xhw_udp_send` refuses it; the
   largest is D5's 603-byte input packet), so nothing is ever fragmented
   (RFC 8085 §3.2). IP TTL 1 on every datagram we send: the game is
   link-local by design, and a datagram that would need a router dies at
   the first one.
8. *Broadcast* (RFC 919, RFC 922, RFC 1812 §5.3.5): discovery uses the
   limited broadcast 255.255.255.255 only, never the subnet's directed
   broadcast (on one link it reaches the same hosts; a router configured to
   forward directed broadcasts, RFC 2644, could carry it further). One
   announce a second, one more on entering the lobby, never more than two
   in any second, under 200 bytes, only while the lobby is open and the
   address is up; nothing is broadcast in a match. A broadcast is never
   answered by a broadcast, and nothing answers a datagram with more bytes
   than it carried until the handshake has seen both sides (no
   amplification).
9. *Who we talk to.* Only on-link peers: a source or destination outside
   our subnet (or outside 169.254/16 when we are link-local) is dropped,
   `MELEE_LAN_DIRECT` included. Datagrams from our own address, from
   0.0.0.0, a broadcast or multicast source, port 0, or of the wrong length
   or magic are dropped before any parser sees them. Unicast goes only to
   an address that announced itself or opened a session; after a session
   ends (BYE, timeout, desync) nothing more is sent to that peer.
10. *Rates and the circuit breaker* (RFC 8085 §3.1, RFC 8084). A session
    sends one input packet a tick (≤ 5 a present, ~60 a second), acks, the
    reliable lane and keep-alives; ~40 KB/s each way at 2+2. Changes from
    upstream (`PORT:` edits in phase 1): keep-alives while the game thread
    is inside a load go out at 10 a second, not every 7 ms (`net.c:404-439`;
    the peer's silence limit is 3 s); the reliable lane's fixed 250 ms
    resend (`net_reliable.c:44`) backs off exponentially to 2 s; the
    mid-frame resend after a loss stays as upstream (at most one per 7 ms).
    Independently of the engine, `xhw_net.c` enforces a ceiling per socket
    (token bucket: 250 datagrams and 256 KB a second, bursts of 32): excess
    is dropped and counted (`[NET] tx governor dropped N`), so no engine bug
    can flood a LAN. The silence limits (3 s, then 3 s of resume) are the
    breaker that stops a session talking to a peer that has gone.
11. *Everything else lwIP does* stays standard: ICMP echo replies and port
    unreachables (RFC 1122 §3.2.2), TCP resets to connection attempts (no
    listener is ever opened).

Verification, in the phases' gates: `tools/xbox/net_audit.py <pcap>` (phase
0B; standard library only) checks a capture against rules 1-10: per-source
rates in every one-second window and the governor never reached in normal
play, sizes, no IP fragments, TTL 1, broadcast only to
255.255.255.255:41001 at most twice a second, no multicast or IGMP frame
from a console's MAC, IP and UDP checksums valid, ARP probe and announce
timing for AutoIP (RFC 3927 §2.2) and ACD (RFC 5227 §2.1), the DHCP
exchange well formed, nothing to a peer after its BYE or timeout.
Captures come from `tools/xbox/xemu_tap.py` (phase 0B: a relay between the
pair's two UDP tunnel endpoints that forwards each Ethernet frame and
writes it to a pcap; no driver needed) and on the hardware from Windows'
own `pktmon` on the PC, on the same switch (an administrator prompt, the
user's: `pktmon start --capture`, `pktmon stop`, `pktmon etl2pcap`); it
sees broadcasts, ARP, DHCP and the console's traffic to `lan_probe.py`.
A host fuzz test (`test_net_fuzz.py`, phases 1 and 3) feeds random and
mutated datagrams to every parser (announce, input, ack, reliable,
handshake): no crash, no read out of bounds (built with
`-fsanitize=address,undefined`), no state change from a rejected one.

## Architecture

```
game (imported)   gmscene.c hooks, gmonlinemode.c lobby, axdriver.c, ifmagnify.c ...
      |  pc_net_* / pc_lan_* / net_sfx_*            (src/pc/net.h, net_lan.h, net_sfx.h)
sdk, game triple  src/pc/net.c net_wire.c net_reliable.c net_handshake.c net_sync.c
                  net_sfx.c net_sim.c                 imported, PORT: edits
                  xbox/src/sdk/net_lan.c              broadcast discovery, election (pc_lan_*)
                  xbox/src/sdk/net_state.c            frame checksum, state ring, render audit;
                                                      "no snapshot region" -> lockstep
                  xbox/src/sdk/net_xbox.c             identity, overlay, watchdog, fixtures
                  xbox/src/sdk/net_sock.[ch]          BSD-shaped shim, reflect mode
                  xbox/src/sdk/sdl3_net.c             SDL ticks, mutex, timer thread
      |  xhw_net_* / xhw_udp_*                       (xbox/include/xhw.h: scalars, byte buffers)
hw, nxdk triple   xbox/src/hw/xhw_net.c               lwIP raw UDP, DHCP/AutoIP, link state,
                                                      datagram rings, log stream
                  libnxdk_net.lib                     lwIP 2.2.1 + nvnetdrv (tcpip thread, ISR/DPC)
```

The boundary, to be fixed in phase 0B:

```c
int  xhw_net_start(void);            /* lazy, returns at once; a worker brings the link up */
void xhw_net_pause(int on);          /* leaving / entering the lobby */
void xhw_net_shutdown(void);         /* every exit path, before XLaunchXBE */
int  xhw_net_state(void);            /* OFF, NO_CABLE, CONFIG, UP, FAILED */
uint32_t xhw_net_ip(void);           /* network order; 0 until UP */
uint32_t xhw_net_bcast(void);
void xhw_net_ident(uint8_t mac[6], uint8_t serial[12]);
int  xhw_udp_open(uint16_t port);    /* handle, -1 */
void xhw_udp_close(int h);
int  xhw_udp_send(int h, uint32_t ip, uint16_t port, const void* p, uint32_t n);
int  xhw_udp_recv(int h, void* p, uint32_t cap, uint32_t* ip, uint16_t* port);  /* -1: none */
int  xhw_udp_wait(int h, uint32_t ms);                                          /* 1: one is queued */
```

Threads while a session is up: the game thread (ticks, receive, send), the
timer thread (keep-alive and reliable resend, priority +1), lwIP's tcpip
thread (+1), the NIC's DPC. Neither of the last two touches lazily
committed memory.

One tick in a session: capture the local samples (physical ports 1-2) for
frame + delay and send everything unacked; settle game-issued disc and ARAM
transfers; wait (event, 1 ms turns, overlay after 0.5 s) for the peer's
input for this frame; write every player's pad into the queue head through
the port map (D5), unused ports empty; checksum; run the tick; compare
checksums as the peer's arrive.

## Phases

Order: 0A and 0B (independent, either first or side by side), console
session S1, then 1, 2 (S2), 3, 4 (S3), 5 (S4). Gates run in the order host
tests -> xemu single -> xemu pair -> one console -> two consoles.

### Phase 0A: offline probes (no network code)

Entry: this plan approved.

Work:
1. RNG trace, test builds: `HSD_Rand` reports each draw's return address;
   per tick a count and a rolling hash, logged with `[SIMH]` when
   `env MX_RAND_TRACE=1`; `env MX_RAND_DUMP=<a>-<b>` logs every caller in
   that tick range. `tools/xbox/simh_diff.py` takes two or more logs and
   prints the first tick whose hash or draw count differs and the callers
   on each side (symbolized with the build's map).
2. Timing jitter, test builds: `env MX_JITTER=<seed>` adds seeded sleeps of
   0-2 ms in the mixer loop, in the DVD worker before a completion, and at
   the frame boundary. Purpose: make xemu show what the console shows.
3. `[NETM]` at tick 600 and 3600 of a match (test builds): bytes of live
   cells per OSAlloc heap, and the time to copy 1 MB eight times between
   two cold buffers.
4. Scenarios: `det` (Fountain, `cpu4`, seed 1, 120 s, no shots), `v1-<stage>`
   (two CPUs, 60 s) for Fountain, Stadium, Yoshi's Story, Dream Land,
   Battlefield, Final Destination, Corneria, Peach's Castle. Console round
   7 in `console_round.py`: a warm-up, `det` six times with the trace, the
   eight `v1-*` at 720p, three of them at 480p, `det` once at 480i
   (`env MX_VIDEO=480i`: console B's mode, F9).
5. `docs/README.md` gets this file's row.

Files: `src/sysdolphin/baselib/random.c` (`PORT:` one call),
`xbox/src/sdk/simhash.c`, `xbox/src/sdk/os.c`, `xbox/src/sdk/dvd.c`,
`xbox/src/sdk/sdl3_audio.c`, `xbox/src/sdk/vi.c`, `xbox/src/hw/xhw_sys.c`,
`xbox/include/xhw.h`, `tools/xbox/simh_diff.py`, `tools/xbox/console_round.py`,
`tools/xbox/scenarios/det`, `v1-*`.
Switches: `env MX_RAND_TRACE`, `MX_RAND_DUMP`, `MX_JITTER` (autopad, test
builds). Docs owed: `testing.md` (switch table, `[NETM]`, `simh_diff.py`),
`decisions.md` (the `random.c` edit), this file (F5, F6 with the numbers).

Gate:
- G0. The trace only observes: `[SIMH]` of `gl` equal with and without
  `MX_RAND_TRACE=1`.
- `det` twice under `-icount`: `python3 tools/xbox/simh_diff.py a.log b.log`
  prints `equal through tick 7200`.
- `det` four times in real time with `MX_JITTER=1..4`: record whether any
  two part, and where (a finding, not pass/fail).
- `console_round.py 7 stage` produces the round for S1.

### Phase 0B: the Xbox network layer, a probe, two xemu instances

Entry: this plan approved. Independent of 0A.

Work:
1. `xbox/src/hw/xhw_net.c` behind the boundary above (D11), with the
   address sequence of D3, the link state polled a few times a second with
   `nvnetdrv_is_link_up()` (the prebuilt library's link callback is taken,
   and lwIP's is not compiled in), `xhw_net_shutdown()` in
   `xhw_quit_to_dashboard` and `launch`. Test builds:
   `env MX_LOG_UDP=<ip>:<port>` sends every log line as a datagram from the
   log's flush thread; `tools/xbox/lan_logd.py` writes one file per source
   address with host time stamps.
2. A probe, test builds only (`env MX_NETPROBE=1`): at boot it starts the
   layer, logs `[NETP] link up after N ms`, `dhcp a.b.c.d after N ms` or
   `autoip ...`, broadcasts a beacon each second on 41001 and logs each
   source it hears, and pings any peer 60 times a second for 60 s during a
   match (`rtt min/avg/p99/max, lost`). `tools/xbox/lan_probe.py` is the
   same peer on the PC.
3. Two xemu instances: `tools/xbox/xemu_pair.sh <scenario-a> <scenario-b>
   <secs>` (two work folders, two configs with the UDP tunnel on
   127.0.0.1:9368/9369, two serial logs, kills its own two), and
   `tools/xbox/eeprom_mac.py` to make the second EEPROM from a copy of the
   user's with another MAC and a fixed checksum. The second HDD image and
   EEPROM stay in `C:\xemu\b`.
4. Network conduct (D14), all of it that lives in the hw layer: TTL 1,
   the 1200-byte limit, the on-link and source filters, the transmit
   governor, ACD results acted on, `dhcp_network_changed` on link up.
   `tools/xbox/net_audit.py` and `tools/xbox/xemu_tap.py`;
   `tests/xbox/test_net_gov.c` (the token bucket and the filters against
   a model).
5. `tools/xbox/console2.py`: `stage|deploy|pull vNN` for consoles A and B
   (`MX_FTP_HOST`, `MX_FTP_HOST_B`), logs into `logsNN-a`, `logsNN-b`.

Files: `xbox/src/hw/xhw_net.c`, `xhw_netprobe.c` (new), `xhw_main.c`,
`tools/xbox/net_audit.py`, `xemu_tap.py`, `tests/xbox/test_net_gov.c`,
`tools/xbox/test_net_gov.py`,
`xhw_sys.c` (log tee), `xbox/include/xhw.h`, `xbox/CMakeLists.txt` (lwIP
include paths for `mx_hw`, `libnxdk_net.lib`), `tests/xbox/test_net_ring.c`,
`tools/xbox/test_net_ring.py`, `xemu_pair.sh`, `eeprom_mac.py`,
`lan_probe.py`, `lan_logd.py`, `console2.py`, `scenarios/netprobe`,
`.github/workflows/build.yml` (the new host test).
Switches: `env MX_NETPROBE`, `MX_NETPROBE_FLOOD`, `env MX_LOG_UDP`. Docs owed: `platform.md` (a
"Network" section), `testing.md` (switches, "Two xemu instances", "Two
consoles"), `decisions.md` (D3, D11), `toolchain.md` (the link line),
`architecture.md` (memory table), `LICENSE.md` (lwIP's BSD and nvnetdrv's
MIT notices, now linked in).

Gate:
- Host: `python3 tools/xbox/test_net_ring.py` (the datagram ring against a
  model, one writer and one reader, random sizes and overruns).
- G0 with the library linked and never started: no `[NET]` line in an
  offline run; image size and `[BEAT]` free memory deltas quoted.
- xemu, NAT back end, `MX_NETPROBE=1` with `lan_probe.py` on the host:
  `[NETP] dhcp 10.0.2.15`, an `rtt` line, a `[BEAT]` after the probe.
- xemu pair: each log has `[NETP] autoip 169.254.` and `[NETP] beacon from
  169.254.` within 5 s of `[NETP] link up`. If the tunnel doesn't work,
  try the pcap back end on a loopback adapter, record which worked, and if
  neither does say so here: pair gates then move to console sessions.
- Host: `python3 tools/xbox/test_net_gov.py`; `net_audit.py` against
  hand-made pcaps, one per rule, each rule's violation caught.
- xemu pair through `xemu_tap.py`: `net_audit.py` clean over the whole
  run (AutoIP probe/announce, ARP, beacons, pings), and an offline boot
  through the tap with no frame from its MAC (D14 rule 1). A probe run
  with `env MX_NETPROBE_FLOOD=1` (test builds: the probe tries 2000
  datagrams a second) shows `[NET] tx governor dropped` and a capture
  that never exceeds the ceiling.
- Relaunch: a `NEXT` chain of two folders with the probe on boots the
  second cleanly in xemu.

### Console session S1 (after 0A and 0B; ~45 min)

One test build carrying both phases (`-DXHW_AUTOPAD=1`), one deploy.
Part 1, console A alone, unattended (round 7 and the probe run, PC running
`lan_probe.py` and `lan_logd.py`). Part 2, consoles A and B (needs B's
address, asked for then; ~20 min): the probe on both through the
switch (the crossover cable waits for S4), and `det` with the trace twice on B at
480i (B's frame rate, and whether B's runs part where A's do: a second
console's view of item 10). What needs the hardware, and why xemu can't
answer it:

| measurement | console | why not xemu |
|---|---|---|
| item 10 reproduced with the RNG trace: the first differing draw and its caller | A | `-icount` never parts; real time rarely |
| 1v1 frames per second and ticks per render, eight stages, 720p and 480p; four fighters (`det`) at 720p, 480p, 480i | A, B (480i) | xemu's speed says nothing about the console's |
| `[NETM]`: live heap bytes, 1 MB copy time | A (heap bytes also in xemu) | memory speed |
| link-up and DHCP times, AutoIP on a cable without a router | A, B | no PHY, no real DHCP server |
| broadcast received by the real NIC, both directions | A + PC, A + B | xemu's NIC has no hardware filter |
| RTT and jitter at 60 packets a second during a match; fps with the stack up | A + PC, A + B | host scheduling, not the console's |
| free memory with the stack up; the contiguous allocation | A | close in xemu, confirmed here |
| audio with the NIC interrupting (one `[AUDIO] AC97 polled` line, the user's ear) | A | xemu uses the APU path |
| return to the dashboard and the next boot after the NIC ran | A | xemu forgives a running device |

Network conduct on the real LAN: the user runs `pktmon` on the PC during
part 1's probe run and part 2 (five-line instruction in the hand-over),
then `net_audit.py` on both captures must be clean: DHCP with the real
router, ARP and ACD, broadcasts, the probe's unicast to `lan_probe.py`.

Results go into F4-F6 and the Status table; D11's conditions and D2's
numbers are settled here.

### Phase 1: the engine, lockstep only, reflect session

Entry: 0B merged (the boundary exists). S1 not required.

Work:
1. Import the seven files of D1 from melee-pc `e833835` into `src/pc`, add
   them to `SDK_SOURCES`, record the commit in `decisions.md`'s sync
   section (and that `src/UPSTREAM_COMMIT` is doldecomp's).
2. Shims: `net_sock.[ch]` (the socket calls `net.c` makes, over
   `xhw_udp_*`; `getaddrinfo` for dotted IPv4), SDL headers and
   `sdl3_net.c` (ticks, delays, mutex, one timer thread, `SDL_CreateThread`
   returning NULL).
3. `PORT:` edits, each small and listed in `decisions.md`: the socket-shim
   branch in `net_internal.h`; IPv6 and TOS out; no session key (D10);
   nonces from the tick/TSC/MAC mix; the wait loop blocks on
   `xhw_udp_wait` and calls the Xbox hook (watchdog, overlay later);
   `pc_net_rumble_command` stays `pad.c`'s; time sync measures only (D12);
   a lockstep-only build takes the lockstep delay in fights too; D14
   rule 10's two rate changes (keep-alives at 10 a second during loads,
   the reliable lane's backoff).
4. Two players a console (D5), in the same edits: the peer index split from
   the port map (`net.port_base`, `net.count`), two-pad rings and
   `write_head` through the map, `capture_local_sample` from physical
   ports 1-2, the two-pad wire entry and `PC_NET_PROTO_VERSION` 109, the
   counts in `Rules`/`Ready`, `pc_net_local_count()` next to
   `pc_net_local_player()` in `net.h` (`PORT:`), the ring dump naming
   ports. `env MX_NET_LOCAL=1|2` (test builds) sets the local count where
   the lobby will later. Autopad drives port 2 too: a `P2` token in front
   of a line's buttons (`600 P2 A`), port 1 when absent, so old scripts
   are unchanged.
5. `net_state.c`: `frame_checksum`, the 64-frame state ring and its dump,
   `pc_net_render_audit`, and the snapshot, record and sync-test entry
   points as "not available". `net_xbox.c`: `pc_install_id`, `pc_app_rev`,
   watchdog stubs, `pc_net_init` from boot. `stubs.c` loses the symbols
   that are now real (`pc_lan_*` stay stubs until phase 3).
6. Reflect session, test builds: `env MX_NET_REFLECT=1` opens a session at
   boot against the shim's mirror (player byte flipped), with the handshake
   completed locally (unlock forced, rules in force, start frame = next).
   Scenario `netloop`: `MELEE_BOOT_SCENE=vs`, `cpu4`, Green Greens, 60 s.
   Scenario `netloop2`: Green Greens, 60 s, `MX_NET_LOCAL=2`, ports 1-2
   human and scripted by autopad, ports 3-4 the mirror's copies of them.
7. `test_net_fuzz.py` (D14): the engine's receive path and parsers under
   random and mutated datagrams, with the sanitizers.
8. Host tests from upstream's fixtures, built against the imported files:
   `test_net_reliable.py`, `test_net_handshake.py`, `test_net_sfx.py`; and
   `test_net_wire.py` (ours): the two-pad entry encodes and decodes for
   counts 1 and 2 and every port map of D5, a truncated or over-long
   packet is rejected whole, a v9 header is refused.

Files: `src/pc/net.c`, `net_wire.c`, `net_reliable.c`, `net_handshake.c`,
`net_sync.c`, `net_sfx.c`, `net_sim.c` (new), `src/pc/net_internal.h`,
`xbox/src/sdk/net_sock.c`, `net_sock.h`, `sdl3_net.c`, `net_state.c`,
`net_xbox.c` (new), `stubs.c`, `boot.c`, `xbox/include/sdk/SDL3/*.h`,
`xbox/CMakeLists.txt`, `src/pc/net.h` (`PORT:` `pc_net_local_count`),
`xbox/src/sdk/pad.c` (rumble port map), `xbox/src/hw/xhw_autopad.c` (`P2`),
`tests/xbox/test_net_*.c`, `tools/xbox/test_net_*.py`,
`tools/xbox/scenarios/netloop`, `netloop2`, `.github/workflows/build.yml`, `CLAUDE.md`
(test list).
Switches: `-DXSDK_NET=0` (the stubs again, to bisect), `env MX_NET_REFLECT`,
`env MX_NET_LOCAL`, the autopad `P2` token,
and upstream's knobs now reachable from autopad scripts (`MELEE_NET_DELAY`,
`MELEE_NET_SIM_LOSS`, `MELEE_NET_SIM_DELAY_MS`, `MELEE_NET_AUDIO_DEAF`,
`MELEE_NET_SFX_LOG`, `MELEE_NET_RENDER_AUDIT`, `MELEE_NET_RECONNECT_MS`,
`MELEE_NET_STALL_TEST`, `MELEE_NET_EXIT_AFTER_FRAMES`). Docs owed:
`decisions.md` (D1, D2, D10, D12 and the edits), `testing.md` (switches, the
`net:` lines worth reading), `architecture.md` (the "not built" paragraph),
`platform.md` ("Not built").

Gate:
- Host: the four new tests and all the old ones.
- **G0 with the engine compiled in and no session.** This is requirement 8.
- xemu, `netloop` with `MX_NET_REFLECT=1`: the log has `net: handshake done`,
  a `net: frame 600, rollbacks 0` line with `pad slips 0`, `empty 0` and
  `idle ticks 0`, `[SIMH]` lines to the match's end, `[PERF]` ticks per
  render above 1 (the multi-tick path ran), and no `net: DESYNC`.
- xemu, `netloop2`: the same lines, `net: ports 1-2 local, 3-4 remote` at
  connect, the ring dump with four ports, and a `[SIMH]` that changes when
  only the port 2 script changes (port 2's input reaches the game).
- The same run twice under `-icount`: `simh_diff.py` says equal.
- `-DXSDK_NET=0` builds and passes G0. Image and free-memory deltas quoted.

### Phase 2: determinism on one console

Entry: phase 1 merged; 0A's trace results from S1 read (which draw parts
first on the console).

Work:
1. Settle on the Xbox: reads the game thread issued, and the requests their
   completions chain, are tagged; before each tick of a session the game
   thread waits for those and runs their completions itself; the music
   stream's reads are not waited for. While a session is up the workers
   queue completions for the game thread, which takes them at the settle
   and inside `pc_os_yield`/`pc_os_wait_alarm`, never from
   `OSRestoreInterrupts`.
2. Whatever S1's trace named, fixed under `pc_net_deterministic()` (a
   `PORT:` edit where it is game code). If it is one of F6 1-4, the engine
   already covers it and the work is the proof.
3. Checks that find the rest: `MELEE_NET_RENDER_AUDIT=1` (the render must
   not write what a tick reads), `env MX_HEAP_POISON=<byte>` (test builds:
   fill each OSAlloc cell on allocation), a 16:9 720p-path run against a
   4:3 run (`-DXHW_VIDEO_480_BPP=16` and a staged `settings.ini`), and a
   720p run against a 480i run (`MX_VIDEO=480i`): the pairing every
   A-against-B match has (F9).
4. `env MX_NET_DET=<bits>` (test builds) turns single parts off again, to
   attribute the cause: 1 audio answers, 2 sound handles, 4 settle and
   queued completions, 8 the off-screen test.
5. Scenario `det-net` (`det` plus `MX_NET_REFLECT=1`) and console round 8:
   warm-up, `det-net` eight times (four with `MX_NET_LOCAL=2`), `det`
   twice (the control), `det-net` once at 480p and once at 480i.
6. A list, for this file, of the simulation's calls into pdclib's double
   math (`sin`, `cos`, `atan2`, `pow`, `exp`, `log`): they are x87
   instructions there, the one place two machines could round differently
   (R16), and the reason console and xemu hashes may differ.

Files: `xbox/src/sdk/dvd.c`, `ar.c`, `os.c`, `net_xbox.c`, `net_state.c`,
`src/pc/net.c` (`PORT:` the settle), the game file of the cause if any,
`tools/xbox/console_round.py`, `scenarios/det-net`.
Switches: `env MX_NET_DET`, `MX_HEAP_POISON`. Docs owed: `decisions.md`
(the deterministic mode, D4, D7), `roadmap.md` item 10 (cause and state),
`testing.md`, this file.

Gate:
- G0.
- xemu, real time: `det-net` with `MX_JITTER=1..4`: `simh_diff.py` over the
  four logs and the `-icount` log says equal through the last tick. The
  control (`det`, same four seeds) is recorded; if it never parts in xemu
  the console carries the proof alone.
- xemu: render audit run with no hit; poison `0x00` against `0xCC` equal;
  the 16:9 run equal to the 4:3 run; the 720p run equal to the 480i run.
- **Console (S2, console A, ~35 min unattended):**
  `tools/xbox/console_round.py 8 report` and `simh_diff.py logs-r8/boot_*`:
  the eight `det-net` runs equal to each other through the match's end,
  and the 480p and 480i runs equal to them (and whether they equal xemu's
  `-icount` log, recorded); the two `det`
  runs part (expected at tick 4440-4500). If the eight are not equal,
  `MX_RAND_DUMP` around the first differing tick names the caller and this
  phase repeats; no two-console match before it passes.

### Phase 3: lobby and discovery

Entry: phases 1 and 2 merged, S1's network facts in (broadcast works on the
hardware, address times).

Work:
1. `xbox/src/sdk/net_lan.c`: `pc_lan_*` with D3's announce and upstream's
   state machine and timeouts (announce 1 s, lost after 5 s, election 100
   ms after START, 15 s limits), the READY barrier with the scene check,
   `pc_lan_connect_direct`. Identity per D6 in `net_xbox.c`. The local
   count (D5) taken at START from ports 1-2, sent in the announce,
   frozen for the session.
2. Game side, `PORT:` edits: the lobby bail narrowed to the non-LAN kinds
   (`gmonlinemode.c:816-825`), the lobby's status line from the Xbox's
   network state where upstream speaks of mDNS and Direct Connect
   (`gmonlinemode.c:752-768`), ONLINE replaced by LAN PLAY on the VS. Mode
   menu (D13, `mnonline.c:30-33`, `mnmain.c:2627-2629`), `MELEE_BOOT_SCENE=lan` (`gmboot.c`). For D5: the
   CSS's human doors from the session's port map (`onEnterLobby`'s fixed
   two, `gmonlinemode.c:173-178`, set again at session start), the lobby
   row with each peer's player count, the HUD line with port sets
   (`ifnet.c:59-62`), the stage pick by the console's first port. Record
   whether the online CSS lets a free port be set to CPU and whether that
   CPU plays identically on both (D5: allowed only if it does).
3. Test fixtures: `env MX_LAN_AUTO=1` presses START once a compatible peer
   is listed; autopad lines relative to a scene's entry in session frames
   (`@css+30 SR for 20`), so each instance's script drives its own ports
   1 and 2 (the `P2` token) through the character and stage select on
   frames both peers share; if scripted menus prove flaky in xemu,
   `env MX_LAN_PICK=<ckind>[,<ckind>],<stage>`, plus `MX_LAN_TEAMS=1`
   for a teams match.
   `env MX_NET_HUD=0` hides the HUD line for shot comparisons.
4. `tools/xbox/lan_diff.py a.log b.log`: lines up two consoles' logs by
   session frame (`net: scene`, `online: enter`, `[SIMH]`, the 600-frame
   report) and prints the first difference.
5. `settings.ini` `[lan] name`, `delay`.

Files: `xbox/src/sdk/net_lan.c` (new), `net_xbox.c`, `stubs.c`,
`settings.c`, `xsdk_settings.h`, `src/melee/gm/gmonlinemode.c`,
`src/melee/mn/mnonline.c`, `src/melee/mn/mnmain.c`, `src/melee/gm/gmboot.c`,
`xbox/src/hw/xhw_autopad.c`, `tests/xbox/test_lan_record.c`,
`tools/xbox/test_lan_record.py`, `tools/xbox/lan_diff.py`,
`src/melee/if/ifnet.c`, `scenarios/lanflow-a`, `lanflow-b`, `lan22-a`,
`lan22-b`, `lan21-a`, `lan21-b`.
Switches: `MELEE_BOOT_SCENE=lan`, `env MX_LAN_AUTO`, `MX_LAN_PICK`,
`MX_LAN_TEAMS`, `MX_NET_HUD`, `MX_LAN_ID` (an id override for instances that share an
EEPROM), `MELEE_LAN_DIRECT`. Docs owed: `platform.md` ("LAN play": lobby,
discovery, identity, `[lan]`), `testing.md` (pair scenarios, the autopad
lines, `lan_diff.py`), `decisions.md` (D3, D6, D13, the edits), `README.md`
(how to play over a LAN, one or two players a console on ports 1-2, same
build and disc).

Gate:
- Host: `python3 tools/xbox/test_lan_record.py`: announce encode/parse
  round trip, every malformed record rejected whole, and an election model
  (two and three peers, every order of START including simultaneous) ends
  with exactly one host, the lowest id; the port map for every pair of
  counts and both host orders.
- G0.
- xemu pair, `tools/xbox/xemu_pair.sh lanflow-a lanflow-b 600`, then
  `python3 tools/xbox/lan_diff.py a/serial.log b/serial.log`: both logs
  have a `lan: peer` line for the other within 3 s of `lan: started` (the
  Xbox file keeps upstream's `lan:` wording); `lan: match start
  seed=S start_frame=F` with the same S and F as P1 and as P2; the same
  frame in `lobby: entering CSS at frame`, `online: enter VS at frame` and
  `online: enter RESULTS at frame`; identical `[SIMH]` lines; the same
  `[GAME] match ends: outcome`; a second `online: enter VS` (the rematch)
  with identical `[SIMH]` again; no `net: DESYNC`, no `pad slip`. The
  match is four stocks (one side walks off, as `scenarios/res` does).
- xemu pair, `lan22-a lan22-b` (two players each, teams, 2v2) and
  `lan21-a lan21-b` (two on the host, one on the guest; then swapped so
  the guest has two): the same lines, the ports in `net: ports` as D5's
  map, four (three) fighters in `[SIMH]`, and the results screen's shot
  the same on both.
- xemu pair, one instance started 5 minutes before the other joins (a warm
  preload cache against a cold one): the same lines.
- Lobby shots (`[FBDUMP]`) at 480p, at 480i (`MX_VIDEO=480i`, console B's
  mode) and on the 16-bit path: text readable, counts shown.

### Phase 4: pacing and failure handling

Entry: phase 3 merged.

Work:
1. Delay: stall counts from the 600-frame report (`stalls N (worst X ms)`)
   at delay 1, 2 and 3 in the xemu pair (about two ticks per render, the
   hard case), for 1+1 and for 2+2 (four fighters render slower, F9). If delay 2 stalls more than once per 600 frames: capture
   each pad sample for its frame and send it when the pad alarm queues it
   (a hook after the alarms in `os.c`, a capture pass in `net.c`), with a
   host model test (`test_net_capture.py`: every frame gets exactly one
   local sample, in order, whatever the queue does) before any xemu run.
2. The wait: watchdog held off, "Waiting for the other Xbox" drawn by the
   CPU overlay after 0.5 s, the link state in the reason string.
3. Desync ends the session with status 3 on both (D8); `env
   MX_NET_DESYNC_TEST=<frame>` (test builds) flips one bit of a fighter's
   percent on one side to prove it.
4. The save: a guard in the card writer while the unlock state is forced,
   and `[CARD] image hash` logged at boot and on leaving `GM_ONLINE`.
5. Quit paths: the reset combo and the dashboard return send BYE first;
   `xhw_net_shutdown` verified in each.
6. Scenarios `lanfail-*` for the pair: peer killed, peer suspended 4 s and
   resumed, 5% loss with 30 ms delay (`MELEE_NET_SIM_*`), forced desync,
   peer quit; the kill and the desync also in a 2+2 match. A controller
   pulled mid-match on a two-player console (`env MX_PAD_UNPLUG=<port>,<frame>`,
   test builds): that player idles, nothing desyncs.

Files: `src/pc/net.c`, `net_sync.c` (`PORT:`), `xbox/src/sdk/os.c`,
`net_xbox.c`, `net_lan.c`, `src/melee/lb/lbcardgame.c` (`PORT:` the guard),
`xbox/src/sdk/pad.c`, `xbox/src/hw/xhw_main.c`, `tests/xbox/test_net_capture.c`,
`tools/xbox/test_net_capture.py`, `tools/xbox/xemu_pair.sh` (kill and
suspend one instance at a frame), `scenarios/lanfail-*`.
Switches: `env MX_NET_DESYNC_TEST`, `MX_PAD_UNPLUG`. Docs owed: `decisions.md` (D8, D9, the
delay default with its numbers, the edits), `testing.md`, `platform.md` (the
failure table), `README.md`.

Gate:
- Host: `test_net_capture.py` if item 1's change is made.
- G0.
- xemu pair: the delay table recorded here; at the chosen default, stalls
  no more than 1 per 600 frames over a whole match.
- xemu pair, each `lanfail-*`: killed peer: the survivor logs `net: peer
  silent for 3000 ms` and `net: disconnected ... (status 2)`, then `[SCENE]`
  shows the lobby, the shot shows "Failed:", the B button then reaches the menu;
  suspended 4 s: `net: resumed at frame`, `[SIMH]` equal afterwards; loss
  and delay: no desync, the match ends; forced desync: both log `net:
  DESYNC`, both in the lobby with "Desync"; peer quit: `net: peer left`
  within a second; the unplug: no desync, the match ends. No `[WDOG]`
  line in any of them.
- `[CARD] image hash` equal before and after a session.
- Every pair run of this phase through `xemu_tap.py`: `net_audit.py`
  clean, and after each failure nothing more sent to the gone peer; the
  governor never reached.
- **Two consoles (S3, the user and a second player present, four
  controllers, ~60 min; A at 720p, B at 480i):** discovery under 5 s on
  both; matches with a rematch at the default delay with no desync
  (`console2.py pull`, `lan_diff.py`): 1+1, 2+2 teams, 2+1, 1+2; the lobby
  readable on B's TV; the stall table at delay 1, 2, 3 for 1+1 and 2+2;
  cable out and back within 3 s (resumes); cable out for 10 s (both in the
  lobby with a message); B powered off mid-match (A in the lobby within
  10 s); the reset combo on A; a forced desync run; return to the dashboard
  and a clean next boot on both; the audio check.

### Phase 5: acceptance, tooling, release

Entry: phase 4 merged, S3 read.

Work: two-console rounds (`console_round.py` chains run on both consoles
through `console2.py`, each run ending in the next folder as today);
`netloop` and `netloop2` added to `pgo_train.sh`'s scenarios and the profile retrained
(game and sdk code changed; committing it needs the user's approval);
documents consolidated (`handoff.md`, `roadmap.md` item 10 and "Netplay",
`README.md`, `.github/release-notes.md`, `CLAUDE.md` layout and tests,
`architecture.md` memory table, this file's Outcome). Files: those, plus
`tools/xbox/console2.py`, `console_round.py`, `pgo_train.sh`,
`xbox/melee.profdata`, `xbox/melee.profdata.txt`.

Gate (S4, both consoles, four controllers, ~75 min, with a release-built
XBE: ThinLTO, PGO, no test tools, and a test build of the same commit for
the logged runs): every line of the Goal with B at 480i and A at 720p,
then A at 480p, and once A at 480i too; each of 1+1, 2+2 teams, 2+2 free
for all, 2+1 and 1+2 at least once; once on a crossover cable with no
router; a 30-minute soak of 2v2 rematches with no desync, free memory flat
and above 5 MB; G0 on the release configuration in xemu; `net_audit.py` clean on a `pktmon`
capture of a 2+2 session and of the lobby, with the release XBE.

Two players a console is not a phase of its own: D5 puts it in phases 1,
3, 4 and 5 (the former phase 6).

## Status

| item | state | result |
|---|---|---|
| plan | written 2026-10-04 | this file |
| D5 players per console | decided 2026-10-04 (the user) | one or two per console, ports 1-2; built from phase 1 |
| console B | 480i only (the user, F9) | address given when S1 part 2 is due (`MX_FTP_HOST_B`); dashboard and disc image still open |
| phase 0A offline probes | paused 2026-10-04: branch `lan-0a-probes` (08c7598, WIP), `HANDOFF-0a.md` | G0 on gl passes; `det` parts under `-icount` (F6); left: jitter runs, final G0, the S1 hand-over |
| phase 0B network layer, probe, xemu pair | paused 2026-10-04: branch `lan-0b-net` (3e91404, WIP), `HANDOFF-0b.md` | host tests pass, +248 KB; NAT: DHCP after 10-12 s, rtt open (question 4); left: pair, tap, flood, relaunch, G0 |
| S1 console session | not run | item 10's first differing draw; 1v1 and four-fighter fps (B at 480i); `[NETM]`; link, DHCP, RTT; pair on the LAN |
| phase 1 engine, two-pad wire, reflect session | not started | |
| phase 2 determinism | not started | |
| S2 console session | not run | eight equal runs, control parts |
| phase 3 lobby and discovery | not started | |
| phase 4 pacing and failures | not started | delay table (1+1, 2+2); default delay |
| S3 console session | not run | |
| phase 5 acceptance, tooling, release | not started | |
| S4 console session | not run | the Goal's eight lines |

## Risk register

| | risk | what it would cost | answer |
|---|---|---|---|
| R1 | the console's simulation still depends on timing after phase 2 | no LAN play: every match desyncs | the trace and the bisect bits find the draw; nothing two-console runs before S2 passes; a desync ends cleanly (D8) |
| R2 | one tick per present is assumed in more of the engine than `pad_queue_pin` | wrong inputs consumed, slips | phase 1's gate runs xemu's ~2 ticks per render with `pad slips 0`, `empty 0` required |
| R3 | the NIC on hardware: broadcast not received, slow link-up, DPC and tcpip thread disturbing the AC97 pump or the frame | no discovery, bad audio | S1 measures all of it before anything is built on top; audio check each build; D11's fallback |
| R4 | the NIC left running across `XLaunchXBE` writes into the next image's memory | corrupted next boot, like the stuck AC97 | `xhw_net_shutdown` in every exit path; relaunch gates in 0B, S1, S3 |
| R5 | memory: the library's code (the archive is 944 KB; what the link keeps is **unverified**, 0B quotes it), 128 KB contiguous for receive buffers late in a session | under the 5 MB floor, or LAN refuses to start | start in the menu scene, quote `[BEAT]`, "LAN unavailable" message, own lwIP build (D11) |
| R6 | two xemu instances can't be networked, or starve each other | pair gates fall on console sessions (more of the user's time) | pcap fallback; S3 and S4 widen |
| R7 | stalls from tick bursts make delay 2 feel bad, or need 3 | play feels laggy | send at sample time (phase 4), the delay table decides |
| R8 | a load completes on different ticks on two consoles (HDD against DVD, warm against cold cache) | desync at scene entry or on Pokémon Stadium | settle + hand-off (D7), the cold/warm pair test, a Stadium match in S3 |
| R9 | the forced unlock state reaches the memory card | a player's save changed for good | the guard and the hash gate (phase 4) |
| R10 | the checksum sees only pads and seed outside fights | a menu divergence shows only at the fight's first frame | `lan_diff.py` on scene frames; test builds can exchange `[SIMH]`-style hashes per 60 frames if it bites |
| R11 | reads of unwritten heap differ between consoles with different histories | rare desyncs that no single-console test shows | the poison check (phase 2); the pair's cold/warm run |
| R12 | upstream moves (protocol 10, changed files) | harder sync | edits are small and marked; the commit is recorded; no need to follow |
| R13 | scripted menus in the pair are flaky | red gates that aren't bugs | `MX_LAN_PICK` |
| R14 | a release (LTO + PGO) console paired with a test build | refused as "other version" | intended (D6); S4 uses one XBE on both |
| R15 | two frames of delay against none offline | players notice | said in the README; `[lan] delay` |
| R16 | x87 transcendental instructions round differently on a console with a replaced CPU | desync between unlike consoles | phase 2 lists the simulation's double-math calls; stock consoles share one CPU; the FP control words are logged at connect |
| R17 | A (720p, R5G6B5) and B (480i, 32-bit) draw differently, and something the render writes reaches a tick | desync in every A-B match that no single-console run shows | the 720p-against-480i equality in phase 2 (xemu and S2); the render audit |
| R18 | four fighters run slower than two, more ticks per present, more stalls | 2v2 feels worse than 1v1, or needs delay 3 | phase 4's table for 2+2; delay per session (the host's `[lan] delay`) |
| R19 | the online CSS assumes two human doors somewhere phase 3 doesn't see | a 2+2 CSS that misbehaves or desyncs | `lan22` and `lan21` in the pair before S3; `lan_diff.py` on CSS frames |
| R20 | thin lobby text flickers or is unreadable at 480i | B's players can't read the lobby | 480i lobby shot (phase 3); by eye on B's TV (S3); larger text if needed |
| R21 | the console misbehaves on someone's network (a flood, a stolen address, broadcasts beyond the link) | the user's LAN disturbed; the feature unusable | D14: rules enforced in the hw layer below the engine, a transmit governor, `net_audit.py` on every pair run and on `pktmon` captures in S1, S3, S4 |

## Open questions for the user

Answered 2026-10-04: one or two players a console (D5); console B is
480i only, its address comes when S1 part 2 needs it (F9); both consoles
play the same disc image file; the consoles meet through an ordinary
network switch, a crossover cable is tested later (S4); the name format is
ours to choose (D6's `XBOX-xxxx` stands).

Still open:
1. Console B's dashboard (only matters for FTP paths in `console2.py`).
2. Unlock-all in RAM during a LAN session (never saved, D9): planned as
   the default because two different saves desync on the first frame;
   the user can still say no before phase 3.
3. D13: hide the ONLINE entry on `dev` now (it leads nowhere until phase
   3), or leave it until LAN PLAY replaces it?
4. (phase 0B) xemu's NAT hands the PC's datagrams in from 127.0.0.1,
   which D14 rule 9 drops, and TTL 1 may die in the NAT: measure RTT in
   the xemu pair (no exception to D14; the plan's choice, unless the user
   wants a test-only exception for the NAT gate).
5. (phase 0B) AutoIP binds at least 10 s after link up (lwIP's ACD
   probes, RFC 3927), and DHCP took 10-12 s in xemu, so Goal 1's "lists
   the other within 5 s" holds only if the NIC starts before the lobby
   opens (at the menu, or when VS. Mode opens), or if the 5 s is counted
   from "address up". Phase 3 decides with the user.
6. (phase 0B) lwIP 2.2.1 at the pin: `autoip_stop` left AutoIP running
   (it later took over the DHCP address) and a router advertisement made
   lwIP send IPv6 with no IPv6 address: both worked around in
   `xhw_net.c`. Not handled yet: a DHCP lease arriving during a
   link-local session changes the address under it (D14 rule 5); phase 3
   holds DHCP back while a session runs.

## Picking this up

Read `CLAUDE.md`, `docs/handoff.md`, then this file. Take the first phase in
the Status table that is not done and whose entry line is met; phases 0A and
0B don't depend on each other. Work on a branch and worktree of your own,
keep to the phase's file list (say so if you have to leave it), run its gate
in the order given, write the results into the Status table and the Finding
they belong to, and stop at the hand-over for a console session with the
five-line instruction. The melee-pc checkout for the net sources, outside
the repository: `git fetch --depth 1 https://github.com/999sian/melee-pc
e83383539a4fd06e9bd8b28c270b3d1ecc7d3be3`.
