# Frame-rate plan (2026-10-03, dev at d1786ec)

For the session that executes it. Read `CLAUDE.md`, `docs/handoff.md`,
`docs/testing.md` ("Measuring on the console", "Performance runs in xemu") and
`docs/renderer.md` ("CPU cost of the back end") first. This replaces "Ideas
left" in `docs/roadmap.md`. Keep the status table at the end current.

## Goal

More frames per second in matches on the console, with the same picture and
the same simulation results. Menus already run at 60.

| case | today (console) | CPU per rendered frame has to drop |
|---|---|---|
| 4 players, Fountain of Dreams: 30 steady | 21-22 | ~29% |
| 4 players, typical stage (Peach's Castle): 40 | 33 | ~17% |
| 1v1 or a light stage: locked 60 | 40-52 | 0-28% |

What to expect: the toolchain track (A) should cut instruction counts by
15-30%, and how much of that the console shows depends on its stalls
(round 2 measures it). A step change can only come from the stall track (B),
if round 1 finds one dominant cause, or from drawing less (C, F).

## What the data says

### The frame is a cost per draw plus a cost per tick

Medians of the console burn-in logs (`~/xemu/hw/logsburn-v39`, `-v42`,
`-v43b`, `logs44*`, `logs45burn`; 4-CPU matches, 480i and 720p alike) against
instruction counts from xemu run with `-icount` (dev, Fountain 4-CPU). The
raw logs are on the Mac, not in the repo; the numbers here are what carries
over to another machine.

| `[PERF]` bucket | console | xemu `-icount` | instructions per cycle |
|---|---|---|---|
| render (HSD walk, GX front end) | 18-21 µs a draw = ~14.3k cycles | ~4.7k instructions | ~0.33 |
| draw (back end) | 8-10 µs a draw = ~6.6k cycles | ~3.2k | ~0.48 |
| dlist | 2.3-3.3 µs a draw = ~2.1k cycles | ~0.4k | ~0.19 |
| sim, per tick, 4 CPUs | 4.0-5.0 ms = ~3.3M cycles | ~0.9-1.1M | ~0.3 |

With D draws a frame, c = ~31 µs a draw and T ms a tick, the console's frame
rate is

    fps = (1000 - 60 T) / (D c)

Fountain (D ~1025, T 4.5): 21.3 predicted, 21.3 measured. Peach's Castle
(D ~710, T 4.9): 33.3 predicted, 33.0 measured. GPU waits are ~0. The audio
mixer thread takes 7-10% of the CPU inside every bucket.

So: the ticks cost 27-30% of the CPU whatever the frame rate, the rest is
draws times a fixed cost, and a millisecond saved per tick is worth 60 ms a
second while one saved per frame is worth fps ms a second.

### The console runs at ~0.3-0.5 instructions per cycle

A Pentium III does about 1 on code that stays in its caches, so roughly
60-70% of the console's CPU time is stalls, not work. xemu cannot see them
(TCG has no caches) and the sampling profile cannot tell a slow instruction
from a stalled one. Suspects, none measured yet:

- instruction fetch: 4.27 MB of `.text` laid out alphabetically by source
  path, hot paths spread over it, 16 KB of L1 code cache, a 32-entry ITLB
  (128 KB of reach), and a per-draw path that alternates HSD, the GX front
  end, the back end and pbkit;
- data: 128 KB of L2, no hardware prefetcher, pointer-chasing over a ~20 MB
  heap, display-list rechecks that sample scattered words (dlist: 0.19);
- TLB: MEM1 is 4 KB pages, the DTLB has 64 entries (256 KB of reach);
- FP assists: MXCSR is never set, so flush-to-zero is off; a denormal
  operand traps to microcode on a P3 and costs nothing extra in xemu.

The two consequences that shape this plan: instruction counts (all xemu can
verify) understate and misrank the console's costs, so the stalls get
measured on the console before any structural work is chosen; and work that
is skipped entirely takes its stalls with it.

The instruction counts assume xemu's TSC follows virtual time at 733 MHz
(1 ms of a bucket = 500k instructions at `shift=1`); step 0.1 checks that,
and round 1's `INST_RETIRED` settles the ratio. The console logs and the
xemu run are not the same match (the burn-ins had items on), so read the
ratios as good to about a third.

### Other facts

- The profile is flat: the report's top 192 buckets (12 KB of code) hold
  ~60% of the samples, no function has 5%. The last console profile is v32;
  v33-v35 changed the picture.
- Fighters are drawn in up to four passes: main (high-poly set), a shadow
  map each (low-poly set, no material setup, one EFB copy per fighter per
  frame), and on Fountain two reflection passes (low-poly set, full
  materials). Fox's low-poly set is ~33 PObjs, Mario's ~17, a PObj is a draw.
  On Fountain with four fighters the extra passes are roughly a quarter to
  a third of the frame's draws.
- Fountain's working set of cached display lists does not fit the 4 MB
  vertex pool: 550-1460 rebuilds per 600 frames in a fresh xemu match,
  700-3300 on the console, pool at 2-465 KB free.
- Back end, per draw (console, v45 Fountain): 22% change nothing, 15% only a
  position matrix; 57% load a position matrix, 51% rebind maps, 23% change
  TEV. 54 vertex programs are uploaded and ~200 switched a frame.
- EFB copies: ~6 a frame, each with two extra waits for idle since
  `XGX_COPY_FIX=5` (v46). Their cost in a normal match has not been measured
  (stress logs suggest ~0.1 ms a copy).
- Build: `-O2 -march=pentium3`, no LTO, no PGO, link order = source order.
  362 mingw `.refptr` stubs. `__assert` is already `noreturn`.
- Free RAM in a match: 6.8 MB at 720p, 7-9 MB at 480. MEM1 is ~20 of 24 MB
  committed. The pushbuffer, vertex ring and both pools are write-combined;
  the pools' bookkeeping is in cached memory.
- `lbArq_80014BD0` (fighter animation loads) spins until the ARQ worker
  thread runs the completion callback, after `ARQPostRequest` has already
  copied the data: two thread switches per load on the console (~0.35% of
  the CPU in v32), and under `-icount` ~15% of the sim bucket's
  instructions.

## Rules

- Own worktree and branch; nothing goes on `dev` or `main` except through
  the main session's merge gate (`docs/handoff.md`): code review, host
  tests, `gl` shots against the baseline plus the scenes a change touches.
- Same picture. Anything that changes it is track F: off by default, built
  only when the user says so.
- Same simulation results. Rewrites of game math are bit-identical
  (`test_anim_mtx.py`'s pattern). Toolchain changes keep
  `-ffp-contract=off` and no fast-math. Before LTO or PGO: log the game
  thread's x87 control word (xemu runs the real kernel, so xemu will do).
  At 53-bit precision inlining can't move a double's rounding; at 64-bit it
  can, so stop and say so.
- 64 MB. A match keeps at least 5 MB free; quote `[BEAT]` free memory for
  every change that allocates.
- Every edit to imported code: `PORT:` comment and a line in
  `docs/decisions.md`. Every new switch: `docs/testing.md`'s table,
  `docs/renderer.md` where it applies, `docs/decisions.md`.
- xemu: one instance. Where other Claude sessions share the machine's xemu
  (they do on the Mac), look for a running one before a run
  (`pgrep -fl Xemu.app`; `tasklist | grep -i xemu` on Windows) and wait if
  it isn't yours. Kill only the one you started, not every xemu
  (`CLAUDE.md`'s `pkill`, `taskkill //IM`): a run killed that way looks
  like a short log, not an error.
- Console rounds are the user's time: one deploy per round, switches read
  from the autopad script's `env` lines so an A/B is a script swap, the
  audio check list before each build, logs pulled before the next boot.
  Hand over the build with a five-line instruction and keep working on
  what does not need its result.
- Each change reports: instructions per draw and per tick before and after
  (step 0.1), image size, free memory, shots, and the console's `[PERF]`
  once a round has run it.

## Step 0: tools (xemu only)

0.1 **Instruction counts.** `MX_XEMU_ARGS="... -icount shift=1,sleep=off"`
makes `[PERF]`'s CPU buckets instruction counts, free of softfloat skew and
host noise. Tried on the Mac only (M4, xemu 0.8.136), where a match runs at
about real speed. It needs TCG: if the PC's xemu runs under WHPX, add
`-accel tcg`, and check the speed there before relying on it. Add
`tools/xbox/icount_report.py`: medians over a match's rows, as instructions
per draw (render, draw, dlist) and per tick (sim). Check the 500k-per-ms
scale once with a loop of known length. Two runs of one build must agree
within ~1% before it gates anything. Ignore `gpu` and `vsync` (spin loops).
Baselines for `fodperf`, `gl`, and a new 2-CPU Final Destination scenario.

0.2 **Whole profile.** Dump every bucket (all samples and sim-only), not
the top 192: `[PROFH]` hex lines on COM1 at the end of a match in autopad
builds, `E:\UDATA\4d580001\prof.bin` on the console (`console.py pull`
fetches it), `prof_report.py --full`. One report at the end, not every
20 s (reports hitch). Fix the symbol gaps first: the in-match xemu profile
credits samples to `pc_load_disc_fonts`, so some static functions resolve
to a neighbour. Under `-icount` this is an instruction profile.

0.3 **Draw census** (`-DXGX_CENSUS=1`): draws, vertices and dirty bits per
600 frames by pass and owner (fighter main, fighter shadow, fighter
reflection, stage, items, effects and particles, HUD), from a marker set
around `render_cb` in `gobj.c` (lines 138 and 185) and around the passes
in `lbshadow.c` and `grizumi.c`. Counts are the same on the console.

0.4 **Display-list census:** cached bytes by owner, lists cached under more
than one key, the lists rebuilt most (for C2).

0.5 **Simulation check:** a hash of the fighters' state (position,
velocity, damage, action state) and the random seed, logged every 60 ticks
in autopad builds as `[SIMH]` (melee-pc's netplay code may already have
one). Two runs of one build must agree first; if they don't, find what ties
the simulation to frame timing before trusting it. Then every toolchain
and simulation change must reproduce the baseline's hashes on `gl` and
`fodperf`.

## Step 1: console round 1, the probe build

`-DXHW_PROF=1 -DXHW_AUTOPAD=1 -DXHW_PMC=1`, one boot, ~5 minutes: a 4-CPU
Fountain match from a `scenarios/probe` script. It answers where the stall
time goes and what each pass costs on hardware.

1.1 **Performance counters** (new `xhw_pmc.c`; everything is ring 0):
event selects in MSRs 0x186/0x187 (bits 16 and 17 count all rings, bit 22
of 0x186 enables both), counters read with `rdpmc`. None of it runs in xemu
(`running_in_xemu()` in `xhw_audio.c`; `rdpmc` is undefined under TCG).
`xhw_perf.c` reads both counters at each bucket switch and keeps sums per
bucket; each `[PERF]` period logs a `[PMC]` line and moves to the next
pair. Event codes from the P6 table, to be checked against the Intel SDM:

| pair | counter 0 | counter 1 | answers |
|---|---|---|---|
| 1 | 0x79 `CPU_CLK_UNHALTED` | 0xC0 `INST_RETIRED` | instructions per cycle, per bucket |
| 2 | 0x86 `IFU_MEM_STALL` | 0x48 `DCU_MISS_OUTSTANDING` | cycles stalled on fetch; on data misses |
| 3 | 0x81 `IFU_IFETCH_MISS` | 0x85 `ITLB_MISS` | L1 code misses, ITLB misses |
| 4 | 0x28 `L2_IFETCH` (umask 0x0F) | 0x29 `L2_LD` (0x0F) | what reaches L2 |
| 5 | 0x24 `L2_LINES_IN` | 0x45 `DCU_LINES_IN` | L2 and L1 data misses |
| 6 | 0xC5 `BR_MISS_PRED_RETIRED` | 0xC4 `BR_INST_RETIRED` | mispredicts |
| 7 | 0xA2 `RESOURCE_STALLS` | 0x11 `FP_ASSIST` (counter 1 only) | denormals, store buffer |
| 8 | 0x14 `CYCLES_DIV_BUSY` (counter 0 only) | 0x13 `DIV` (counter 1 only) | divides |
| 9 | 0x03 `LD_BLOCKS` | 0x05 `MISALIGN_MEM_REF` | store forwarding, split loads |
| 10 | 0x07 `EMON_KNI_PREF_DISPATCHED` | 0x4B `EMON_KNI_PREF_MISS` | do today's prefetches fetch anything |

The P6 has no DTLB-miss event; B2 is decided by an A/B.

1.2 **Boot probes**, one `[CPU]` block: CR0, CR4, MXCSR and its mask (is
DAZ there), the x87 control word, the MTRRs and PAT, CPUID 2, the page
directory entries of 0x00000000-0x1FFFFFFF and 0x80000000-0x8FFFFFFF with
their PS bits (does the kernel map contiguous memory with 4 MB pages?).
At the end of the match: MEM1's committed chunks per 4 MB range.

1.3 **Rotating ablations**, 10 s each, cycled through the match and logged
as `[AB] n name` so each `[PERF]`/`[PMC]` window has a label. Platform-side
flags behind one accessor that returns 0 outside test builds:

| window | change | tells |
|---|---|---|
| 0, 7 | none | baseline, drift |
| 1 | MXCSR flush-to-zero on | B1, with `FP_ASSIST` |
| 2 | fighter shadow maps off (hook in `lbshadow.c`) | cost of the shadow passes and their copies |
| 3 | Fountain reflection off (hook in `grizumi.c`) | cost of the reflection passes |
| 4 | back end off (`xgx_draw` returns at once; black frames) | back-end cost, and whether HSD's bucket gets faster per draw without it (B4) |
| 5 | display-list content rechecks off | their share of dlist (B5) |
| 6 | audio mixer off (silence) | audio's real cost, cache pollution included (E) |

All of step 1 is test-build only; a plain build has none of it.

1.4 The whole profile (0.2), the census (0.3), `[NV2A]`/`[DLC]` as usual.

1.5 Hand-over: `console.py stage`/`deploy`, the script uploaded as
`/F/Applications/Melee-X/autopad.txt`, launch, wait for the results screen,
back to the dashboard, `console.py pull`, delete the script. A second
script (Peach's Castle, or 2-CPU Final Destination) for a second boot if
the user has time.

## Work items

Expected gains are of CPU time in the buckets named; "console" means the
gain can only be measured there.

### A. Toolchain (start right after step 0; correctness in xemu)

| | what | expect | risk |
|---|---|---|---|
| A1 | `-fno-auto-import` on game and sdk objects: no `.refptr` stubs | small | none; must link |
| A2 | `-ffunction-sections` and lld-link `/order:@order.txt` from the profile (`tools/xbox/make_order.py`): sim-hot functions together, then render-hot, the rest as today | 0-10% everywhere, console | none: no code changes |
| A3 | ThinLTO over game + sdk objects (same triple; hw and the nxdk libraries stay native): GX setters and matrix loads inline into HSD | 5-15% of render and sim instructions | mismatched prototypes across units; the map, `.statics` and `sym.py` must follow (`/lldsavetemps` or `/lto-obj-path`) |
| A4 | PGO from xemu: `-fprofile-generate`, counters dumped over COM1 at match end by a small freestanding writer built from `llvm/ProfileData/InstrProfData.inc` (the image has no compiler-rt; start with value profiling off), `llvm-profdata merge`, `-fprofile-use`. Branch counts are the same on the console. Train on `gl`, `fodperf`, `ps`, `corn`, a 1v1, menus and results | 5-15% more, plus hot/cold layout and call-graph function order | image size of the instrumented build; where the `.profdata` lives (GitHub release builds need it: ask the user) |
| A5 | size against speed: cold code at `-Oz` after A4; whole game at `-Os` as a console A/B (a 16 KB code cache may prefer it) | console | none |

Each of A2-A5 becomes its own XBE for round 2 (separate
`XBOX_GAME_OUT` and deploy folder).

The flags were tried in the Docker image only (clang 21.1.8):
`-fno-auto-import` drops the stub, `-flto=thin` gives bitcode for the game
triple, `-fprofile-generate` emits `.lprfc`/`.lprfd`/`.lprfn` and refers to
`__llvm_profile_runtime`, `InstrProfData.inc` is at raw version 10,
`llvm-profdata` is there, `__attribute__((uninitialized))` stops the
zeroing. MSYS2's clang is the same version: confirm its `llvm` package has
`llvm-profdata` and that header.

### B. Stalls (chosen by round 1)

| | what | do it when | notes |
|---|---|---|---|
| B1 | MXCSR flush-to-zero (and DAZ if the mask has it) on the game and mixer threads | `FP_ASSIST` costs ≥ 1% of cycles, or window 1 is ≥ 2% faster | changes results only where a float result is denormal. Check whether the GameCube ran with `FPSCR[NI]` set (then this is the faithful setting) before it is the default |
| B2 | MEM1 on 4 MB pages | DTLB can't be counted: build it as `env MX_MEM1_LARGE=1` and A/B in round 2 | easiest if 1.2 shows the kernel's 0x80000000 map already uses 4 MB pages (put MEM1 there); otherwise 4 MB-aligned contiguous blocks and our own PDEs, every commit path skipped for them, and DVD reads into them checked against the kernel's buffer locking (bounce if it walks PTEs). Only ranges ≥ 90% committed in 1.2. Drop below 3% or at the first instability. The same for `.text` if pair 3 stays high after A2 |
| B3 | prefetch plans: per tree root, the nodes the last walk touched, in order; the walk prefetches three or four ahead and rebuilds the list when it diverges. `HSD_JObjAnimAll` and `HSD_JObjDispAll`. Side tables only | data-miss stall dominates sim and render, and pair 10 shows today's prefetches hit | console |
| B4 | deferred back end: the GX front end appends a compact record per draw (primitive, vertices, dirty bits, copies of the dirty state groups) to a 32-64 KB ring; a flush replays them through today's `xgx_draw`. Flush on a full ring, an EFB copy, a texture upload or free, the present | fetch stall ≥ 20% of render + draw, or window 4 makes HSD's bucket ≥ 10% faster per draw | keeps each phase's code in the cache for many draws in a row; also the base for sorting later |
| B5 | display-list and texture validation by events (game heap frees and loads bump an epoch for their range) instead of sampled rechecks | window 5 saves ≥ 1 ms a frame | xemu shows correctness |

### C. Work removed, same picture (xemu shows the counts)

- C1 **Census-led.** Check: shadow maps rendered for a fighter whose shadow
  is not applied that frame (`lbshadow.c` renders the map whenever the
  shadow has objects and never reads the flag `HSD_ShadowSetActive` sets),
  the second reflection pass, off-screen items and effects. Skip what
  provably draws nothing.
- C2 **Vertex pool thrash on Fountain** (0.4): duplicates under several
  keys first, then denser vertices (packed normals, 16-bit texture
  coordinates where GX's are), then pool size against the 5 MB floor.
- C3 **EFB copy waits:** read `gpu` and idle waits per frame from round 1.
  Above 0.5 ms a frame, see how much queued work each wait sits behind and
  reorder or merge them; the stall fix must survive `scenarios/stall` on
  the console.
- C4 **ARQ completion without the thread hop:** the data is copied inside
  `ARQPostRequest`; run a blocking request's completion in line.
- C5 **Envelope blends once per frame:** the weighted joint blend does not
  depend on the view; cache it per envelope under an epoch that any joint
  matrix rebuild bumps, and let each pass only concatenate its view matrix.
  Same operations, same bits. Today's memo is per DObj.
- C6 **Material setup memo:** only if `HSD_MObjSetup` and its callees are
  ≥ 15% of the render bucket in the whole profile. Validate by comparing a
  compact copy of the inputs (never assume a material is static), replay a
  saved block of GX material state, keep HSD's own state cache consistent,
  leave the fighters' custom setup (`ftmaterial.c`) out at first.
- C7 **Vertex-program uploads** (54 a frame): `XGX_DEBUG_VPTRACE` and
  `vp_policy.py`, as the roadmap had it.

### D. Simulation (27-30% of the CPU)

The console's sim-only profile first (0.2 in round 1). Candidates from the
xemu profiles: stage collision (`mpLib_800511A4_RightWall`,
`mpLib_800515A0_LeftWall` and the floor checks test every line of every
near joint: an exact bounding reject per line), the dynamics solver
`lb_8001044C` (several hundred bytes of locals zeroed per call by
`-ftrivial-auto-var-init=zero`: mark audited ones `[[clang::uninitialized]]`,
never drop the flag), `FObj` interpretation, trig. Bit-identical only.

### E. Audio (7-10% of the CPU)

If window 6 confirms it: the mixer's per-sample loops in SSE1, four samples
a step, the same operations per lane; `test_audio_mix.py` is the gate.

### F. Picture trades (the user decides; default off)

A `[video]` option: fighter shadows updated every second frame in turn,
Fountain's reflection every second frame or off. Windows 2 and 3 give the
numbers to decide with; the estimate for four fighters on Fountain is
+8-15% at half rate and +15-40% with both off. Not built until the user
asks.

## Order

1. Step 0, baselines recorded here.
2. The round 1 build handed to the user. Don't wait for it.
3. A1, A2, A3, A4 in that order, each gated in xemu (shots, simulation
   check, instruction counts); C1, C2, C4 alongside.
4. Round 1's logs: fill in the table under "What the data says" with the
   measured stall shares, pick B items by their conditions, update this file.
5. Round 2: one deploy with the toolchain variants as separate XBEs and the
   runtime candidates as `env` switches; `[PERF]` on Fountain 4-CPU and one
   light scenario decides what merges.
6. D and E once the console profile names their targets.

## Picking this up

Nothing is implemented: this file, written on the Mac on 2026-10-03, is the
whole of the work so far, and no code changed. The session that executes it
starts from this prompt (any machine; `docs/handoff.md` has the Windows
setup):

```
You are the fps executor for Melee-X, the native original-Xbox port of Melee.
Goal: raise the console's frame rate in matches as far as it will go, with the
same picture and the same simulation results.

Read CLAUDE.md, docs/handoff.md, then docs/fps-plan.md and the docs it names.
Follow the plan's Rules and Order. Do not re-plan.

Setup: create your own worktree and branch `fps-exec` from `dev`. Never commit
to, merge into or push dev or main. Commit each finished item on your own
branch.

Work, in order:
1. Step 0 tools: icount report, whole-profile dump, draw and display-list
   census, simulation hash. Record this machine's baselines in the plan's
   Status table.
2. Build the round 1 probe build (Step 1). Stage it with console.py, write the
   scenario and a five-line run instruction for the user, then continue
   without waiting for the console.
3. Track A items A1 to A4 in order. Gate each in xemu: host tests, gl shots
   against the baseline, equal simulation hashes, instructions per draw and
   per tick before and after. Then C1, C2, C4.
4. When the user hands you round 1 logs: fill in the measured stall shares,
   choose B items by their conditions, prepare round 2 as one deploy.

Stop and ask before: any change to the picture (track F), any change to
simulation results (B1 as a default), committing a .profdata, going below
5 MB free in a match, or continuing with LTO if the x87 control word is at
64-bit precision.

xemu: one instance. Check for a running one first and kill only your own,
never every xemu. Never capture the desktop.

Report per item: what changed, numbers before and after, risks, and what the
console still has to confirm. Keep the plan's Status table current.
```

## Status

Windows PC (2026-10-03): xemu runs `-icount shift=1,sleep=off` under TCG
(no WHPX), a `gl` match in ~2.5 minutes; `[CAL]` measures 499.9
instructions per guest microsecond. Instructions below are aggregates of
`tools/xbox/icount_report.py` (audio mixer taken out): render + dlist +
draw per draw / sim per tick. Two runs of one build agree within 0.6% on
`gl`; Fountain's sim per tick varied 5% until C4 (the lbArq spin), now
0.5%. MSYS2's LLVM 21.1.8 has `llvm-profdata` and `InstrProfData.inc`
(raw version 10); `-fno-auto-import`, `-flto=thin` and
`-fprofile-generate` work for the game triple. The game thread's x87
control word is `027f` (53-bit precision) on xemu and the console: LTO
may go ahead.

| item | state | instructions (draw / tick) | console |
|---|---|---|---|
| baseline, dev d1786ec, Fountain 4-CPU | first reading 2026-10-03 (Mac) | ~8.3k / ~0.9-1.1M | 21-22 fps (v43-v45) |
| baseline, PC, step 0 tools (69bddad) | `gl` / `fodperf` / `fd2` | 7174 / 1.57M; 7740 / 1.03M (�5%); 7781 / 586k | |
| step 0 tools | done: `[PERFX]`, `[CAL]`, `icount_report.py` (0.1); `[PROFH]`, `prof.bin`, `--full` (0.2); `[CENSUS]`, `[DLCC]`, `census_report.py` (0.3, 0.4); `[SIMH]` (0.5); the pad-alarm wait charged to vsync (it was idle time in sim) | | |
| round 1 probe build | v49 (196db39): user quit after 130 s, counters good; v50 (345d197, + prefetcht0 umask test) deployed for the full run | | v49: IPC sim 0.34, render 0.35, dlist 0.26, draw 0.53 |
| A1 `-fno-auto-import` | done 0005392: 363 `.refptr` -> 1, `.text` -2.5 KB, gl shots same | -0.3% / 0 (gl); -0.4% / 0 (Fountain) | |
| C4 ARQ completion in line | done 68a9a42, `[SIMH]` equal, gl shots same | 0 / -9.2% (gl); 0 / -15.2% (Fountain) | |
| A2 function order | done: `-ffunction-sections`, `.text$*` renamed to `.text` before the link, `-opt:noicf`, `xbox/order.txt` (xemu instruction profiles of gl, fodperf, fd2, ps, corn); `/opt:ref` drops 133 KB of unreferenced code; `[SIMH]` equal, shots same | 0 / 0 (layout only) | round 2 |

Census (xemu, Fountain 4-CPU, 728 draws a frame): main pass 71% (fighters
314, stage 144, effects 30, HUD 21 a frame), fighter shadow maps 92 draws
(13%; 70% of them change only the position matrix), the reflection 119
(16%: fighters 85, effects 28). Green Greens: shadow maps 92 of 582. The
display-list cache holds 540-820 lists under a second format key, 1.4-1.7
MB of the 4 MB vertex pool, on both stages (C2's first target).

v49 console counters (Fountain 4-CPU, 26 periods, few per pair; the full
run is v50): instruction-fetch stalls (`IFU_MEM_STALL`) are 16-21% of sim's
cycles, 32-37% of render's, 19-27% of dlist's and 36-38% of draw's;
`L2_IFETCH` ~300k a frame in render with few `L2_LINES_IN`: the code fits
L2, not the 16 KB L1. `FP_ASSIST` 58k a frame in render, 13k in sim;
DAZ is not available (MXCSR mask ffbf). The kernel maps 0x80000000-
0x83FFFFFF with page tables, not 4 MB pages (`[CPU] pde`).
