# Melee-X

Native original-Xbox port of Super Smash Bros. Melee (NTSC-U 1.02, `GALE01` rev 2)
built with nxdk and LLVM 21. It is not an emulator: the decompiled game (doldecomp via
melee-pc) is compiled for the Pentium III and draws on the NV2A.

Read first: `docs/handoff.md` (current state, Windows setup, working notes),
`docs/README.md` (index), `docs/decisions.md`, `docs/renderer.md`,
`docs/testing.md`. `docs/architecture.md` has the memory budget.

## Layout

| path | what | triple |
|---|---|---|
| `src/melee`, `src/sysdolphin`, `src/pc` | imported game code (melee-pc) | game: `i686-pc-windows-gnu -mno-ms-bitfields`, lowered by `tools/lower/disc_lower` |
| `xbox/src/sdk` (+ `gx/`) | Dolphin SDK on the Xbox; GX front end (`gx_state.c`, `gx_vtx.c`, `gx_tex.c`, `gx_copy.c`) | game |
| `xbox/src/hw` | kernel, pbkit, AC97, USB; NV2A back end (`nv2a.c`, `nv2a_vp.c`, `nv2a_rc.c`); log, watchdog, profiler, perf | nxdk `i386-pc-win32` |
| `xbox/include/xgx.h`, `xhw.h` | the only structs crossing the two triples: 32-bit scalars, floats, byte arrays; no bit-fields, no 64-bit members | both |
| `tools/xbox` | build, xemu runner, profiler/crash symbolizers, host tests | host |

## Build and test

```sh
tools/xbox/docker/build.sh                        # -> build-xbox/xbe/default.xbe + build-xbox/melee_x.map
XBOX_CFLAGS="-DXHW_PROF=1" tools/xbox/docker/build.sh   # extra platform flags (switch table: docs/testing.md)
python3 tools/xbox/test_tex_convert.py            # host tests (tests/xbox/*.c)
python3 tools/xbox/test_vp_encoder.py
python3 tools/xbox/test_fog.py                    # GX fog math (nv2a_fog.c) vs GX's fog factor
python3 tools/xbox/test_card_endian.py            # card files: field tables vs GmSaveData, BE <-> native
python3 tools/xbox/test_pool.py                   # nv2a.c texture/vertex pool allocator, random alloc/free
python3 tools/xbox/test_anim_mtx.py               # fobj.c/mtx.c rewrites vs tests/xbox/anim_mtx_ref.c, bit for bit
python3 tools/lower/test_lower.py
```

`tools/xbox/test_tex_convert.py` compiles `gx_tex.c` against the stubs in
`tests/xbox/test_tex_convert.c`: a new external call from `gx_tex.c` needs a stub there.

xemu (one instance at a time; `pkill -9 -f Xemu.app/Contents/MacOS/xemu` first):

```sh
MX_RUN=~/xemu/mc/run MX_ISO=~/xemu/roms/melee102.iso MX_STAGE_EXTRA=tools/xbox/scenarios/gg \
MX_XEMU_ARGS="-config_path $HOME/xemu/mc/xemu.toml" \
  tools/xbox/xemu_run.sh <secs> '<stop regex>'       # serial log: $MX_RUN/serial.log
tools/xbox/fbdump_to_png.py $MX_RUN/serial.log out   # [FBDUMP] screenshots (-DXHW_AUTOPAD=1 + SHOT lines)
```

On Windows (Git Bash) also set `MX_XEMU=/c/.../xemu.exe`; kill xemu with `taskkill //F //IM xemu.exe`
(see `docs/handoff.md`).

Standard smoke/perf run: `XBOX_CFLAGS=-DXHW_AUTOPAD=1` build, `MX_STAGE_EXTRA=tools/xbox/scenarios/gl`
(60 s 4-CPU match, shots mid-match, at TIME! and on the results), `xemu_run.sh 330`;
read `[PERF]`/`[NV2A]`/`[DLC]` and compare the shots with the last good run
(docs/testing.md "Performance runs in xemu"). The performance plan is in docs/roadmap.md.

Hardware: FTP `192.168.158.113` (`xbox`/`xbox`), deploy to `/F/Applications/Melee-X/`
(`default.xbe` and `default.tbn`, the dashboard icon, next to the disc image; `TitleImage.xbx` and `TitleMeta.xbx` to `/E/UDATA/4d580001/` for UnleashX's icon cache), logs in `/E/UDATA/4d580001/` (`boot.log`, `shotNN.bmp`,
`crash.log`, `hang.log`): `tools/xbox/console.py stage|deploy|pull vNN`. Keep each deployed build's `melee_x.map` in `~/xemu/hw/` so
`tools/xbox/sym.py` and `tools/xbox/prof_report.py` can symbolize its logs.

## Rules

- Never capture the user's desktop; look at frames through `[FBDUMP]` screenshots.
- Never commit game data (images, DOLs, BIOS, saves). Commit only when asked.
- Every edit to imported game code (`src/melee`, `src/sysdolphin`, `src/pc`) gets a
  `PORT:` comment and a line in `docs/decisions.md` ("Edits to imported code").
- New build switches or behavior changes: update `docs/renderer.md`, `docs/testing.md`
  (switch table) and `docs/decisions.md`.
- xemu is slow (TCG on Apple Silicon) and differs from hardware in timing, audio and
  memory; prefer reasoning from code plus one smoke run over testing each change there.
- Code style: C, match the surrounding comment density and idiom; `xgx.h`/`xhw.h`
  structs must lay out identically on both triples.
