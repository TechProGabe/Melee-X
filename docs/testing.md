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
tools/xbox/test_anim_mtx.py      # HSD keyframe interpreter, HSD_MtxSRT, envelope blend vs the code before the rewrites [--full]
tools/xbox/test_pobj_mtx.py      # PObjSetupMtx (envelope memo, prefetches), SSE HSD_MtxInverseTranspose vs upstream
tools/xbox/test_tex_cache.py     # gx_tex.c's texture cache and binds vs the file before the entry split: the same trace
tools/xbox/test_dl_cull.py       # gx_dl_culled's box test with clip planes kept per projection vs planes per box
tools/xbox/test_audio_mix.py     # src/pc/audio.c's voice mixer (block decoder, SSE1), output clamp and reverb vs the code before
tools/xbox/test_mplib.py         # stage collision's line rejects (mplib.c) vs the line tests before them, random stages [rounds]
tools/xbox/test_pool.py          # nv2a.c's pool allocator: random allocations and frees, block-list invariants (not in CI)
```

CI (`.github/workflows/build.yml`, started by hand; it also builds the
releases) runs all but `test_pool.py` after building `default.xbe`, and
uploads the XBE with its link map.

The reference tests build the current code on the host the way the Xbox
builds it (`TARGET_XBOX`, SSE, `-ffp-contract=off`) next to a verbatim copy
of the code before a rewrite in `tests/xbox/*_ref.c`, run both on random and
adversarial inputs (denormals, signed zeros, infinities, NaNs), and require
the same bits; a NaN only has to stay a NaN. A new rewrite of guarded code
needs its reference added first.

| test | reference | what is compared |
|---|---|---|
| `test_anim_mtx.py` | `anim_mtx_ref.c` (`fobj.c`, `mtx.c` before the rewrites, `docs/decisions.md` "Edits to imported code") | `pc_sincosf` vs `pc_sinf`/`pc_cosf` (16M floats, `--full` all 2^32), `parseFloat`, the spline, 20000 random keyframe streams frame by frame, `HSD_MtxSRT` and the fused envelope blend |
| `test_pobj_mtx.py` | `pobj_mtx_ref.c` (upstream setup, scalar `HSD_MtxInverseTranspose`) | the inverse transpose on 4M matrices, then 300000 random DObjs: every GX matrix load, `GXSetCurrentMtx`, blend count and joint update in order |
| `test_tex_cache.py` | `gx_tex_ref.c` (before the hot-line split and the bind-path cuts) | 4000 seeded frames of HSD-shaped binds, all eleven formats, EFB copies, collisions, pool overflow: the full back-end trace must match |
| `test_dl_cull.py` | `dl_cull_ref.c` (planes made per box) | 12M boxes against GX-shaped and random projections changed every few boxes: every answer and the planes' bits |
| `test_audio_mix.py` | `audio_mix_ref.c` (per-sample `next_sample()`/`mix_voice()`) | 200000 random voices (all formats, ARAM wrap, loops, ratios, ramps), the output clamp and the aux reverb; built with SSE1 and as plain C (`PC_AUDIO_SCALAR`) |
| `test_mplib.py` | `mplib_ref.c` (line loops before the rejects; cut out of `mplib.c` by name) | the eight line loops on a Fountain-like stage, random polylines and adversarial lines: return, outputs, flags, callbacks; prints the share of line tests skipped |
| `test_card_endian.py` | `offsetof`-built saves | field tables tile `GmSaveData` and `NameTagDataBank`, round trip is the identity, little-endian (old Melee-X) files left alone; a table that doesn't add up to `sizeof` also stops the Xbox build |

`test_audio_mix.py` and `test_mplib.py` take `CC`/`CFLAGS` to check the
Xbox's code generation on Windows (MSYS2), e.g. `CC="clang
--target=i686-w64-mingw32 --sysroot=C:/msys64/mingw32" CFLAGS="-march=pentium3
-msse -mfpmath=sse"`, or `CC=i686-w64-mingw32-gcc` (x87 doubles, as on the Xbox).

`tools/xbox/vp_policy.py [boot.log]` replays vertex-program selects (a
`-DXGX_DEBUG_VPTRACE` log, or a synthetic 4-CPU Fountain frame) through
residency policies; `--keys`, `--diffuse`/`--spec`/`--point` vary it.

## Running it

You need your own Melee NTSC-U 1.02 image (`GALE01`, revision 2) as `.iso`,
`.gcm` or `.ciso`; no game data belongs in this repository. On an Xbox it
sits next to `default.xbe` and `default.tbn` (the dashboard icon);
`TitleImage.xbx`/`TitleMeta.xbx` go to `E:\UDATA\4d580001\` (UnleashX's icon
cache). In xemu it is packed into an XISO with the XBE and found on D:.
xemu needs your own BIOS, MCPX ROM and HDD image (64 MB); it is good for
crashes and rendering, but timing, audio (the APU fallback) and memory
headroom differ from hardware: check fixes on a console.

```sh
docker build -t melee-x:sdk tools/xbox/docker   # once (macOS; Windows: tools/xbox/msys/build.sh, docs/toolchain.md)
tools/xbox/docker/build.sh                      # -> build-xbox/xbe/default.xbe
MX_ISO=~/roms/melee.iso tools/xbox/xemu_run.sh 120 'melee_main'
```

`tools/xbox/xemu_run.sh [seconds] [stop-regex]` packs the XISO, boots it,
logs COM1 to `$MX_RUN/serial.log`, and stops after that many seconds, when
the regex matches, or once the scenario's screenshots are out. Variables
(the script header has them too):

| variable | meaning |
|---|---|
| `MX_ISO` | your image, packed next to `default.xbe`; `none` omits it (the missing-disc screen) |
| `MX_XBE` | the XBE (default `build-xbox/xbe/default.xbe`) |
| `MX_RUN` | work dir for the XISO and logs (default `~/xemu/mx-run`) |
| `MX_STAGE_EXTRA` | a folder whose contents are also packed, e.g. `tools/xbox/scenarios/gl` (its `autopad.txt`) |
| `MX_SHOTS` | stop once this many `[FBDUMP]` screenshots are complete; default the number of `SHOT` lines in `$MX_STAGE_EXTRA/autopad.txt`, `0` runs to the regex or timeout (e.g. for `[PERF]` after the last shot) |
| `MX_GUI=1` | leave xemu running at the end |
| `MX_XEMU_ARGS` | extra xemu arguments: `-config_path <xemu.toml>`, a QEMU monitor (`-monitor unix:/tmp/mxmon.sock,server,nowait`, for `tools/xbox/xemu_prof.py` sampling), `-icount` |
| `MX_XEMU`, `MX_XISO` | xemu binary (default macOS's `/Applications/Xemu.app`) and a native extract-xiso (Windows without Docker) |

One xemu at a time (Windows: `taskkill //F //IM xemu.exe`). Screenshots come
out of the serial log as `[FBDUMP]` lines: `tools/xbox/fbdump_to_png.py
serial.log shot`. To stop on one by regex use `'FBDUMP\] END.?$'` (a base64
line can start with END).

If the log stops dead, heartbeat included, the guest has bugchecked: the
monitor's `info registers` shows `HLT=1` with IF clear and the bugcheck code
on the stack (`0x7F, 8` is a double fault; its EIP and ESP are in the TSS
the current TSS's link field names).

### Comparing builds by screenshot

A `SHOT` frame shows whichever tick the game had reached, so a faster build
shows a different moment at the same frame number. With `env MX_LOCKSTEP=1`
in the script the game's clock (`OSGetTime`) moves 1/60 s per frame
boundary (and 1 ms at a time while the frame loop waits for a pad sample),
so every frame is one tick (`[PERF]` "1.0 ticks per render"), frame N shows
tick N in any build, and two builds' shots must match byte for byte (`cmp`
the PNGs). The frame-rate counter is hidden. Use it to gate a change that
shouldn't alter the picture; time without it. On the console use `N TSHOT`
(tick-based) since loading times vary.

### Performance runs in xemu

The standard run is `tools/xbox/scenarios/gl` (other scenarios:
`docs/handoff.md`): `env` lines for a 60-second 4-CPU timed match on Green
Greens (`MELEE_BOOT_SCENE=vs`, `MELEE_DEBUG_VS_STAGE=17`,
`MELEE_DEBUG_VS=cpu4`, `MELEE_DEBUG_VS_TIME=60`, `MELEE_SEED=1`) and `SHOT`s
at frames 200, 500, 800 and 1200 (mid-match, TIME!, results). Build with
`XBOX_CFLAGS=-DXHW_AUTOPAD=1` (add `-DXHW_PROF=1` for a profile), run
`xemu_run.sh 330`, read the match's `[PERF]` lines and compare what the
shots draw with the last good run. Each `[FBDUMP]` stalls the game for
seconds (each profiler report briefly): `[PERF]` intervals containing one
are outliers.

xemu is not a proxy for the console's GPU: its GL renderer runs every
non-point draw through a geometry shader (on macOS a compute pass, ~70 µs per
draw), so frames are bound by `gpu`. CPU numbers are a rough proxy (TCG makes
float code look hot). To profile xemu itself on macOS: `sample $(pgrep -x
xemu) 5 -file xemu.txt`, the `pfifo_thread` tree.

### Instruction counts in xemu (`-icount`)

For comparing builds (`docs/fps-plan.md` step 0.1) add
`-icount shift=1,sleep=off` to `MX_XEMU_ARGS` (TCG only). Guest time then
counts instructions (2 ns each), so `[PERFX]` buckets are instruction
counts (500 per µs; `[CAL]` at boot checks it). Drop the `SHOT` lines and
stop at the results:

```sh
sed -i '/^[0-9]* *SHOT/d' <staged copy>/autopad.txt
tools/xbox/xemu_run.sh 1500 '\[GAME\] end banner done'
python3 tools/xbox/icount_report.py base.log new.log   # per draw, per tick, [SIMH] compared
```

`icount_report.py` takes the audio mixer's time out of the buckets it
preempted, prints instructions per draw and per tick (medians and
aggregates), and with two logs exits non-zero if their `[SIMH]` hashes
differ. Two runs of one build agree within ~0.6%. `gpu` and `vsync` are spin
loops and mean nothing here. Census counts (`-DXGX_CENSUS=1`) are the same
in xemu and on the console.

## The console loop

Two test consoles on the LAN, FTP login `xbox`/`xbox`, address in
`MX_FTP_HOST`:

| console | drives | game folder |
|---|---|---|
| red | F: | `F:\Applications\Melee-X` (`console.py` and `console_round.py` work as is) |
| gold | softmod, no F: | `E:\Applications\Melee-X`: `console.py deploy` targets F:, so upload `default.xbe`/`default.tbn` by FTP to `/E/Applications/Melee-X/` (`pull` works) |

The FTP server is UnleashX's: `LIST` ignores a path argument (CWD first),
and the 220 banner shows each drive's free space. Test builds without an
image in their own folder take `F:\Applications\Melee-X\`'s, so console
rounds are red only.

`tools/xbox/console.py` (any OS; one folder per build in `MX_HW`, default
`~/xemu/hw/`):

1. Build: `XBOX_CFLAGS=-DXHW_PROF=1` for a test round, plain for a release.
2. `stage vNN`: the four files into `stage-vNN/`, the map to
   `melee_x.vNN.map`, checked against the build's objects (`static_syms.py`).
3. `deploy vNN`: deletes the console's old logs and shots, uploads (XBE and
   icon to `/F/Applications/Melee-X/`, `.xbx` files to `/E/UDATA/4d580001/`)
   and re-downloads each file to compare.
4. Play; BACK screenshots anything wrong (test builds, or the settings
   menu's "BACK screenshots").
5. `pull vNN`: `boot*.log`, `boot_prev.log`, `trace.log`, `crash.log`,
   `hang.log`, `shotNN.bmp`, `prof.bin` into `logsNN/` (`ls` lists them).
   Pull before launching again: each boot deletes the previous boot's logs
   but `boot.log`, kept as `boot_prev.log`.

Where xemu and the console render differently, trust the console (xemu
doesn't model the w-buffer, PFIFO timing or tile regions).

Console-only faults (xemu raises no NV2A limit faults): an autopad test build
plus `autopad.txt` uploaded next to it (the game's `D:\autopad.txt`) whose
`env` lines pick the fix and stress (`MX_COPY_FIX`, `MX_COPY_STRESS`,
`scenarios/stall`), so one deploy covers an A/B. A run without the fix must
fail reliably before a fixed run counts. Delete `autopad.txt` afterwards.

### Console rounds (many A/B runs, one launch)

`tools/xbox/console_round.py N stage|upload|watch|report|shots|clean`
(`docs/fps-plan.md`): each run of round N is an autopad build in its own
folder (`F:\Applications\Melee-X-rN?`, image from `Melee-X`) with a script
from `CHAINS[N]`. A run ends by launching the next folder (`env
MX_NEXT_XBE`, 12 s after the match or at `NEXT`), keeping its log as
`boot_<folder>.log` and shots as `shot_<folder>_NN.bmp`; the last run (the
baseline in `Melee-X`) returns to the dashboard. `watch` waits for the
dashboard's FTP and pulls into `$MX_HW/logs-rN`, `report` tabulates fps,
buckets and GPU waits per run, `shots` groups byte-identical shots (with
`MX_LOCKSTEP=1` and `TSHOT`), `clean` removes the folders. The first run of
a chain is ~4% slow: make it a warm-up.

## Measuring on the console

Every build logs `[PERF]` every 5 s (`xhw_perf.c`): fps and ms per frame in
the simulation (`sim`), the render pass (`render`: HSD walking the scene,
GX state), display-list decoding, back-end draws, texture conversion, EFB
readback, GPU waits and vsync pacing, plus ticks per render, the mixer's CPU
share, draws and vertices per frame. Melee runs one tick per pad poll queued
since the last frame (up to 5), then renders once, so `sim` = ticks per
render times one tick's cost: cutting a tick's cost pays twice.

`-DXHW_PROF=1` adds a sampling profiler (`xhw_prof.c`, ~1000 Hz on the game
thread) whose hottest 192 buckets are logged every 20 s as `[PROF]` lines.
Fold them with the same build's map:

```sh
tools/xbox/prof_report.py boot.log --map path/to/melee_x.map
tools/xbox/prof_report.py --full serial.log --map ...                 # [PROFH]: every match in the log
tools/xbox/prof_report.py --full prof.bin --map ... --csv match.csv   # the console's file; --last, -n N
```

Buckets are 64 bytes; `prof_report.py` shares a bucket among the functions
it overlaps, weighted by each one's sample density (`--first`: all to the
first). `[PROFL]` counts callers of samples in memcpy/memset/memcmp/memmove,
`[PROFC]` the hottest call sites one level up, `[PROFS]` only samples taken
in the simulation bucket (listed last). The whole-match profile (every
bucket, restarted at each scene entry) is written at TIME!/GAME! to
`E:\UDATA\4d580001\prof.bin`, and with autopad also as `[PROFH]` lines, then
`[PROF] whole-match profile N written`. `prof.bin` is little-endian: twelve
u32 (`MXPH`, version 1, image base, bucket shift, bucket count, match
number, ms, samples in the image, outside, while waiting, unreadable, in the
simulation), `u32 all[count]`, `u32 sim[count]`; a later match overwrites it.

## Logs

Everything goes to `E:\UDATA\4d580001\` (title ID `4d580001`) and to COM1;
if E: can't be written, `[BOOT] can't write` and the log is `D:\boot.log`.

| file | contents |
|---|---|
| `boot.log` | the first 4 MB; then `boot2.log` and `boot3.log` in turn, each restarted at 2 MB, so the newest 2-4 MB survive. Every line is flushed during the first 600 frames; after that urgent tags (`[SCENE]` `[GAME]` `[MEM]` `[CARD]` `[WDOG]` `[NV2A] GPU`/`flip` `[TEX] drop` `[FATAL]` `[CRASH]` `[BOOT]` `[WARN]`) at once, the rest within a second |
| `boot_prev.log` | the previous boot's `boot.log` |
| `trace.log` | `[DRAW]` lines of a `-DXGX_DEBUG_TRACE` build, restarted at 64 MB |
| `hang.log` | the watchdog's dump: log tail and every thread (also appended to `boot.log`) |
| `crash.log` | a CPU exception: last log lines, fault, registers, XBE addresses on the stack |
| `lastexit.txt` | how the boot ended (dashboard, resets, restart, fatal screen, relaunch, next XBE, crash, hang); logged at the next boot as `[BOOT] previous exit: ...` and deleted. `not recorded`: switched off, reset or killed |
| `shot00.bmp`.. | BACK screenshots, test builds (24-bit BMP, `[SHOT] wrote ...`, numbering restarts each boot). BACK on the title screen opens the settings menu in every build; Y while BACK is held flushes every cached texture and display list (`[DEBUG] ... caches flushed`) |
| `prof.bin` | profiler builds: the last match's whole profile |
| `settings.ini` | options (`docs/platform.md`) |
| `card_a\*.gci` | memory card saves |

Tags (switch-only tags such as `[EFB]`, `[DRAW]`, `[VPT]`, `[PBCHECK]`,
`[CENSUS]`, `[PMC]`, `[PGOC]` are in the switch table):

- `[MEM] boot`, `[MEM] after nv2a`: free RAM, committed MEM1 and ARAM (low
  while loading: the 64 MB budget, `docs/architecture.md`). `[MEM] 128 MB
  console: running in 64 MB` (`settings.ini` `ram128 = 0`). Test builds:
  `[MEM] lazy <base>:` at scene leave, committed 64 KB chunks per 4 MB.
- `[DVD] GALE01 rev 2, N FST entries`: the image was accepted.
- `[SCENE] enter/leave: mode M state S scene K` and its `[MEM] scene` line:
  free RAM, MEM1+ARAM (and ARAM left on the disc), texture pool, vertex
  cache. `[GAME] match ends: outcome N` is TIME!/GAME!, `[GAME] end banner
  done` the results taking over.
- `[PERF]`: above. Test builds add `[PERFX]` (buckets in µs, raw counts, the
  mixer's time per bucket, x87 control word and MXCSR; `027f` = 53-bit) and
  `[CAL]` (20M instructions timed; 40000 µs under `-icount shift=1`).
- `[SIMH] tick N: hash`: test builds, every 60 ticks of a match: fighters'
  state and the random seed hashed. Equal lines = the same simulation.
- `[CPU]`: test builds at boot: CPUID, CR0/CR3/CR4, MXCSR, x87 control word;
  on the console MTRRs, PAT and 4 MB page entries.
- `[NV2A] layout:` pushbuffer, framebuffers, depth buffer, vertex ring and
  pools (a write past one lands in its neighbour); `[NV2A] tiles`.
- `[NV2A] first GPU fault: kind K ...` (1 PGRAPH, 2 DMA pusher) with
  GET/PUT, frame, draw count and 96 pushbuffer words around GET; later
  faults are usually consequences. `first fault pgraph 400800:` adds the
  `LIMIT_COLOR`/`LIMIT_ZETA` details and the last EFB copy's target.
- `[NV2A] frame N: D draws (A approximated), tex pool ...` every 600 frames:
  `approximated` = TEV setups the combiners only approximate; `pool
  allocations failed` is harmless while `[TEX]` drops stay 0; `pushbuffer
  peak P of 1024 KB (R restarts)`. `[NV2A] per N draws:` state changed per
  draw; `per N frames: L vertex programs loaded` (`renderer.md`).
- `[DLC]`: cached, dynamic and volatile display lists, batches, `N of 2048
  lists ... vertex pool K of 4096 KB free` (full in a long session, LRU;
  only `uncached:`/volatile lists bypass the cache). `immediate: ... N
  short`: immediate batches that ended with fewer vertices than `GXBegin`
  announced (writes that didn't fit the vertex descriptor, dropped);
  should be 0 (it was ~8000 per 600 frames in a match while particles
  were dropped, issue #7).
- `[TEX]`: last frame's textures by format, `pool holds ...`, `N drops`
  (couldn't upload even after evicting: drawn untextured; first one gets a
  `[TEX] drop:` line), `copy dropped` (an EFB copy with no room).
- `[LED] ROGO sweep`, `[LED] SMC`: front-LED writes (two per KO, three in a
  timed match's last 10 s, two per match end); xemu has no LED, so runs
  check these (`docs/platform.md` "Front LED").
- `[BEAT] Ns: retrace R, presented P, free ...` every 5 s: `retrace`
  climbing with `presented` still = the game loops without drawing; no
  `[BEAT]` = the machine stopped.
- `[WDOG] presents stopped, retrace running`: after 10 s every thread is
  dumped (game thread's EIP marked) to `boot.log` and `hang.log`, after a
  minute on screen too; `frames stopped` (no retrace for 6 s) at once.
  `[WDOG] frames again after N s`: a long stall (a load), not a hang.
- `[CARD] save data big-endian (votes be N, le M)`, `looks mixed`.
- `[WARN] hit`: the knockback diagnostic (`docs/decisions.md`), first 32 per
  boot, expected with items or hazards. `[WARN] dlist`: `-DXGX_CHECK_VERTS`.
- `[AUDIO]`, `[BOOT] previous exit:`: "Audio at boot".

A healthy console session: one `[AUDIO] AC97 polled` line and no `halted`,
`stuck` or `cold reset`; `[BEAT]` lines to the end with `presented`
following `retrace`; `ticks per render` well under 5; no `crash.log` or
`hang.log`.

### Audio at boot

Every boot logs the audio hardware as the previous XBE left it, then what
the driver did to bring it to idle (`xhw_audio.c`):

- `[AUDIO] found: pci aci .. pcm bd .. civ a->b ... cr .., spdif ...`: the
  AC97 controller and, per bus master (PCM, S/PDIF), descriptor list, CIV,
  LVI, SR, PICB, CR; CIV and PICB read 5 ms apart move if an engine runs
  (`cr 01` with `sr` bit 0 clear: one left running).
- `[AUDIO] found: apu ... xgscnt a->b gprst .. eprst .. gp fifo0 a->b ...`:
  the MCPX APU (names from xemu's `apu_regs.h`): moving `xgscnt` = setup
  engine running, `gprst`/`eprst` 3 = DSPs out of reset, moving FIFOs = DSPs
  writing output (DirectSound).
- `[AUDIO] found: codec 26 .. vendor ..`: AC97 codec registers, or `codec
  not ready`.
- `[AUDIO] idle: ... halt pcm ok N us spdif ok N us, apu stopped, ...`: bus
  masters halted (`TIMEOUT` after 20 ms), APU stopped (`left (xemu voice)`
  in xemu), codec powered up if needed.
- `[BOOT] previous exit: ...`: from `lastexit.txt`.

An engine that never finishes a buffer from the boot on (reset, three
restarts, a cold reset, three more) is given up: `[AUDIO] stuck since boot:
...` once, the game runs on silent, and "Sound hardware is stuck. Turn the
Xbox off and on to get sound back." shows for 10 s. `-DXHW_AUDIO_TEST`
exercises this in xemu (`scenarios/relaunch`).

## Crashes

The crash guard shows the report on screen, writes `crash.log` and parks the
thread. Symbolize with the link map of the **same** build:

```sh
tools/xbox/sym.py crash.log                          # map: build-xbox/melee_x.map
tools/xbox/sym.py --map path/to/melee_x.map crash.log
tools/xbox/sym.py 0036c4f4 0014b6d0                  # single addresses
```

A fault at raised IRQL (in a DPC) can't write files: photograph the screen.
A reported fault inside 0x10000000-0x11800000 (MEM1) or
0x12000000-0x13000000 (ARAM), which is normally committed on demand, means
raised IRQL or RAM ran out: compare `free` on the same report.

## Build switches

`-D` switches go in `XBOX_CFLAGS`; `env` and script lines go in an autopad
script (test builds). Test builds are `-DXHW_TEST_BUILD=1`, implied by
`-DXHW_PROF=1` and `-DXHW_AUTOPAD=1`.

| switch | effect |
|---|---|
| `-DXHW_TEST_BUILD=1` | console test tools: BACK screenshots (BACK+Y flushes caches), counter on by default, and without an image in its own folder the XBE uses `F:\Applications\Melee-X\`'s (so variants can sit in folders of their own). A plain build is a release. `-DXHW_AUTOPAD=1 -DXHW_TEST_BUILD=0` is a release with scripted input |
| `-DXHW_AUTOPAD=1` | scripted input from `D:\autopad.txt` (`xhw_autopad.c`): `<frame> <buttons/SHOT/TSHOT/NEXT> [for N]` per line; `env NAME=VALUE` lines feed `getenv` ("Straight into a match") |
| `-DXHW_PROF=1` | sampling profiler: `[PROF]` every 20 s, `prof.bin` per match (`[PROFH]` with autopad) |
| `-DXHW_PROF_SECS=<n>`, `-DXHW_PROF_TOP=<n>` | profiler report period (default 20 s); buckets and call sites per report (default 192) |
| `-DXHW_PERF_SECS=<n>` | `[PERF]` period (default 5 s) |
| `-DXHW_PMC=1` | console probe (with `-DXHW_PROF=1 -DXHW_AUTOPAD=1`): one pair of Pentium III counter events per `[PERF]` period as `[PMC]` (ten pairs in turn; off in xemu), and `[GPUP]` per period (frames that found the GPU busy, the wait, frame span and lag). `env MX_ABLATE=1` rotates ablation windows every two periods (`[AB]`: FTZ, no shadow maps, no reflection, no back end, no display-list rechecks, no audio); `env MX_ABLATE=0,2,3,8,9` only those (8: every draw scissored to one pixel, 9: no EFB copies). `tools/xbox/probe_report.py`; `scenarios/probe`, `probe2` |
| `-DXGX_CENSUS=1` | `[CENSUS]`/`[DLCC]` every 600 frames (`tools/xbox/census_report.py`) |
| `-DXHW_CRASH_GUARD=0` | no SEH guard: crashes become bugchecks and demand-committed memory stops working; debug only |
| `-DXHW_WATCHDOG=0`, `-DXHW_HEARTBEAT_SECS=<n>` | hang dumper off; `[BEAT]` period (0 = off) |
| `-DXHW_WATCHDOG_SECS=<n>`, `-DXHW_WATCHDOG_BOOT_SECS=<n>`, `-DXHW_WATCHDOG_PRESENT_SECS=<n>` | watchdog: seconds without retraces (default 6), without a first frame after boot (45), of retraces without presents (10) |
| `-DXHW_AUDIO_APU=0` | never use the xemu APU fallback |
| `-DXHW_AUDIO_TEST=<bits>` | xemu audio tests (with `-DXHW_AUDIO_APU=0`, `scenarios/relaunch`): 1 leaves the AC97 engine running into the next boot at a relaunch; 2 reads CIV as 0 for good (an engine stuck from boot: the give-up path). Never on a console build |
| `-DXHW_NO_SPLASH`, `-DXHW_SPLASH_MS=<n>`, `-DXHW_SPLASH_DUMP` | boot title card off; its hold time; stream it as `[FBDUMP]` |
| `-DXHW_FBDUMP_EVERY=<n>` | screenshot every n presented frames |
| `-DXHW_VIDEO_480_BPP=16` | 640x480 at 16 bits (R5G6B5, Z16, 720p's pool sizes): the 720p path at a size xemu can show. Test only: shots are 16-bit |
| `-DXSDK_FPS_DEFAULT=<0/1>` | the counter's default when `settings.ini` has no `fps` line (default `XHW_TEST_BUILD`) |
| `-DXSDK_SETTINGS_RESET` | test builds: delete `settings.ini` at boot, then copy `D:\settings.ini` over it if staged (`MX_STAGE_EXTRA`) |
| `-DXSDK_ARAM_VERIFY=1` | compare every ARAM copy left on the disc (`ar.c`) with the image; `[AR] verify:` |
| `-DXSDK_MEM1_VA`, `-DXSDK_MEM1_SIZE`, `-DXSDK_ARAM_VA` | MEM1 and ARAM placement (0x10000000, 24 MB; 0x12000000): layout constants, not test knobs |
| `-DXGX_EFB_GPU_COPY=0` | EFB copies read back on the CPU (into A8R8G8B8 textures) instead of drawn by the GPU |
| `-DXGX_COPY_FIX=<bits>` | EFB copy surface switches: 1 resend the pitch after the format, 4 resend target DMA objects, pitch and offsets after a wait for idle, 2 one `CLEAR_SURFACE` for colour and depth (untried); default 5, 0 is v45's. Test builds read `env MX_COPY_FIX=` |
| `-DXGX_COPY_STRESS=<n>` | repeat each self-clearing EFB copy n more times into a scratch texture (picture unchanged): makes copy GPU faults frequent. Test builds read `env MX_COPY_STRESS=`; `scenarios/stall` |
| `-DXGX_TILE=<bits>` | re-program pbkit's tile regions (`renderer.md` "Tile regions"): 1 Z16 depth compresses as Z16, 2 tile 0 enable bit `base \| 1`, 4 `base \| 3` (wins over 2), 8 no Z compression; default 4, 0 pbkit's. With tile 0 on the CPU reaches the framebuffers through the aperture (`[NV2A] tiled framebuffers ... aperture`). Picture must not change. Test builds read `env MX_TILE=`. Console only (xemu ignores tiles) |
| `-DOCX_Z16_TILE_FLAGS=<flags>` | pbkit's Z16 depth tile flags (`patch_pbkit.py`); default `0x84000001`; A/B `0x80000001` (no 32-bit flag), `0x00000001` (uncompressed). Console only |
| `-DXGX_Z16_DEPTH_RATIO=<n>` | Z16 depth as if the extreme camera's near plane were at far / n (default 4096; `renderer.md` "Depth"); 0: GX's depth as is |
| `-DXGX_DEPTH_CULL=1` | cull pixels outside the clip depth range instead of clamping (pre-v15) |
| `-DXGX_OVERLAP=0` | `xgx_present` waits for the GPU before the flip (up to v32) instead of at the next frame's first GPU use |
| `-DXGX_PB_KICK=<words>` | pushbuffer words per kick (default 8192) |
| `-DXGX_VB_CACHE_BREAK=0` | no `BREAK_VERTEX_BUFFER_CACHE` at each batch start |
| `-DXGX_VBUF_FREE_NOW=0` | evicted display-list vertex buffers go through the deferred free |
| `-DXGX_TEX_POOL_KB=<n>` | texture pool size (default 8192 at 480, 6144 at 720p) |
| `-DXGX_STATS_EVERY=<n>` | `[NV2A]`/`[TEX]` stats period in frames (default 600) |
| `-DXGX_NO_INDIRECT=1` | GX indirect stages draw direct (no BUMPENVMAP; up to v40) |
| `-DXGX_DEBUG_TRACE` | log every draw of the frame a `SHOT` or BACK captures as `[DRAW]` lines (~400 KB, in `trace.log`); on a BACK frame each EFB copy's source is also a `shotNN.bmp` (up to 8), with autopad also `[FBDUMP]`. `env XGX_SKIP=a-b` leaves those draws out of the traced frame |
| `-DXGX_DEBUG_VPTRACE[=<n>]` | vertex-program selects of two frames every n (default 600) as `[VPT]`; replay with `vp_policy.py boot.log` |
| `-DXGX_DEBUG_EFBLOG` | first 200 EFB copies as `[EFB]` lines |
| `-DXGX_DEBUG_PBCHECK` | parse every pushbuffer segment before its kick, `[PBCHECK]` on malformed headers (first 16): a bad stream vs a GPU fault in xemu |
| `-DXGX_CHECK_VERTS` | positions decoded by display-list builds at 2^20+, infinite or NaN logged as `[WARN] dlist` (first 32) |
| `-DXGX_DEBUG_NOMIP`, `-DXGX_DEBUG_NOZ`, `-DXGX_DEBUG_NOCULL`, `-DXGX_DEBUG_NOEFB` | bind only base mip levels; depth test off; face culling off; every EFB-to-texture copy dropped |
| `-DXGX_DEBUG_MAGENTA` | textures the pool couldn't take draw magenta instead of untextured |
| `-DPC_AUDIO_SCALAR` | the mixer in plain C instead of SSE1 (`test_audio_mix.py` builds both) |
| `env MX_LOCKSTEP=1` | game clock moves 1/60 s per frame: every frame is one tick ("Comparing builds by screenshot"); pacing off, `[PERF]` keeps the real clock |
| `env MX_SIMH_VERBOSE=1` | `[SIMH]` every tick |
| `env MX_NEXT_XBE=<path>` | 12 s after the match (or at `NEXT`) launch that XBE (`F:\Applications\<folder>\default.xbe` or a `\Device\` path), or `dashboard`; chains a console round |
| `env MX_VIDEO=480\|480i\|720` | video mode over settings.ini's (`[VIDEO] MX_VIDEO=`); 720 still needs the dashboard to allow it |
| `env MX_FILL_E=1` | fill E: at boot (`E:\mx_fillN.bin`) to test the full-disk paths (failed saves and shots with notices, `[BOOT] can't write`, log on `D:\`). Use a copy of xemu's HDD; without the line the files are deleted |
| `env MX_COPY_FIX`, `MX_COPY_STRESS`, `MX_TILE`, `MX_ABLATE` | run-time overrides of the switches above |
| `N SHOT` | (script) screenshot at frame N: `[FBDUMP]` in xemu, `shotNN.bmp` on the console |
| `N TSHOT` | (script) screenshot at the match's tick N (as `[SIMH]` counts): the same moment on every run with `MX_LOCKSTEP=1` |
| `N NEXT` | (script) launch `MX_NEXT_XBE` at frame N (runs without a match); logs `[AUTOPAD] NEXT` |
| `N BACK` | (script) press BACK: a screenshot in a test build (with `-DXGX_DEBUG_TRACE` the traced frame) |
| `XBOX_LTO=1`, `XBOX_PGO=gen\|<file>` | build knobs (`docs/toolchain.md`, `docs/pgo.md`): ThinLTO; PGO instrumented (`XHW_PGO`, `[PGOC]` counters at each scene exit) or optimized with a `.profdata` |
| `XBOX_FORCE=1`, `XBOX_KEEP_TEMPS=1` | `compile_game.py` rebuilds every game unit; keeps the `.i`/`.lowered.c` intermediates |
| `XBOX_CFLAGS`, `XBOX_CMAKE_ARGS`, `XBOX_NINJA_ARGS` | passed through by `xbox/build.sh` |

## Straight into a match

With an autopad build, `env` lines set what `getenv` returns, so
melee-pc's harness hooks work on the Xbox:

```
env MELEE_BOOT_SCENE=vs        # skip the menus: debug VS (gmvsmode.c); =title: the title screen (BACK: settings menu, scenarios/settings)
env MELEE_DEBUG_VS_STAGE=10    # StKind (src/melee/gr/forward.h): 10 = Mute City
env MELEE_DEBUG_VS=cpu4        # Link, Mario, Fox and DK as four CPUs
env MELEE_DEBUG_VS_TIME=20     # a 20-second timed match: ends on TIME!
env MELEE_DEBUG_VS_STOCKS=3    # stock match
env MELEE_DEBUG_VS_CHARS=4:1h,0 # CKind[:costume][h] (yellow Kirby on port 1, Falcon CPU)
env MELEE_DEBUG_VS_ITEMS=3     # items on, hex ItemKind mask (capsules, crates)
env MELEE_DEBUG_VS_INVISIBLE=3 # players 1 and 2 cloaked (indirect refraction)
env MELEE_DEBUG_KIRBY_HAT=2    # Kirby spawns with that FighterKind's copy
env MELEE_SEED=1               # fixed random seed (also the attract demo's pick)
env MELEE_NO_ATTRACT=1         # no attract loop
300 SHOT
```

A slow load in xemu may trip the watchdog ("frames stopped"); it logs
`frames again` and the run carries on.

## First-boot checklist

1. Boots past the "no disc image" screen; `boot.log` has the `[DVD]` line.
2. The title screen draws, and `[NV2A]` lines appear.
3. Audio plays on hardware and in xemu.
4. Controllers map to players 1-4 by port; hot-plugging doesn't shuffle them.
5. 720p is 16:9 with the HUD at the edges; 480 (4:3) matches the GameCube.
6. A 4-player VS match on a busy stage keeps full speed (`ticks per render`
   under 5) at 30 fps or more.
7. Saving creates `card_a\01-GALE-*.gci`, and it loads back after a reboot.
