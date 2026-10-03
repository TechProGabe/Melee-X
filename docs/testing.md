# Testing and debugging

## Host tests (no Xbox needed)

```sh
tools/lower/test_lower.py        # disc_lower vs GCC scalar_storage_order (needs GCC 12+)
tools/xbox/test_vp_encoder.py    # vertex-program encoder vs nv2a-vsh (pip install nv2a-vsh)
tools/xbox/test_vp_opt.py        # optimized vertex programs vs the pre-optimizer generator, random keys [n]
tools/xbox/vp_policy.py --check  # program-memory residency (nv2a_vpmem.c) vs its model and flush/LRU
tools/xbox/test_tex_convert.py   # native texture formats vs the GX decoder
tools/xbox/test_fog.py           # GX fog on the NV2A vs GX's fog factor (libogc registers, Dolphin's formula)
tools/xbox/test_rc.py            # TEV -> combiners: no-swizzle programs unchanged, swap tables vs a combiner model [n]
tools/xbox/test_card_endian.py   # memory-card files: field tables vs the game's structs, big-endian <-> native
tools/xbox/test_pool.py          # nv2a.c's pool allocator: random allocations and frees, block-list invariants
tools/xbox/test_anim_mtx.py      # HSD keyframe interpreter, HSD_MtxSRT, envelope blend vs the code before the rewrites [--full]
tools/xbox/test_audio_mix.py     # src/pc/audio.c's voice mixer (block decoder) vs the per-sample code before it, random voices
```

CI (`.github/workflows/build.yml`, started by hand; it also builds the
releases) runs them all after building `default.xbe`, and uploads the XBE
with its link map.

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

`test_audio_mix.py` does the same for the software AX mixer: it builds
`src/pc/audio.c` (with the Xbox's SDL3 shim and stubs) next to
`tests/xbox/audio_mix_ref.c`, the per-sample `next_sample()` and
`mix_voice()` from before the block decoder, and mixes 200000 random voices
for up to six frames each through both: ADPCM, PCM16, PCM8 and unknown
formats, addresses across the end of ARAM, end and loop addresses on header
nibbles and above the end (HPS), extreme coefficients and histories, ratios
0, 1.0, up to 4.0 and unclamped ones that wrap `frac`, volume ramps through
0 and 32767, and every dry/aux send combination. The dry mix, both aux
busses and the whole `Voice` must have the same bits.

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

The scripts do all of this for you. On macOS they use Docker (colima works)
and xemu at `/Applications/Xemu.app`, with its BIOS, MCPX and HDD already
set up in xemu's settings; on Windows, build with `tools/xbox/msys/build.sh`
and point `MX_XEMU`/`MX_XISO` at xemu and extract-xiso
(`docs/toolchain.md`):

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
`'FBDUMP\] END.?$'` as its stop regex (`.?$`: a base64 line can start with END); `SHOT at frame` is logged before the
dump is written.
`xemu_run.sh` also stops by itself once the scenario's screenshots are all
out (one per `SHOT` line in `$MX_STAGE_EXTRA/autopad.txt`, or `MX_SHOTS=N`),
so a run doesn't idle on the results screen until the timeout; `MX_SHOTS=0`
keeps it running (e.g. for `[PERF]` lines after the last shot).

### Comparing builds by screenshot

A `SHOT` line names a frame, and a frame shows whichever tick the game had
reached by then: a faster build has run fewer ticks per frame, so the same
frame number is a different moment and the shots differ though both builds
draw the same thing. With `env MX_LOCKSTEP=1` in the script the game's
clock (`OSGetTime`) moves 1/60 s at each frame boundary and, while the
frame loop waits for a pad sample, 1 ms at a time instead of sleeping; it
stands still otherwise, and the frame-rate counter is hidden. Every frame is
then exactly one tick (`[PERF]`
"1.0 ticks per render"), frame N shows tick N in any build, and two
builds' shots must match byte for byte (`cmp` on the PNGs). Use it to gate
a change that shouldn't alter the picture; for timing, run without it.

If the log stops dead, heartbeat included, the guest has bugchecked. In the
monitor, `info registers` then shows `HLT=1` with IF clear, and the
bugcheck code is on the stack (`0x7F, 8` is a double fault). A double fault
arrives through a task gate, so the faulting EIP and ESP are in the TSS
that the current TSS's link field names (read the GDT to find it).

## The console loop

What each hardware round looks like (one folder per build in `MX_HW`,
default `~/xemu/hw/`; `tools/xbox/console.py` does it on any OS, with the
console's address in `MX_FTP_HOST`):

1. Build. A test round uses `XBOX_CFLAGS=-DXHW_PROF=1` (profiler, BACK
   screenshots, counter on); a release candidate is a plain build.
2. `console.py stage vNN` copies `default.xbe`, `default.tbn`,
   `TitleImage.xbx` and `TitleMeta.xbx` into `stage-vNN/`, the map to
   `melee_x.vNN.map`, and checks the map against the build's objects
   (`static_syms.py`: any function the map lacks goes to
   `melee_x.vNN.map.statics`; none so far).
3. `console.py deploy vNN` deletes the console's old logs and shots, then
   uploads (the XBE and icon to `/F/Applications/Melee-X/`, the dashboard
   files to `/E/UDATA/4d580001/`) and re-downloads each file to compare.
4. The user plays; BACK takes a screenshot of anything wrong (test builds, or
   a release with the settings menu's "BACK screenshots" on). `boot.log` of the
   boot before is kept as `boot_prev.log` (a restart no longer loses it).
5. `console.py pull vNN` fetches `boot*.log`, `trace.log`, `crash.log`,
   `hang.log` and the `shotNN.bmp` files into `logsNN/`; symbolize with that
   build's map. Pull before the game is launched again: each boot deletes
   the previous boot's logs.

Rendering that differs between xemu and the console has come from state
xemu doesn't model (the w-buffer bit, PFIFO timing): trust the screenshot.

Console-only faults (xemu doesn't raise NV2A limit faults) are hunted with
an autopad test build (`-DXHW_AUTOPAD=1`) and a script uploaded over FTP
as `/F/Applications/Melee-X/autopad.txt` (the game's `D:\autopad.txt`):
it boots straight into the scripted match, and its `env` lines pick the
candidate fix and a stress factor (`MX_COPY_FIX`, `MX_COPY_STRESS`,
`scenarios/stall`), so one deploy covers an A/B and only the script is
swapped between runs. A run without the fix has to fail reliably before
a fixed run counts. Delete `autopad.txt` from the console afterwards (a
release build ignores it).

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

Profile buckets are 64 bytes; three functions in four start inside one and
half are shorter than one. `prof_report.py` shares a bucket out among the
functions it overlaps, by their bytes in it weighted by each function's
sample density over the whole profile. Crediting the whole bucket to the
function at its start (`--first`, the old way) put `pc_atanf`'s samples on
the end of `pc_load_disc_fonts`, `memcpy`'s on `xhw_splash_release` and
pdclib's `memcmp`'s on `longjmp`. The map itself is complete: lld-link
lists the static functions after the public ones, and `static_syms.py`
checks every function of the build's objects against it.

Each sample also records a caller: the first word above the interrupted
stack pointer that points into the XBE right after a call instruction (the
function's return address, or its caller's once it has made a call itself).
`[PROFL]` lines count them for samples inside memcpy/memset/memcmp/memmove
(who copies), `[PROFC]` lines for every sample (the hottest call sites one
level up). `prof_report.py` folds both into functions after the main table.
`[PROFS]` lines (v33 on) count only the samples taken while `[PERF]`'s
current bucket was the simulation: `prof_report.py` lists them last, as the
simulation's own profile (HSD's animation and matrix code runs in both).

The periodic report lists the hottest 192 buckets only (~60% of the
samples). The whole profile of a match is kept too: every bucket of the
code, all samples and the simulation's, in 32-bit counts the periodic
report doesn't reset. It restarts at each scene's entry and is written
once at the match's end (TIME!/GAME!, from `xhw_led_match_end`; the
sampler thread does the writing): as `E:\UDATA\4d580001\prof.bin` in every
profiler build (`console.py pull` fetches it; a profiler build deletes the
previous boot's at startup), and in autopad builds also as `[PROFH]` lines
on COM1 and in the log, so an xemu run has it in `serial.log`. A
`[PROF] whole-match profile N written` line follows. It costs 8 bytes per
64 bytes of `.text` (~550 KB), on top of the periodic report's 4 per 64
bytes of the whole image (~490 KB).

```sh
tools/xbox/prof_report.py --full serial.log --map path/to/melee_x.map   # [PROFH]: every match in the log
tools/xbox/prof_report.py --full prof.bin --map ... --csv match.csv     # the console's file; --last, -n N
```

lists every function with samples, its share and the cumulative share,
for all samples and for the simulation's alone; `--csv` writes both to a
spreadsheet. The formats (counts in hex; buckets of 64 bytes counted from
the image base):

```
[PROFH] begin match 1: base 00010000 shift 6 buckets 111c0, 61000 ms, 60512 samples: 60400 in image, 112 outside, 830 while waiting, 20 unreadable, 17020 in the simulation
[PROFH] all 141 2a,3,+4,1f,...   first bucket, then its count and the next ones'; +n skips n empty buckets
[PROFH] sim 2b7 5,+1c,9,...      (lines stay under 500 bytes, written ~23 KB per log call)
[PROFH] end match 1: all 60400 sim 17020   the sums, which prof_report.py checks
```

`prof.bin` is little-endian: twelve u32 (magic `MXPH`, version 1, image
base, bucket shift, bucket count, match number, milliseconds, samples in
the image, outside it, while waiting, unreadable, in the simulation), then
`u32 all[count]` and `u32 sim[count]`. A second match in the same boot
overwrites it (match number 2); the `[PROFH]` lines keep every match.

### Performance runs in xemu

The standard run is a 60-second 4-CPU timed match on Green Greens with
screenshots mid-match, at TIME! and on the results screen
(`tools/xbox/scenarios/gl/autopad.txt`; the other scenarios are listed in
`docs/handoff.md`):

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

### Instruction counts in xemu (`-icount`)

For comparing builds (`docs/fps-plan.md` step 0.1), run xemu with
`MX_XEMU_ARGS="... -icount shift=1,sleep=off"` (TCG only; on Windows xemu
already runs TCG). Guest time then counts instructions, 2 ns each, free of
softfloat skew and host noise, so a test build's `[PERFX]` buckets are
instruction counts (500 per microsecond; `[CAL]` at boot checks it). Drop
the `SHOT` lines (screenshots stall the game) and stop at the results:

```sh
sed -i '/^[0-9]* *SHOT/d' <staged copy>/autopad.txt
tools/xbox/xemu_run.sh 1500 '\[GAME\] end banner done'
python3 tools/xbox/icount_report.py base.log new.log   # per draw, per tick, [SIMH] compared
```

`icount_report.py` takes the match's `[PERFX]` periods, takes the audio
mixer's time out of the buckets it preempted, and prints instructions per
draw (render, dlist, draw) and per simulation tick, as medians and as
aggregates; with two logs it also compares their `[SIMH]` hashes and exits
non-zero if they differ. Two runs of one build agree within ~0.6%. On the
Windows PC a `gl` match takes ~2.5 minutes this way. `gpu` and `vsync` are
spin loops and mean nothing here.

Census builds (`-DXGX_CENSUS=1`) add `[CENSUS]` and `[DLCC]` blocks every
600 frames: draws, vertices and changed state by pass (main, fighter
shadow maps, Fountain's reflection) and owner (the p_link class of the
GObj that drew), and the display-list cache by owner, lists cached under a
second key and the lists rebuilt most. `tools/xbox/census_report.py` sums
them. The counts are the same on the console.

## Logs

Everything is written to `E:\UDATA\4d580001\` (the title ID is `4d580001`):

| file | contents |
|---|---|
| `boot.log` | the log's first 4 MB; after that it goes on in `boot2.log` and `boot3.log` in turn, each restarted at 2 MB, so the newest 2-4 MB before a late hang survive (all three are deleted at boot). Every line is flushed to disk during the first 600 frames; after that urgent lines (`[SCENE]` `[GAME]` `[MEM]` `[CARD]` `[WDOG]` `[NV2A] GPU`/`flip` `[TEX] drop` `[FATAL]` `[CRASH]` `[BOOT]` `[WARN]`) at once and the rest within a second (the watchdog thread flushes what is pending every second). Tags: `[BOOT]` `[MEM]` `[OS]` `[DVD]` `[NV2A]` `[PAD]` `[AUDIO]` `[CARD]` `[SCENE]` `[GAME]` `[BEAT]` |
| `trace.log` | the `[DRAW]` lines of a `-DXGX_DEBUG_TRACE` build (COM1 still gets them), restarted at 64 MB |
| `hang.log` | written by the watchdog: the log tail and a dump of every thread (also appended to `boot.log`) |
| `crash.log` | written on a CPU exception: the last log lines, the fault, registers, XBE addresses found on the stack |
| `shot00.bmp` .. `shot99.bmp` | screenshots, test builds only (`XHW_TEST_BUILD`): BACK on any controller (unmapped by default in `settings.ini`) writes the next frame as a 24-bit BMP and logs `[SHOT] wrote ...`; numbering restarts at 00 each boot. An autopad `BACK` line does the same in xemu. On the title screen BACK also opens the settings menu (in every build). Y pressed while BACK is held drops every cached texture and display list at the frame end (`[DEBUG] ... caches flushed`): a surface that comes back right afterwards had its cached copy corrupted |
| `settings.ini` | options (`docs/platform.md`) |
| `card_a\*.gci` | memory card saves |

The log also goes to COM1 (the serial port), which xemu can redirect to a terminal or file.

Lines worth reading first:

- `[MEM] boot` and `[MEM] after nv2a`: free RAM, plus how much of MEM1 and
  ARAM has been committed so far. If free RAM runs low while the game is
  still loading, that is the 64 MB budget (`docs/architecture.md`).
- `[DVD] GALE01 rev 2, N FST entries`: the image was accepted.
- `[NV2A] layout:` the physical addresses and sizes of the pushbuffer,
  framebuffers, depth buffer, nxdk's video framebuffer, the vertex ring and
  the texture and vertex pools: a write past one lands in its neighbour.
- `[NV2A] first GPU fault: kind K ...`: the first fault the patched pbkit
  reported (1 PGRAPH: nsource, class, trapped method, data; 2 DMA pusher:
  software put, put, get), with the pusher's GET and PUT at that moment,
  the pushbuffer's base, the frame and the draw count, then the 96
  pushbuffer words around that GET. Later faults are usually its
  consequences. `[NV2A]  first fault pgraph 400800:` follows with PGRAPH
  0x400800-0x40080C read at the fault (pbkit's "limit details" for
  `LIMIT_COLOR`/`LIMIT_ZETA`, nsource 0x10/0x20) and the last EFB copy's
  target (offset, size, frame).
- `[MEM] 128 MB console: running in 64 MB, N KB above it held back`: the
  RAM above 64 MB was allocated at boot and is never used
  (`settings.ini` `ram128 = 0`, the default; `xhw_mem_hold_upper`). `[NV2A] GPU stalled` dumps the words at the current GET only
  when that address is mapped.
- `[PERF]`: see "Measuring on the console". Test builds add `[PERFX]`:
  the same buckets in microseconds, the raw counts, the audio mixer's time
  inside each bucket, and the game thread's x87 control word and MXCSR
  (`027f`: 53-bit precision). `[CAL]` at boot: 20M instructions timed
  (40000 us under `-icount shift=1`).
- `[SIMH] tick N: hash`: test builds, every 60 simulation ticks of a match:
  each fighter's kind, port, action, facing, position, velocities, damage
  and the random seed, hashed. Equal lines = the same simulation.
- `[CPU]`: test builds, at boot: CPUID 1 and 2, CR0/CR3/CR4, MXCSR and its
  mask (DAZ), the x87 control word, and on the console the MTRRs, PAT and
  the page directory's 4 MB entries. `[MEM] lazy <base>: ...` at each scene
  leave: committed 64 KB chunks of each 4 MB range (of 64). `ticks per render` near 5 means
  the game can't keep up and is slowing down (5 is the cap).
- `[NV2A] per N draws: ...`: what changed before each draw (nothing, only a
  position matrix, then per dirty group) and draws by primitive; `[DLC]`
  lines: cached, dynamic and volatile lists, joined batches, immediate-mode
  batches and joins, the calls that drew a waiting batch, and the first
  lists to go volatile with the reason.
- `[NV2A] frame N: D draws (A approximated), tex pool K KB free (largest L
  KB)`: logged every 600 frames. A high `approximated` count means TEV
  setups the combiners only approximate; a tex pool near 0 means a full
  pool (normal in a long session), and `pool allocations failed` counts
  uploads that only fit after evicting: harmless while `[TEX]`'s `drops`
  stays 0.
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
- `[LED] ROGO sweep (retrace R)`, `[LED] SMC`: each front LED write (the
  four steps, `-` for off, and the effect), from its worker; two per KO,
  three in a timed match's last 10 s (at 10, 5 and 2), two per match end,
  none per frame. xemu doesn't show the LED, so these are what a run checks
  (`docs/platform.md` "Front LED").
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
- `[DLC] N of 2048 lists ... vertex pool K of 4096 KB free`: a long
  session fills both and evicts least recently used lists; `builds` per
  interval is the rebuild cost. Only `uncached:`/volatile lists mean lists
  drawn without the cache.
- `[CARD] save data big-endian (votes be N, le M)`, and `looks mixed` when
  an older build rewrote some fields (`docs/platform.md`, CARD).
- `[WARN] hit` / `[WARN] hit by item kind`: the knockback diagnostic
  (`docs/decisions.md`), its first 32 hits a boot; expected in any match
  with items or stage hazards.

A healthy console session: one `[AUDIO] AC97 polled` line and no `halted`,
`stuck` or `cold reset`; `[BEAT]` lines to the end with `presented`
following `retrace`; `[PERF]` `ticks per render` well under 5; no
`crash.log` or `hang.log`.

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
| `XBOX_LTO=1`, `XBOX_PGO=gen\|<file>` | build knobs (not `-D` switches; `docs/toolchain.md`): ThinLTO over the game and sdk code; PGO instrumented (`[PGOC]` counters at each scene's exit) or optimized with a `.profdata` |
| `-DXGX_CENSUS=1` | draw and display-list census: `[CENSUS]`/`[DLCC]` every 600 frames (`nv2a.c`, `gx_vtx.c`, `tools/xbox/census_report.py`) |
| `-DXHW_PMC=1` | the console probe build's counters (`xhw_pmc.c`, with `-DXHW_PROF=1 -DXHW_AUTOPAD=1`): one pair of Pentium III performance-counter events per `[PERF]` period, summed per bucket, as `[PMC]` lines (ten pairs in turn; off in xemu). With `env MX_ABLATE=1` in the autopad script the ablation windows rotate every two periods (`[AB]` lines: FTZ, no shadow maps, no reflection, no back end, no display-list rechecks, no audio). `tools/xbox/probe_report.py`; `scenarios/probe`, `probe2` |
| `-DXHW_PROF=1` | sampling profiler: `[PROF]` lines every 20 s (`xhw_prof.c`, `tools/xbox/prof_report.py`), and each match's whole profile at its end in `prof.bin` (with `-DXHW_AUTOPAD=1` also as `[PROFH]` lines; `prof_report.py --full`) |
| `-DXHW_PROF_SECS=<n>`, `-DXHW_PROF_TOP=<n>` | profiler report period (default 20 s); buckets and call sites per report (default 192) |
| `-DXGX_EFB_GPU_COPY=0` | EFB copies read back on the CPU (into A8R8G8B8 textures) instead of drawn by the GPU, at every bpp; 720p used the CPU readback always until the GPU copy learned R5G6B5 targets |
| `-DXHW_VIDEO_480_BPP=16` | 640x480 at 16 bits: R5G6B5 colour, Z16 depth and 720p's pool sizes, the 720p rendering path at a size xemu can show (xemu has no 720p). Test only: shots and `[FBDUMP]` are 16-bit |
| `-DXGX_Z16_DEPTH_RATIO=<n>` | Z16 depth (720p, `-DXHW_VIDEO_480_BPP=16`) as if the extreme camera's near plane were at far / n (default 4096; `renderer.md` "Depth"); 0: GX's depth as is, the z-fighting crates and grass of v41 |
| `-DOCX_Z16_TILE_FLAGS=<flags>` | pbkit's tile flags for a Z16 depth buffer (720p, or the switch above); default `0x84000001`, pbkit's own: compression tags with the 32-bit flag. Console A/B: `0x80000001` (tags, no 32-bit flag), `0x00000001` (uncompressed); a console-only test |
| `-DXGX_DEPTH_CULL=1` | cull pixels whose depth falls outside the clip range instead of clamping it (the pre-v15 behaviour) |
| `-DXGX_DEBUG_EFBLOG` | log the first 200 EFB copies (source rect, size, format, copy-clear depth) as `[EFB]` lines |
| `-DXHW_TEST_BUILD=1` | console test tools (and: without a disc image in its own folder, the XBE uses `F:\Applications\Melee-X\`'s, so build variants can sit in folders of their own): BACK takes a screenshot (BACK+Y flushes the caches) and the frame-rate counter defaults to on. Implied by `-DXHW_PROF=1` and `-DXHW_AUTOPAD=1`; a plain build is a release and has neither. `-DXHW_AUTOPAD=1 -DXHW_TEST_BUILD=0` is a release with scripted input (release defaults in xemu) |
| `-DXSDK_FPS_DEFAULT=<0/1>` | the frame-rate counter's default when `settings.ini` has no `fps` line (default: `XHW_TEST_BUILD`) |
| `-DXGX_DEBUG_TRACE` | log every draw (TEV stages, textures and their colours, konst, channels, lights, texgen matrices, screen box, blend, fog) of the frame an autopad `SHOT` or a console BACK screenshot captures, as `[DRAW]` lines (~400 KB each, in `trace.log`, not `boot.log`); on a BACK frame each EFB copy's source is also written as a `shotNN.bmp` (up to 8, announced by a `[DRAW] efb copy` line), and with `-DXHW_AUTOPAD=1` also streamed as `[FBDUMP]` (an autopad `BACK` in xemu, whose HDD is out of reach) |
| `-DXGX_CHECK_VERTS` | check every position a display-list build decodes (model space): one at 2^20 or more, infinite or NaN is logged as `[WARN] dlist` (first 32) |
| `-DXGX_PB_KICK=<words>` | pushbuffer words per kick (default 8192; v25 and before 4096) |
| `-DXGX_VB_CACHE_BREAK=0` | no `BREAK_VERTEX_BUFFER_CACHE` at each batch start (v26 added it) |
| `-DXGX_VBUF_FREE_NOW=0` | evicted display-list vertex buffers go through the deferred free like the rest (v26 freed them at once) |
| `-DXGX_COPY_FIX=<bits>` | the EFB copy's surface switches (the post-copy GPU stalls): 1 the retarget sends the pitch again after the format, 4 the copy's target and the retarget's DMA objects, pitch and offsets again after a wait for idle, 2 colour and depth cleared by one `CLEAR_SURFACE` (untried); default 5, 0 is v45's. Test builds also read `env MX_COPY_FIX=` from the autopad script |
| `-DXGX_COPY_STRESS=<n>` | repeat each EFB copy that clears after itself n more times into a scratch texture, with its clear (picture unchanged): makes copy-related GPU faults frequent on the console. Test builds also read `env MX_COPY_STRESS=`; `scenarios/stall` |
| `-DXGX_DEFER=1` | the deferred back end (`docs/renderer.md` "Deferred back end"): draws are queued as records of what changed and replayed in batches; `[NV2A] per 600 frames: N draws queued, M flushes` lines. Test builds also read `env MX_DEFER=1`. `-DXGX_DEFER_KB=<n>`: the queue's size (default 32) |
| `-DXSDK_MEM1_LARGE=<mask>` | MEM1's 4 MB ranges (bit per range, 0x10000000 up) on 4 MB pages (`docs/decisions.md` "Stall candidates"); `[OS] MEM1 on 4 MB pages` at boot. Test builds also read `env MX_MEM1_LARGE=<mask>` (6: the two ranges a 4-CPU match fills) |
| `env MX_NEXT_XBE=<path>` | (autopad script, test builds) 12 s after the match ends, launch that XBE (`F:\Applications\<folder>\default.xbe`, or a `\Device\` path), which reads its own folder's script: a console round chains its builds and runs unattended. Each boot keeps the one before's log as `boot_prev.log`; pull as the chain goes (`docs/fps-plan.md` round 2) |
| `env MX_LOCKSTEP=1` | (autopad script, test builds) the game's clock moves 1/60 s per frame and stands still in between, so every rendered frame is one simulation tick: screenshots by frame number show the same moment in builds of any speed ("Comparing builds by screenshot"). Pacing is off; `[PERF]` keeps the real clock |
| `env MX_PREFETCH=1` | (autopad script, test builds) prefetch plans for the animation walk (`jobj.c`, `docs/decisions.md` "Stall candidates") |
| `-DXGX_OVERLAP=0` | `xgx_present` waits for the GPU before the flip, as up to v32, instead of the next frame's first GPU use (v33) |
| `-DXGX_DEBUG_VPTRACE[=<n>]` | log the vertex-program selects of two consecutive frames every n (default 600) as `[VPT]` lines: each program (key hash, instructions, key bytes), then the selects in order with `L` where one was loaded; replay with `tools/xbox/vp_policy.py boot.log` |
| `-DXGX_DEBUG_NOMIP` | bind only the base level of every texture |
| `-DXGX_NO_INDIRECT=1` | GX indirect stages draw direct (no BUMPENVMAP units, as up to v40): the cloak's refraction shows the frame copy unshifted |
| `-DXHW_FBDUMP_EVERY=<n>` | screenshot every n presented frames |
| `-DXGX_STATS_EVERY=<n>` | `[NV2A]` / `[TEX]` stats period, in frames (default 600) |
| `-DXHW_WATCHDOG=0`, `-DXHW_HEARTBEAT_SECS=<n>` | hang dumper off; `[BEAT]` period (0 = off) |
| `-DXHW_WATCHDOG_PRESENT_SECS=<n>` | seconds of retraces without presents before the watchdog reports (default 10) |
| `-DXGX_TEX_POOL_KB=<n>` | texture pool size (default 8192 at 480, 6144 at 720p) |
| `-DXGX_DEBUG_MAGENTA` | textures the pool could not take draw magenta instead of untextured |
| `-DXHW_NO_SPLASH`, `-DXHW_SPLASH_MS=<n>` | boot title card off; its hold time |
| `-DXGX_DEBUG_PBCHECK` | parse every pushbuffer segment before its kick and log malformed headers as `[PBCHECK]` (first 16): tells a bad command stream from a GPU fault on a good one in xemu, which forgives both |
| `-DXSDK_SETTINGS_RESET` | test builds: delete `settings.ini` at boot (a first boot), then copy `D:\settings.ini` over it if the disc has one (stage it with `MX_STAGE_EXTRA`) |
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
env MELEE_DEBUG_VS_INVISIBLE=3 # players 1 and 2 cloaked all match (Invisible Melee: indirect refraction)
env MELEE_DEBUG_KIRBY_HAT=2    # Kirby spawns with that FighterKind's copy
env XGX_SKIP=569-570           # -DXGX_DEBUG_TRACE: the traced frame leaves those draws out
300 SHOT
```

`MELEE_SEED=<n>` fixes the attract demo's pick, and `MELEE_NO_ATTRACT=1`
turns the attract loop off. `MELEE_BOOT_SCENE=title` starts on the title
screen, where BACK opens the settings menu (`scenarios/settings`). If a load in xemu is slow enough to trip the
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
6. A 4-player VS match on a busy stage keeps full game speed (`[PERF]`
   ticks per render under 5) at 30 fps or more.
7. Saving creates `card_a\01-GALE-*.gci`, and it loads back after a reboot.
