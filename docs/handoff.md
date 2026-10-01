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

## Windows setup (not yet tried on Windows)

The tools were written on macOS; the scripts below were adjusted for Git
Bash but haven't run there yet. Expect small fixes.

- **Git Bash** (comes with Git for Windows) for the `.sh` scripts.
  `xemu_run.sh` and `docker/build.sh` set `MSYS_NO_PATHCONV=1` and pass
  `C:/...` paths to docker and xemu when they detect MSYS.
- **Docker Desktop**, then once: `docker build -t melee-x:sdk tools/xbox/docker`.
  Build: `tools/xbox/docker/build.sh` (the first build takes a while).
- **Python 3** with Pillow (`pip install pillow`) for the dashboard icon,
  `fbdump_to_png.py` and the host tests. The host tests compile C with a
  host compiler: on macOS `test_anim_mtx.py` needed
  `CC=/opt/homebrew/opt/llvm/bin/clang`; on Windows try clang from LLVM or
  run them in WSL. `test_lower.py` needs `/opt/llvm21` (the docker image).
- **xemu for Windows.** Supply your own files; none are committed: MCPX
  boot ROM, BIOS (flash), EEPROM, HDD image (qcow2, 64 MB RAM setting), and
  the GALE01 rev 2 image (`melee102.iso`). A minimal `xemu.toml`:

  ```toml
  [general]
  show_welcome = false
  skip_boot_anim = true
  [general.updates]
  check = false
  [sys.files]
  bootrom_path = 'C:/xemu/mcpx.bin'
  flashrom_path = 'C:/xemu/bios.bin'
  eeprom_path = 'C:/xemu/eeprom.bin'
  hdd_path = 'C:/xemu/hdd.qcow2'
  ```

- A run (Git Bash):

  ```sh
  XBOX_CFLAGS=-DXHW_AUTOPAD=1 tools/xbox/docker/build.sh
  MX_XEMU=/c/xemu/xemu.exe MX_RUN=/c/xemu/run MX_ISO=/c/xemu/melee102.iso \
  MX_STAGE_EXTRA=tools/xbox/scenarios/gl MX_XEMU_ARGS="-config_path C:/xemu/xemu.toml" \
    tools/xbox/xemu_run.sh 330 'FBDUMP\] END'
  python tools/xbox/fbdump_to_png.py /c/xemu/run/serial.log shots
  ```

  Kill a stale xemu first (it holds the HDD's write lock):
  `taskkill //F //IM xemu.exe`. xemu on a Windows PC (x86 host, likely KVM-
  less WHPX or TCG) will run at a different speed than on the Mac's TCG:
  re-baseline the standard run's `[PERF]` before comparing.
- **Console** (any OS): `tools/xbox/console.py` replaces the per-round
  bash scripts: `stage vNN` (copies the build and its map into
  `~/xemu/hw`, override with `MX_HW`), `deploy vNN` (deletes the old
  logs/shots, uploads, re-downloads to verify), `pull vNN` (logs and shots
  to `hw/logsNN`), `ls`. Its FTP path is untested (the console was off when
  it was written); the old curl scripts worked. Next build number: **v33**.

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
- **Console**: FTP `192.168.158.113`, `xbox`/`xbox`, reachable only while
  on. Dashboard UnleashX caches icons by title ID (4d580001). The user's 100%
  save is a Dolphin `.gci` kept on the Mac (`~/Downloads/...35037.gci`);
  never restore the corrupted copies in `~/xemu/hw/card-*`.
- **Dolphin reference** (unfinished): an isolated user dir with a Gecko code
  forcing the attract stage; blocked at the memory card prompt; next idea
  was a real save in `GC/USA/Card A`.
- Mac-only leftovers not copied: per-build maps v1-v31 and logs in
  `~/xemu/hw/` (summaries are in the roadmap), xemu scratch in `~/xemu/mc/`.
