# Handoff: state and working notes

The agent's memory
notes don't travel between machines, so what they held is here. Read
`CLAUDE.md`, then this, then `docs/roadmap.md`.

## Where it stands

Updated 2026-10-04 (Windows PC, the frame-rate work).

- **Latest release: v3** (`.github/release-notes.md`). Releases are now
  built with ThinLTO + PGO (`docs/toolchain.md` "Release", `docs/pgo.md`).
- **On the console: v51**, a play build of `dev` after the frame-rate merge,
  built as releases are (plain, `XBOX_LTO=1 XBOX_PGO=xbox/melee.profdata`).
  Stage and map: `C:\xemu\hw\stage-v51`, `C:\xemu\hw\melee_x.v51.map`.
- **Frame-rate work: done** (2026-10-03/04, `docs/fps-plan.md` "Outcome"):
  Fountain of Dreams 4-CPU at 720p 32.0 -> 39.9 fps with the same picture
  and simulation; ThinLTO + PGO +13%, the colour framebuffer's tile region
  +11%, the rest from CPU work. Six console rounds, run as chains of test
  builds (`tools/xbox/console_round.py`, `docs/testing.md` "Console
  rounds"). Retrain the PGO profile after game or sdk changes.
- **Open:** `docs/roadmap.md` "Known issues" and its numbered items (9:
  silent after a crash until a power-off; 10: the simulation isn't
  deterministic on the console).
- **Merge gate** (user's rule): every subagent branch gets the main
  session's code review plus before/after shots before it goes on `dev`:
  lockstep shots (`env MX_LOCKSTEP=1`, `TSHOT`) byte for byte where the
  picture must not change, `[SIMH]` equal where the simulation must not
  (`docs/testing.md` "Comparing builds by screenshot"), and the scenes it
  touches.
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
| `fd2` | Final Destination, 2 CPUs (Fox, Mario), 60 s: the frame-rate plan's light case |
| `relaunch` | AC97 across relaunches: a match, then the XBE relaunches itself at frame 1500, again and again (`-DXHW_AUDIO_APU=0`, `-DXHW_AUDIO_TEST`; testing.md "Audio at boot") |
| `probe`, `probe2` | console round 1 (`docs/fps-plan.md` step 1): Fountain of Dreams / Peach's Castle, 4 CPUs, 5 minutes, ablation windows rotating (`env MX_ABLATE=1`, probe build) |

## Working notes (from the agent's memory)

- **Audio check before every console build** (user rule): list everything
  since the last build with good sound that touches audio, the AC97 driver,
  interrupts, timing or memory layout, and tell the user the risk. A healthy
  console boot logs exactly one `[AUDIO] AC97 polled` line; `halted`,
  `stuck`, `cold reset` or `stuck since boot` lines mean trouble. Before
  it, `[AUDIO] found` (three lines) and `[AUDIO] idle` record the audio
  hardware as found and the idle sequence, and `[BOOT] previous exit` how
  the boot before ended (docs/testing.md "Audio at boot"). xemu uses an APU
  voice, which hides AC97 problems; `-DXHW_AUDIO_APU=0` forces the AC97
  path in xemu, `-DXHW_AUDIO_TEST` simulates a running or stuck engine.
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
