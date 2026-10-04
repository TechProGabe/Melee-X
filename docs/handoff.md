# Handoff: state and working notes

The agent's memory
notes don't travel between machines, so what they held is here. Read
`CLAUDE.md`, then this, then `docs/roadmap.md`.

## Where it stands

Updated 2026-10-04 (Windows PC, v54 = the v4 release candidate).

- **Releases:** v3 is out; **v4 is the v54 build** (`main` fast-forwarded to
  it; published by the GitHub `build` workflow with release tag `v4`,
  notes in `.github/release-notes.md`). Built with ThinLTO + PGO; the
  profile was retrained on the v53 code (`docs/pgo.md`).
- **On both consoles: v54**, a plain (release-style) build, stage
  `C:\xemu\hw\stage-v54`, map `melee_x.v54.map`. Over v52 it adds the
  grAnime longjmp fix (stage events on ~20 stages: Kraid, Bullet Bills,
  Onett, the Corneria comm window), particles (GX raw FIFO writers), the
  audio-heap/SFX-hash lock (tester crash), the SDL event drain (the
  memory leak), Z24S8 depth at 720p, Stadium screen text, 720p team
  cards, 128 MB mode removed. Console: particles, Stadium, Bullet Bills,
  Team Jigglypuff card, credits and ending, Onett confirmed by the user;
  16-minute burn-ins on both clean (no crash, audio healthy, no leak:
  `C:\xemu\hw\logs54-*`). Open: Rainbow Cruise flicker (roadmap "Other
  open"); trophy view lighting not yet re-checked; red at 720p ends long
  mixed sessions with 2.5-6 MB free (Z24 costs 1.8 MB).
- **Consoles**: red and gold (addresses off the repo; CLAUDE.md,
  `docs/testing.md`). Pull logs before every relaunch or deploy.
- **Reddit tester** (v3; 128 MB + CPU upgrade; settings.ini not
  regenerated, no BACK .bmp, widescreen does nothing; E: has over 1 GB
  free): got v52 as `Melee-X-v52-test.zip` with a README
  (`C:\xemu\hw\reddit-v52`). Waiting for his boot.log: read `[BOOT] save
  folder write test` and `[VIDEO] dashboard`.
- **Open:** `docs/roadmap.md` "Known issues".
- **Merge gate** (user's rule): every subagent branch gets the main
  session's code review plus before/after shots before it goes on `dev`:
  lockstep shots byte for byte where the picture must not change, `[SIMH]`
  equal where the simulation must not (`docs/testing.md` "Comparing builds
  by screenshot"), and the scenes it touches.
- **Git** (user's rule): never commit or push without the go-ahead; `dev`
  is the working branch, `main` is releases only. The repo is public.

## Paused work: LAN phases 0A and 0B

The user is skipping LAN for now (`docs/lan-plan.md`). Each branch has one
WIP commit on the `dev` of 2026-10-04 (on origin too; rebase onto v52
first) and a `HANDOFF-*.md` with what's verified and next; worktrees under
`.claude/worktrees/`, scratch in `C:\xemu` (`run-0a`, `base-0a`, `b`).

| branch | commit | what | state |
|---|---|---|---|
| `lan-0a-probes` | 08c7598 | phase 0A: RNG trace, `MX_JITTER`, `[NETM]`, `simh_diff.py`, round 7 (`C:\xemu\hw\stage-r7`), scenarios `det`, `v1-*` | G0 on gl passes; `det` under `-icount` parts at tick 4447 (item 10 in xemu); left: jitter runs, final G0, the S1 hand-over |
| `lan-0b-net` | 3e91404 | phase 0B: `xhw_net.c` (D14 below every caller), probes, pair/tap tools, host tests `test_net_*` | host tests pass; +248 KB; xemu NAT DHCP works, rtt 0 replies (open decision); left: pair, tap, flood, relaunch, G0 |

Any number of xemu instances may run while the PC has headroom (user,
2026-10-04): `slot=$(/c/xemu/slot.sh take)` waits for CPU < 85% and > 4 GB
free RAM and takes `C:/xemu/xemu.lock.N`; `slot.sh give $slot` after. Run
with `-config_path C:/xemu/slots/$slot/xemu.toml`: the slot's own HDD and
EEPROM copies (two xemu can't open one qcow2; the second exits at once). Use
your own `MX_RUN`, kill only your own PID (runners that `taskkill` every
xemu are for a machine with no other runs).

## Pitfalls on this PC

- Host tests failing for setup reasons only: `test_lower.py`,
  `test_vp_encoder.py` (no `nv2a_vsh`), `vp_policy --check` (temp-file
  error). Python: `C:/msys64/mingw64/bin/python3.exe` (`python3` on PATH is
  the Store stub).
- Game-side code (`src/`, `xbox/src/sdk`) builds with `-Wno-everything`:
  give new sdk code a separate `-Wall` syntax pass.
- The Bash tool eats backslashes in heredocs and Git Bash `sed -i` drops
  CRs: write scripts and edits with a file tool.

## Windows setup

The native MSYS2 build, nxdk and xemu on Windows are in
`docs/toolchain.md`. On the PC used so far: MSYS2 in `C:\msys64`, nxdk in
`C:\xdev\nxdk`, xemu in `C:\xemu\`, per-build stage folders, maps and logs
in `C:\xemu\hw` (`MX_HW=/c/xemu/hw`).

- **Console**: `MX_FTP_HOST=<ip> MX_HW=/c/xemu/hw tools/xbox/console.py
  stage|deploy|pull vNN` (CLAUDE.md "Hardware"). `pull` writes `logsNN`:
  rename it per console (`logs52-red-1`) before pulling the other one.
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
| `settings` | title screen: the settings menu (BACK), a few rows changed, saved, reopened, Save and restart; read `[MENU]`/`[SETTINGS]` (`MX_SHOTS=7` to see the relaunch) |
| `inv` | Fountain of Dreams 4-CPU, players 1-2 cloaked (`MELEE_DEBUG_VS_INVISIBLE=3`: indirect refraction) |
| `res` | results screen with a winner: Fox (port 1) walks off Final Destination in a 15 s match, Mario wins (portrait EFB copies) |
| `tie` | results screen with all four CPUs tied for 1st after an 8 s Final Destination match: the cards' portraits (Debug VS only; `fn_80179854` deviation) |
| `castle` | Peach's Castle 4-CPU, untimed, a Bullet Bill every 30 frames (`MELEE_DEBUG_CASTLE_BILL`): read the `[CASTLE]` lines |
| `clear` | Classic stage 1 won at once (`MELEE_INSTANT_WIN`): the Stage Clear screen's sepia freeze frame |
| `cntalk` | Adventure Corneria's Star Fox cutscene from boot (`MELEE_BOOT_SCENE=cutscene`, `MELEE_BOOT_CUTSCENE=5`): the comm window's lines, shots on Slippy and Falco talking |
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
  a session that was restarted keeps only its last boot's log and
  `boot_prev.log`. Ask the user to pull, or pull yourself,
  before they launch again.
- **Bundle hardware tests**: one console round per batch of fixes; the
  user plays and presses BACK (test builds) for screenshots. Deploy test
  builds whenever the user says the Xbox is on. Ask for BACK shots rather
  than descriptions.
- **xemu under load** (slots above): a starved instance loads slowly and
  trips the watchdog's "frames stopped" (not hangs), and its timing means
  nothing for the console. Call a
  hang only when `[BEAT]`'s retrace count stops for minutes. xemu also
  locks up now and then on its own; rerun before suspecting the build.
  xemu's 16-bit Stadium frames differ by a few pixels run to run: compare
  against a second base run before blaming a change.
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
- **Console icons and saves**: UnleashX caches icons by title ID under
  `E:\UDATA\<id>\TitleImage.xbx`: a wrong icon is a stale cache (old nxdk
  builds used `ffff0002`; Melee-X is `4d580001`). Red's save is the user's
  100% save (a Dolphin `.gci`, off the repo); it logs `[CARD] save data
  looks mixed` (older builds wrote some fields little-endian): reimport the
  Dolphin original if counters look wrong, never the `card-*` backups.
- **Dolphin reference** (unfinished): a Gecko code forcing the attract
  stage, blocked at the memory card prompt; next idea: a real save in
  `GC/USA/Card A`.
