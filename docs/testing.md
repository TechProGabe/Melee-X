# Testing and debugging

## Host tests (no Xbox needed)

```sh
tools/lower/test_lower.py        # disc_lower vs GCC scalar_storage_order (needs GCC 12+)
tools/xbox/test_vp_encoder.py    # vertex-program encoder vs nv2a-vsh (pip install nv2a-vsh)
tools/xbox/test_vp_opt.py        # optimized vertex programs vs the pre-optimizer generator, random keys [n]
tools/xbox/vp_policy.py --check  # program-memory residency (nv2a_vpmem.c) vs its model and flush/LRU
tools/xbox/test_tex_convert.py   # native texture formats vs the GX decoder
tools/xbox/test_fog.py           # GX fog on the NV2A vs GX's fog factor (libogc registers, Dolphin's formula)
tools/xbox/test_card_endian.py   # memory-card files: field tables vs the game's structs, big-endian <-> native
tools/xbox/test_pool.py          # nv2a.c's pool allocator: random allocations and frees, block-list invariants
tools/xbox/test_anim_mtx.py      # HSD keyframe interpreter, HSD_MtxSRT, envelope blend vs the code before the rewrites [--full]
```

CI (`.github/workflows/build.yml`, started by hand only: builds and tests
run locally) runs them all after building `default.xbe`, and uploads the
XBE with its link map.

`tools/xbox/vp_policy.py [boot.log]` replays vertex-program selects (a
`-DXGX_DEBUG_VPTRACE` log, or a synthetic 4-CPU Fountain of Dreams frame
without one) through residency policies and prints loads and instructions
per frame for the programs before and after the optimizer; `--keys` lists a
frame's programs and their sizes, `--diffuse`/`--spec`/`--point` vary the
synthetic stage's lights.

`test_card_endian.py` compiles `xbox/src/sdk/card_endian.c` with the
game's `<melee/gm/types.h>` on the host (LP64; the size asserts of unrelated
disc structs that hold pointers are switched off for it). It checks that
the field tables tile `GmSaveData` and `NameTagDataBank` byte for byte,
that the `FighterData.x7C` bit-field widths match the declaration, that
converting to the card and back is the identity, that a big-endian save
and name-tag bank built by hand from `offsetof` read back with the right
values and are written back byte for byte, and that a little-endian file
(an older Melee-X save) is recognised and left alone. A table that doesn't
add up to `sizeof` its struct also stops the Xbox build.

`test_anim_mtx.py` guards the bit-identical rewrites of HSD's animation and
matrix code (`docs/decisions.md`, "Edits to imported code"). It builds the
current `fobj.c` and `mtx.c` on the host as the Xbox builds them
(`TARGET_XBOX`, SSE, `-ffp-contract=off`) next to `tests/xbox/anim_mtx_ref.c`,
a verbatim copy of the code before the rewrites, and compares:
`pc_sincosf` with `pc_sinf`/`pc_cosf` (16M floats spread over all 2^32 and
every branch boundary; `--full` takes all 2^32, a few minutes), `parseFloat`
for every frac byte and 16-bit pattern, the spline with `1/fterm` in float
for every u16, 20000 random keyframe streams (every opcode and frac type,
pack and wait encodings, truncated streams and garbage) run frame by frame
at random rates with stops and rewinds (every update callback and the FObj
state after each frame), and `HSD_MtxSRT` and the fused envelope blend on
random inputs with denormals, negative zero, infinities and NaNs. Floats
must have the same bits; a NaN only has to stay a NaN (which NaN operand's
payload x86 keeps depends on the operand order the compiler picks). A new
rewrite in this code needs its reference added there first.

## Running it

You need your own Melee NTSC-U 1.02 image (`GALE01`, revision 2) as
`.iso`, `.gcm` or `.ciso`. No game data belongs in this repository: the
`.gitignore` blocks images, DOLs, BIOS files and saves.

**On an Xbox:** copy `default.xbe`, `default.tbn` (the dashboard icon, for
XBMC-style dashboards; the XBE also carries it as `$$XTIMAGE`) and the image
into one folder, for example `E:\Games\Melee-X\`, and launch the XBE from
your dashboard. Dashboards that cache icons by title ID (UnleashX) read
`E:\UDATA\4d580001\TitleImage.xbx` and `TitleMeta.xbx`; the build writes
both next to `default.xbe` to copy there.

**In xemu:** make an XISO of a folder holding `default.xbe` and the image,
using `extract-xiso -c <folder>`, and load it as the DVD. The image is
then found on D:. A retail-compatible BIOS, MCPX ROM and HDD image are
needed, as for any xemu title. xemu is useful for crashes and rendering, but
timing, audio (it uses the APU fallback there) and memory headroom differ
from hardware, so check fixes on a console too.

On macOS the scripts do all of this for you. They use Docker (colima works)
and xemu at `/Applications/Xemu.app`, with its BIOS, MCPX and HDD already
set up in xemu's settings:

```sh
docker build -t melee-x:sdk tools/xbox/docker   # once
tools/xbox/docker/build.sh                      # -> build-xbox/xbe/default.xbe
MX_ISO=~/roms/melee.iso tools/xbox/xemu_run.sh 120 'melee_main'
```

`xemu_run.sh [seconds] [stop-regex]` packs the XISO, boots it, logs COM1 to
`~/xemu/mx-run/serial.log`, and stops after that many seconds or when the
regex matches. See the script header for the `MX_*` variables:

- `MX_STAGE_EXTRA` adds files to the disc, for example `autopad.txt`.
- `MX_GUI=1` leaves xemu running.
- `MX_XEMU_ARGS="-monitor unix:/tmp/mxmon.sock,server,nowait"` adds a QEMU
  monitor. `tools/xbox/xemu_prof.py` then samples where the CPU spends its
  time.
- `XBOX_CFLAGS` (for example `-DXHW_AUTOPAD=1`) passes through to the
  platform build.

Screenshots come out of the serial log as `[FBDUMP]` lines. Decode them
with `tools/xbox/fbdump_to_png.py serial.log shot`. A run that should end on a screenshot needs
`'FBDUMP\] END'` as its stop regex: `SHOT at frame` is logged before the
dump is written.

If the log stops dead, heartbeat included, the guest has bugchecked. In the
monitor, `info registers` then shows `HLT=1` with IF clear, and the
bugcheck code is on the stack (`0x7F, 8` is a double fault). A double fault
arrives through a task gate, so the faulting EIP and ESP are in the TSS
that the current TSS's link field names (read the GDT to find it).

## The console loop

What each hardware round looks like (the dev Mac keeps one folder per
build in `~/xemu/hw/`):

1. Build with `XBOX_CFLAGS=-DXHW_PROF=1`; copy `default.xbe`,
   `default.tbn`, `TitleImage.xbx`, `TitleMeta.xbx` into
   `~/xemu/hw/stage-vNN/` and `melee_x.map` to `~/xemu/hw/melee_x.vNN.map`.
2. `~/xemu/hw/deploy-vNN.sh` uploads over FTP (the XBE and icon to
   `/F/Applications/Melee-X/`, the dashboard files to `/E/UDATA/4d580001/`)
   and re-downloads each file to compare.
3. The user plays; BACK takes a screenshot of anything wrong.
4. `~/xemu/hw/pull-logsNN.sh` fetches `boot.log` (and `boot2.log`, `boot3.log`, `trace.log`), `crash.log`, `hang.log`
   and the `shotNN.bmp` files; symbolize with that build's map.

Rendering that differs between xemu and the console has come from state
xemu doesn't model (the w-buffer bit, PFIFO timing): trust the screenshot.

## Measuring on the console

Every build logs a `[PERF]` line every 5 s (`xhw_perf.c`): fps and the
milliseconds per frame spent in the game's simulation (`sim`), its render
pass (`render`: HSD walking the scene and setting GX state), display-list
decoding, back-end draws, texture conversion, EFB readback, GPU waits and
vsync pacing, plus simulation ticks per render, the audio mixer's share of
the CPU and the draws and vertices per frame.

Melee runs one simulation tick per pad poll queued since the last frame (up
to 5, `gm_801A4D34`), then renders once. A slow frame makes the next one run
more ticks, so `sim` per frame is `ticks per render` times the cost of one
tick, and cutting a tick's cost pays twice: less time per tick, and fewer
ticks per render as the frame rate rises.

Built with `XBOX_CFLAGS=-DXHW_PROF=1`, a sampling profiler (`xhw_prof.c`)
also records where the game thread is, about 1000 times a second, and logs
the hottest code every 20 s as `[PROF]` lines, written as one block (one
disk flush: flushed line by line during a load, the report once starved the
disc reads for ~10 s). Fold them into functions with the link map of the
same build:

```sh
tools/xbox/prof_report.py boot.log --map path/to/melee_x.map
```

Profile buckets are 64 bytes, so a small function right after another can
be credited to its neighbour (pdclib's `memcmp` showed up as `longjmp`).

Each sample also records a caller: the first word above the interrupted
stack pointer that points into the XBE right after a call instruction (the
function's return address, or its caller's once it has made a call itself).
`[PROFL]` lines count them for samples inside memcpy/memset/memcmp/memmove
(who copies), `[PROFC]` lines for every sample (the hottest call sites one
level up). `prof_report.py` folds both into functions after the main table.

### Performance runs in xemu

The standard run is a 60-second 4-CPU timed match on Green Greens with
screenshots mid-match, at TIME! and on the results screen
(`~/xemu/mc/gl/autopad.txt` on the dev Mac):

```
env MELEE_BOOT_SCENE=vs
env MELEE_DEBUG_VS_STAGE=17
env MELEE_DEBUG_VS=cpu4
env MELEE_DEBUG_VS_TIME=60
env MELEE_SEED=1
200 SHOT
500 SHOT
800 SHOT
1200 SHOT
```

Build with `XBOX_CFLAGS=-DXHW_AUTOPAD=1` (add `-DXHW_PROF=1` for a profile),
run `xemu_run.sh 330`, then read the `[PERF]` lines of the match and compare
the screenshots with the last good run's. Frame timing, and so the frame a
`SHOT` lands on, varies between runs, so compare what is drawn, not
positions. Each `[FBDUMP]` stalls the game for seconds while it streams over
COM1, and each profiler report briefly: the hitches in a watched run are
those, and the `[PERF]` intervals that contain one are outliers.

xemu is not a proxy for the console's GPU. Its OpenGL renderer runs every
non-point draw through a geometry shader, and macOS's GL runs a geometry
shader as a compute pass that ends the render pass, so each draw costs ~70 µs
there whatever its size; the frame is then bound by `gpu` (the wait at
present). Its CPU numbers are a rough proxy (TCG runs float code slowly, so
float-heavy functions look hotter than on a Pentium III). To see where xemu
itself spends its time, sample the host process during a match:
`sample $(pgrep -x xemu) 5 -file xemu.txt` and read the `pfifo_thread` tree.
The xemu source is in `~/xemu/xemu-src` (`hw/xbox/nv2a/pgraph/gl`).

## Logs

Everything is written to `E:\UDATA\4d580001\` (the title ID is `4d580001`):

| file | contents |
|---|---|
| `boot.log` | the log's first 4 MB; after that it goes on in `boot2.log` and `boot3.log` in turn, each restarted at 2 MB, so the newest 2-4 MB before a late hang survive (all three are deleted at boot). Every line is flushed to disk during the first 600 frames; after that urgent lines (`[SCENE]` `[GAME]` `[MEM]` `[CARD]` `[WDOG]` `[NV2A] GPU`/`flip` `[TEX] drop` `[FATAL]` `[CRASH]` `[BOOT]` `[WARN]`) at once and the rest within a second (the watchdog thread flushes what is pending every second). Tags: `[BOOT]` `[MEM]` `[OS]` `[DVD]` `[NV2A]` `[PAD]` `[AUDIO]` `[CARD]` `[SCENE]` `[GAME]` `[BEAT]` |
| `trace.log` | the `[DRAW]` lines of a `-DXGX_DEBUG_TRACE` build (COM1 still gets them), restarted at 64 MB |
| `hang.log` | written by the watchdog: the log tail and a dump of every thread (also appended to `boot.log`) |
| `crash.log` | written on a CPU exception: the last log lines, the fault, registers, XBE addresses found on the stack |
| `shot00.bmp` .. `shot99.bmp` | screenshots: BACK on any controller (unmapped by default in `settings.ini`) writes the next frame as a 24-bit BMP and logs `[SHOT] wrote ...`; numbering restarts at 00 each boot. An autopad `BACK` line does the same in xemu. Y pressed while BACK is held drops every cached texture and display list at the frame end (`[DEBUG] ... caches flushed`): a surface that comes back right afterwards had its cached copy corrupted |
| `settings.ini` | options (`docs/platform.md`) |
| `card_a\*.gci` | memory card saves |

The log also goes to COM1 (the serial port), which xemu can redirect to a terminal or file.

Lines worth reading first:

- `[MEM] boot` and `[MEM] after nv2a`: free RAM, plus how much of MEM1 and
  ARAM has been committed so far. If free RAM runs low while the game is
  still loading, that is the 64 MB budget (`docs/architecture.md`).
- `[DVD] GALE01 rev 2, N FST entries`: the image was accepted.
- `[PERF]`: see "Measuring on the console". `ticks per render` near 5 means
  the game can't keep up and is slowing down (5 is the cap).
- `[NV2A] per N draws: ...`: what changed before each draw (nothing, only a
  position matrix, then per dirty group) and draws by primitive; `[DLC]`
  lines: cached, dynamic and volatile lists, joined batches, immediate-mode
  batches and joins, the calls that drew a waiting batch, and the first
  lists to go volatile with the reason.
- `[NV2A] frame N: D draws (A approximated), tex pool K KB free (largest L
  KB)`: logged every 600 frames. A high `approximated` count means TEV
  setups the combiners only approximate; a tex pool near 0 means texture
  churn, and `pool allocations failed` that textures were dropped.
  `pushbuffer peak P of 1024 KB (R restarts)`: the fullest a frame got,
  and how often a frame had to wait for the GPU and restart at the head.
- `[NV2A] per N frames: L vertex programs loaded (I instructions), S program
  switches`: program-memory traffic (`docs/renderer.md` "Program memory").
- `[TEX] ... fmt n/KB/pool KB`: the textures drawn in the last frame by GX
  format (hex): count, KB of GX data, KB they take in the pool. `[TEX] pool
  holds ...`: every cached texture and EFB copy in the pool, and how many
  copies went to a destination that had none.
- `[TEX] ... N drops`: textures that could not be uploaded even after
  evicting (drawn untextured). The first one of each interval has its own
  `[TEX] drop:` line.
- `[SCENE] enter/leave: mode M state S scene K` and the `[MEM] scene` line
  after it: every scene transition with free RAM, committed MEM1+ARAM (and
  how much of ARAM is only on the disc image, `ar.c`), the texture pool and
  the vertex cache. `[GAME] match ends: outcome N` is
  TIME!/GAME!, `[GAME] end banner done` the moment the results take over.
- `[BEAT] Ns: retrace R, presented P, free ...`: every 5 s from the
  watchdog thread. If the log ends with `[BEAT]` lines whose `retrace` still
  climbs while `presented` stands still, the game is looping without
  drawing; if the `[BEAT]` lines stop too, the whole machine stopped.
- `[WDOG] presents stopped, retrace running`: after 10 s of that the
  watchdog dumps every thread to `boot.log` and `hang.log` (the game thread
  marked, with the EIP it was interrupted at); after a minute it shows the
  dump on screen too. `frames stopped` (no retrace for 6 s) shows it at
  once. If the frames (or presents) come back, `[WDOG] frames again after
  N s` is logged and the game gets the screen back: that was a long stall
  (a load), not a hang.

## Crashes

The crash guard catches the exception and shows the report on screen. It
also writes `crash.log`, then parks the thread. Symbolize the log with the
link map from the **same** build:

```sh
tools/xbox/sym.py crash.log                          # map: build-xbox/melee_x.map
tools/xbox/sym.py --map path/to/melee_x.map crash.log
tools/xbox/sym.py 0036c4f4 0014b6d0                  # single addresses
```

A fault at raised IRQL (inside a DPC) can't write files. It shows only the
screen report, so photograph it.

A fault address inside 0x10000000-0x11800000 (MEM1) or
0x12000000-0x13000000 (ARAM) that is reported as a crash, and not quietly
committed, means one of two things: the fault happened at raised IRQL, or
the commit failed because RAM ran out. Compare with `free` on the same
report.

## Build switches

| switch | effect |
|---|---|
| `-DXHW_CRASH_GUARD=0` | no SEH guard. Crashes become bugchecks, and demand-committed memory stops working, so debug only |
| `-DXHW_AUDIO_APU=0` | never use the xemu APU fallback |
| `-DXHW_AUTOPAD=1` | scripted input from `D:\autopad.txt`: `<frame> <buttons/SHOT> [for N]` per line (`xhw_autopad.c`). `env NAME=VALUE` lines feed `getenv`, which reaches melee-pc's test hooks (below) |
| `-DXSDK_ARAM_VERIFY=1` | compares every ARAM copy left on the disc (`ar.c`) with the image; `[AR] verify:` lines |
| `-DXHW_PROF=1` | sampling profiler: `[PROF]` lines every 20 s (`xhw_prof.c`, `tools/xbox/prof_report.py`) |
| `-DXHW_PROF_SECS=<n>`, `-DXHW_PROF_TOP=<n>` | profiler report period (default 20 s); buckets and call sites per report (default 192) |
| `-DXGX_EFB_GPU_COPY=0` | EFB copies read back on the CPU instead of drawn by the GPU |
| `-DXGX_DEPTH_CULL=1` | cull pixels whose depth falls outside the clip range instead of clamping it (the pre-v15 behaviour) |
| `-DXGX_DEBUG_EFBLOG` | log the first 200 EFB copies (source rect, size, format) as `[EFB]` lines |
| `-DXGX_DEBUG_TRACE` | log every draw (TEV stages, textures and their colours, konst, channels, lights, texgen matrices, screen box, blend, fog) of the frame an autopad `SHOT` or a console BACK screenshot captures, as `[DRAW]` lines (~400 KB each, in `trace.log`, not `boot.log`); on a BACK frame each EFB copy's source is also written as a `shotNN.bmp` (up to 8, announced by a `[DRAW] efb copy` line), and with `-DXHW_AUTOPAD=1` also streamed as `[FBDUMP]` (an autopad `BACK` in xemu, whose HDD is out of reach) |
| `-DXGX_CHECK_VERTS` | check every position a display-list build decodes (model space): one at 2^20 or more, infinite or NaN is logged as `[WARN] dlist` (first 32) |
| `-DXGX_PB_KICK=<words>` | pushbuffer words per kick (default 8192; v25 and before 4096) |
| `-DXGX_VB_CACHE_BREAK=0` | no `BREAK_VERTEX_BUFFER_CACHE` at each batch start (v26 added it) |
| `-DXGX_VBUF_FREE_NOW=0` | evicted display-list vertex buffers go through the deferred free like the rest (v26 freed them at once) |
| `-DXGX_DEBUG_VPTRACE[=<n>]` | log the vertex-program selects of two consecutive frames every n (default 600) as `[VPT]` lines: each program (key hash, instructions, key bytes), then the selects in order with `L` where one was loaded; replay with `tools/xbox/vp_policy.py boot.log` |
| `-DXGX_DEBUG_NOMIP` | bind only the base level of every texture |
| `-DXHW_FBDUMP_EVERY=<n>` | screenshot every n presented frames |
| `-DXGX_STATS_EVERY=<n>` | `[NV2A]` / `[TEX]` stats period, in frames (default 600) |
| `-DXHW_WATCHDOG=0`, `-DXHW_HEARTBEAT_SECS=<n>` | hang dumper off; `[BEAT]` period (0 = off) |
| `-DXHW_WATCHDOG_PRESENT_SECS=<n>` | seconds of retraces without presents before the watchdog reports (default 10) |
| `-DXGX_TEX_POOL_KB=<n>` | texture pool size (default 8192 at 480, 6144 at 720p) |
| `-DXGX_DEBUG_MAGENTA` | textures the pool could not take draw magenta instead of untextured |
| `-DXHW_NO_SPLASH`, `-DXHW_SPLASH_MS=<n>` | boot title card off; its hold time |
| `XBOX_FORCE=1 tools/xbox/compile_game.py` | rebuild every game unit |
| `XBOX_KEEP_TEMPS=1` | keep the `.i` / `.lowered.c` intermediates |
| `XBOX_CFLAGS`, `XBOX_CMAKE_ARGS`, `XBOX_NINJA_ARGS` | passed through by `xbox/build.sh`; `XBOX_CFLAGS` sets the platform's C flags |

## Straight into a match

With an autopad build, `env` lines in `autopad.txt` set what `getenv`
returns, so melee-pc's harness hooks work on the Xbox too:

```
env MELEE_BOOT_SCENE=vs        # skip the menus: debug VS (onEnterDebugVs, gmvsmode.c)
env MELEE_DEBUG_VS_STAGE=10    # StKind (src/melee/gr/forward.h): 10 = Mute City
env MELEE_DEBUG_VS=cpu4        # Link, Mario, Fox and DK as four CPUs
env MELEE_DEBUG_VS_TIME=20     # a 20-second timed match: ends on TIME!
env MELEE_DEBUG_VS_CHARS=4:1h,0 # CKind[:costume][h] (yellow Kirby on port 1, Falcon CPU)
env MELEE_DEBUG_VS_ITEMS=3     # items on, hex ItemKind mask (capsules, crates)
env MELEE_DEBUG_KIRBY_HAT=2    # Kirby spawns with that FighterKind's copy
env XGX_SKIP=569-570           # -DXGX_DEBUG_TRACE: the traced frame leaves those draws out
300 SHOT
```

`MELEE_SEED=<n>` fixes the attract demo's pick, and `MELEE_NO_ATTRACT=1`
turns the attract loop off. If a load in xemu is slow enough to trip the
watchdog ("frames stopped"), it logs `frames again` and the run carries on.

## First-boot checklist

1. Boots past the "no disc image" screen, and `boot.log` shows the
   `[DVD]` line.
2. The title screen draws, and `[NV2A]` lines appear.
3. Audio plays on hardware and in xemu.
4. All four controllers map to players 1-4 by port, and hot-plugging
   doesn't shuffle them.
5. At 720p the picture is 16:9 with the HUD at the screen edges. At 480
   (4:3 dashboard) it matches the GameCube framing.
6. A 4-player VS match on a busy stage holds 60 fps.
7. Saving creates `card_a\01-GALE-*.gci`, and it loads back after a reboot.
