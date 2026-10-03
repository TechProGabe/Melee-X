# Handoff: state and working notes

The agent's memory
notes don't travel between machines, so what they held is here. Read
`CLAUDE.md`, then this, then `docs/roadmap.md`.

## Where it stands

Updated 2026-10-02 (Mac round, v39-v45).

- **On the console: v45** = commit `52ccaff`, a plain (release) build, the
  release candidate (RC2). Stage and map: `~/xemu/hw/stage-v45`,
  `~/xemu/hw/melee_x.v45.map`. The tester copy is
  `~/Downloads/Melee-X-RC2/` (+ `.zip`, with `README.txt`); it goes out only
  after the console checks pass (user's rule: test on our hardware first).
- **v45 over v43:** results winner portrait and 1P Stage Clear freeze frame
  fixed; 720p z-fighting fixed (Z16 depth remap, `-DXGX_Z16_DEPTH_RATIO`);
  Fountain of Dreams reflection skips fighters outside it (4x Fox much
  faster); front LED effects (`led = 1`, menu row); 720p is the default
  where the dashboard allows it, BACK held at boot gives 480i and saves
  `720p = 0`, `progressive = 0`.
- **Console results:** v43 ran 60 min on Fountain + 47 min on Peach's
  Castle (480i) without the GPU stall, then a whole-system freeze at 113 min
  uptime (no fault, no hang report; same as v39). v44 at 480 and 720p:
  clean, LED works, winner portrait fixed, Fountain faster, 720p textures
  right. v45 not checked yet: first boot in 720p, BACK-at-boot 480i, then a
  60-min 720p burn-in (Fountain, 4 CPUs, items) - pull its logs.
- **Frame-rate work (2026-10-03):** planned, nothing implemented and no code
  changed. `docs/fps-plan.md` has the measurements (the console runs at
  ~0.3-0.5 instructions per cycle, so stalls come before instruction
  counts), the plan and, under "Picking this up", the prompt the executing
  session starts from. Next: its step 0 (tools), then the probe build for
  one console round.
- **Open, in `docs/roadmap.md` "Known issues":** memory leak since v39
  (~55 KB/min in 192 KB steps, `[BEAT]` free), long-uptime whole-system
  freeze, Pokémon Stadium screen flicker (cause found; the first fix broke
  Stage Clear), Peach's Castle Bullet Bill stuck + endless quake, Fire
  Flower flame missing, Corneria Arwing cutscene silent, trophy lighting,
  100-Man freeze (tester, v42; retest on RC2).
- **Merge gate** (user's rule): every subagent branch gets the main
  session's code review plus before/after FBDUMP shots (gl vs the v43
  baseline `~/xemu/mc/v43_00{1,2,3}.png`, and the scenes it touches) before
  it goes on `dev`. Gate script and shot diff helper: see the session notes
  in the agent memory; scenarios `res`, `clear`, `toy` are new.
- **Releases build on GitHub** (user's rule): the `build` workflow with a
  `release` tag builds, tests, packages and publishes (`docs/toolchain.md`
  "Release"). The repo stays private until the user says to make it public.
  Never commit or push without the user's go-ahead; `dev` is the working
  branch, `main` is releases only.

## Windows setup

The native MSYS2 build, nxdk and xemu on Windows are in
`docs/toolchain.md`. On the PC used so far: MSYS2 in `C:\msys64`, nxdk in
`C:\xdev\nxdk`, xemu in `C:\xemu\`, per-build stage folders, maps and logs
in `C:\xemu\hw` (`MX_HW=/c/xemu/hw`).

- **Console**: `MX_FTP_HOST=<the Xbox's IP> MX_HW=/c/xemu/hw
  tools/xbox/console.py stage|deploy|pull vNN`. `stage` copies the build
  and writes `<map>.statics` (static functions, for the profiler); `pull`
  fetches `boot*.log`, `trace.log`, `crash.log`, `hang.log` and the shots
  into `logsNN`. `deploy` deletes the console's old logs and shots, so pull
  first. FTP is served by the dashboard, so it's down while the game runs.
- **xemu** needs a memory card for anything past the title: an autopad
  build seeds `card_a` from `D:\card_a\*.gci` on the disc (stage the user's
  100% save with `MX_STAGE_EXTRA`); the save itself stays out of the repo.
  On an x86 PC xemu runs the standard match at ~33 fps (v33 on).

## Scenarios (`tools/xbox/scenarios/`)

Autopad scripts for `MX_STAGE_EXTRA` (need an `-DXHW_AUTOPAD=1` build).
`env` lines set game switches, `<frame> SHOT|BACK|<button>` lines act.

| dir | what |
|---|---|
| `gl` | standard smoke/perf run: Green Greens 4-CPU 60 s, shots mid-match, TIME!, results |
| `fi`, `fod`, `fodperf` | Fountain of Dreams 4-CPU (perf; `fodback`: BACK dumps on a trace build) |
| `mc10` | Mute City 4-CPU |
| `bb` | Big Blue 4-CPU (stage 24; the v35 assert) |
| `ps`, `ps2` | Pokémon Stadium 4-CPU, long (transformations; the old GPU-hang soak) |
| `gi` | Stadium, Kirbys vs Falcon with capsules/crates (`MELEE_DEBUG_VS_ITEMS=3`) |
| `gk` | human Kirby with Falcon's hat, Falcon Punch at frame 200 |
| `corn`, `gg`, `gj` | Corneria, Green Greens 20 s, stage 12 |
| `movie` | intro movie shots |
| `settings` | title screen: the settings menu (BACK), a few rows changed, saved, reopened, Save and restart; read `[MENU]`/`[SETTINGS]` |
| `inv` | Fountain of Dreams 4-CPU, players 1-2 cloaked (`MELEE_DEBUG_VS_INVISIBLE=3`: indirect refraction) |
| `res` | results screen with a winner: Fox (port 1) walks off Final Destination in a 15 s match, Mario wins (portrait EFB copies) |
| `tie` | results screen with all four CPUs tied for 1st after an 8 s Final Destination match: the cards' portraits (Debug VS only; `fn_80179854` deviation) |
| `clear` | Classic stage 1 won at once (`MELEE_INSTANT_WIN`): the Stage Clear screen's sepia freeze frame |
| `toy` | Classic's last stage won at once, Game Clear, START: the trophy fall (`gmregtyfall.c`, scene 15 of mode 21; a second START goes on to the credits) |

## Working notes (from the agent's memory)

- **Audio check before every console build** (user rule): list everything
  since the last build with good sound that touches audio, the AC97 driver,
  interrupts, timing or memory layout, and tell the user the risk. A healthy
  console boot logs exactly one `[AUDIO] AC97 polled` line; `halted`,
  `stuck` or `cold reset` lines mean trouble. xemu uses an APU voice, which
  hides AC97 problems; `-DXHW_AUDIO_APU=0` forces the AC97 path in xemu.
- **Test build or release build**: BACK screenshots and the on-screen
  counter exist only in test builds (`-DXHW_PROF=1`, `-DXHW_AUTOPAD=1` or
  `-DXHW_TEST_BUILD=1`). A console round that needs shots or a profile gets
  `XBOX_CFLAGS=-DXHW_PROF=1`; a release candidate is a plain build.
- **Pull logs before the next launch**: every boot deletes `boot*.log`, so
  a session that was restarted keeps only its last boot's log (the v36
  playtest's first half is gone). Ask the user to pull, or pull yourself,
  before they launch again.
- **Bundle hardware tests**: one console round per batch of fixes; the
  user plays and presses BACK (test builds) for screenshots. Deploy test
  builds whenever the user says the Xbox is on. Ask for BACK shots rather
  than descriptions.
- **One emulator at a time.** Several xemu instances starve the CPU and the
  slow loads trip the watchdog's "frames stopped" (not hangs). Call a hang
  only when `[BEAT]`'s retrace count stops for minutes. xemu also locks up
  now and then on its own; rerun before suspecting the build.
- **Never capture the user's desktop**; look at frames only through
  `[FBDUMP]` screenshots (autopad `SHOT`).
- **Perf work**: pick changes that help the console. xemu's profile is
  skewed (it runs SSE through softfloat, so float code looks hot) and its
  GPU is slow; trust `[PERF]`/`[PROF]` from the console. Profiler and
  FBDUMP output cause visible hitches.
- **Simulation changes must keep the bits**: HSD animation and matrix
  rewrites are checked by `test_anim_mtx.py`; prefetches and memos that
  give the same results are fine.
- **Console-only rendering bugs** (GPU ordering, state xemu doesn't model):
  verify on the console with BACK on a `-DXGX_DEBUG_TRACE` build (draws go
  to trace.log; boot.log continues in boot2/boot3.log).
- **Zero-init vs GameCube stack**: game code is compiled with
  `-ftrivial-auto-var-init=zero`; an HSD local read before it's written was
  the previous call's stack on the GameCube and is 0 here (black capsules,
  grey Kirby helmet: `HSD_TExpSetReg`). Check for this before blaming the
  renderer.
- **Byte-swapped disc structs**: a struct read from disc data that isn't
  `DISC_STRUCT` reads garbage (Big Blue's platforms at x = -4e8, Corneria's
  item scale). A crash or assert on one stage only: check that stage's
  `gr*.c` param structs first.
- **Autopad buttons** name Duke buttons: Duke A = GC A, Duke X = GC B
  (specials), Duke B = GC X (jump), Duke Y = GC Y. Repro switches:
  `MELEE_DEBUG_VS_CHARS=<ckind>[:<color>][h],...`, `MELEE_DEBUG_VS_ITEMS=<hex>`,
  `MELEE_DEBUG_VS_INVISIBLE=<hex>`,
  `MELEE_DEBUG_KIRBY_HAT=<FighterKind>`, `XGX_SKIP=<a>-<b>` (trace builds).
  Shot timing differs between builds.
- **Console**: FTP at the console's IP (`MX_FTP_HOST`), `xbox`/`xbox`,
  reachable only from the dashboard. The dashboard is UnleashX, which
  caches icons by title ID under `E:\UDATA\<id>\TitleImage.xbx`: a wrong
  icon is a stale cache (old nxdk builds used `ffff0002`, which belongs to
  every nxdk title; Melee-X is `4d580001`). The console's save is the
  user's 100% save, a Dolphin `.gci` kept off the repo. Its copy on the
  console logs `[CARD] save data looks mixed` (older builds rewrote some
  fields little-endian); reimport the Dolphin original if counters or play
  time look wrong. Never restore the corrupted copies in `card-*` backups.
- **Dolphin reference** (unfinished): an isolated user dir with a Gecko code
  forcing the attract stage; blocked at the memory card prompt; next idea
  was a real save in `GC/USA/Card A`.
- Mac-only leftovers not copied: per-build maps v1-v31 and logs (summaries
  are in the roadmap).
