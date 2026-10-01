# Handoff: state and working notes

Written 2026-10-01 at the end of the macOS sessions, for the next round on
a Windows PC. The agent's memory notes don't travel between machines, so
what they held is here. Read `CLAUDE.md`, then this, then
`docs/roadmap.md` "Next".

## Where it stands

- **On the console: v32** = commit `b55012d`, built with
  `XBOX_CFLAGS=-DXHW_PROF=1`. Its map is `tools/xbox/maps/melee_x.v32.map`
  (symbolize any log that build writes with it).
- **v32 tested** (2026-10-01): sound fixed (AC97 start sequence), the
  Data -> title hang fix in, stable, no texture problems, Kirby's helmet,
  capsules and crates right. Mute City 30-40 fps, Onett 40-50, Fountain of
  Dreams ~18-24 fps over 5 s windows with dips to 15 (busy) and 9 (a
  fighter off stage, camera pulled back) against high-level CPUs.
  "Almost a release candidate."
- **Next round: Fountain of Dreams frame rate.** Profile and ideas in
  `docs/roadmap.md` (v32 entry): a cheaper cull recheck (`sample_hash`,
  `dlc_content_changed` ~5%), a texture-bind fast path (~7%), capping sim
  ticks per render on a slow frame, then HSD's setup paths. Also to do: the
  FPS counter's options-menu toggle (now `settings.ini` `[video] fps`).
- Open questions: why bank 2's real fill can outgrow the game's SSM
  accounting (v32 logs the numbers in the overflow line if it happens
  again); the Fountain reflection has no indirect stages in xemu (needs a
  console BACK shot before changing anything).

## Windows setup

Tried and working (2026-10-01). Docker and WSL2 need CPU virtualization,
which can be off in the BIOS, so the build also runs natively:

- **MSYS2** (`C:\msys64`) with `make git bison flex cmake ninja` and the
  `mingw-w64-x86_64-` `clang lld llvm python python-pillow gcc` packages.
  Not mingw's own `cmake`/`ninja`: they can't run nxdk's shell wrappers.
  MSYS2's clang is LLVM 21.1.8, the Docker image's version.
- **nxdk** at the Dockerfile's `NXDK_SHA` in `C:\xdev\nxdk`, built once
  with `make tools` and `make NXDK_ONLY=y NXDK_SDL=y NXDK_CXX=y` (see
  `tools/xbox/msys/build.sh`).
- **Build** from Git Bash: `tools/xbox/msys/build.sh` (passes `XBOX_CFLAGS`
  and friends through; Git Bash's environment doesn't reach MSYS2's bash).
  The code layout matches the Docker build's (v32's map, symbol for
  symbol). `-ffile-prefix-map` keeps the checkout path out of the XBE.
- **xemu**: the Windows release; `tools/xbox/xemu_run.sh` with
  `MX_XISO=<nxdk>/tools/extract-xiso/build/extract-xiso.exe` instead of the
  Docker image's packer. Kill a stale one with `taskkill //F //IM xemu.exe`.
  On an x86 PC xemu runs ~19 fps in the standard match (v32), ~33 (v33 on).
- **Console**: `MX_FTP_HOST=<the Xbox's IP> tools/xbox/console.py stage|deploy|pull vNN`;
  stage also writes `<map>.statics` (static functions, for the profiler).
- **Release**: plain build (no `XBOX_CFLAGS`), then
  `tools/xbox/package_release.py <name>` -> `dist/Melee-X-<name>.zip`;
  `tools/make-xiso` packs a burnable disc (README).

## Scenarios (`tools/xbox/scenarios/`)

Autopad scripts for `MX_STAGE_EXTRA` (need an `-DXHW_AUTOPAD=1` build).
`env` lines set game switches, `<frame> SHOT|BACK|<button>` lines act.

| dir | what |
|---|---|
| `gl` | standard smoke/perf run: Green Greens 4-CPU 60 s, shots mid-match, TIME!, results |
| `fi`, `fod`, `fodperf` | Fountain of Dreams 4-CPU (perf; `fodback*`: BACK dumps on a trace build) |
| `mc10` | Mute City 4-CPU |
| `ps`, `ps2` | Pokémon Stadium 4-CPU, long (transformations; the old GPU-hang soak) |
| `gi` | Stadium, Kirbys vs Falcon with capsules/crates (`MELEE_DEBUG_VS_ITEMS=3`) |
| `gk` | human Kirby with Falcon's hat, Falcon Punch at frame 200 |
| `corn`, `gg`, `gj` | Corneria, Green Greens 20 s, stage 12 |
| `movie` | intro movie shots |

## Working notes (from the agent's memory)

- **Audio check before every console build** (user rule): list everything
  since the last build with good sound that touches audio, the AC97 driver,
  interrupts, timing or memory layout, and tell the user the risk. A healthy
  console boot logs exactly one `[AUDIO] AC97 polled` line; `halted`,
  `stuck` or `cold reset` lines mean trouble. xemu uses an APU voice, which
  hides AC97 problems; `-DXHW_AUDIO_APU=0` forces the AC97 path in xemu.
- **Bundle hardware tests**: one console round per batch of fixes; the
  user plays and presses BACK for screenshots. Deploy only when the user
  says the Xbox is on. Ask for BACK shots rather than descriptions.
- **One emulator at a time.** Several xemu instances starve the CPU and the
  slow loads trip the watchdog's "frames stopped" (not hangs). Call a hang
  only when `[BEAT]`'s retrace count stops for minutes.
- **Never capture the user's desktop**; look at frames only through
  `[FBDUMP]` screenshots (autopad `SHOT`).
- **Perf work**: pick changes that help the console; xemu's GL renderer
  makes every draw expensive on macOS, so draw count dominated there.
  Profiler/FBDUMP output causes visible hitches.
- **Console-only rendering bugs** (GPU ordering, state xemu doesn't model):
  verify on the console with BACK on a `-DXGX_DEBUG_TRACE` build (draws go
  to trace.log; boot.log continues in boot2/boot3.log).
- **Zero-init vs GameCube stack**: game code is compiled with
  `-ftrivial-auto-var-init=zero`; an HSD local read before it's written was
  the previous call's stack on the GameCube and is 0 here (black capsules,
  grey Kirby helmet: `HSD_TExpSetReg`). Check for this before blaming the
  renderer.
- **Autopad buttons** name Duke buttons: Duke A = GC A, Duke X = GC B
  (specials), Duke B = GC X (jump), Duke Y = GC Y. Repro switches:
  `MELEE_DEBUG_VS_CHARS=<ckind>[:<color>][h],...`, `MELEE_DEBUG_VS_ITEMS=<hex>`,
  `MELEE_DEBUG_KIRBY_HAT=<FighterKind>`, `XGX_SKIP=<a>-<b>` (trace builds).
  Shot timing differs between builds.
- **Console**: FTP at the console's IP (`MX_FTP_HOST`), `xbox`/`xbox`, reachable only while
  on. Dashboard UnleashX caches icons by title ID (4d580001). The user's 100%
  save is a Dolphin `.gci` kept off the repo;
  never restore the corrupted copies in `~/xemu/hw/card-*`.
- **Dolphin reference** (unfinished): an isolated user dir with a Gecko code
  forcing the attract stage; blocked at the memory card prompt; next idea
  was a real save in `GC/USA/Card A`.
- Mac-only leftovers not copied: per-build maps v1-v31 and logs in
  `~/xemu/hw/` (summaries are in the roadmap), xemu scratch in `~/xemu/mc/`.
