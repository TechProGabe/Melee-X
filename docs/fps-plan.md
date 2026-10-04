# Frame-rate work (2026-10-03/04, finished)

The record of the frame-rate work (from dev d1786ec, merged from
`fps-exec`): outcome, method, every item and its result (Status), lessons.
Round-by-round logs and the original plan: git history before 2026-10-04.

## Outcome

Fountain of Dreams, 4 CPUs, 720p, the release build (console round 6):
**39.9 fps, +25% on dev's 32.0** (21-22 before the v4x work), with the
same simulation (`[SIMH]` over the whole match in xemu) and the same
picture (lockstep shots). Final Destination 1v1 holds 60; 480p +7% from
the tile alone (42.5 -> 45.5).

| change | gain (console, Fountain 720p) |
|---|---|
| ThinLTO + PGO (A3, A4), now how releases are built | +13% (rounds 2-3) |
| the colour framebuffer's tile region enabled (`XGX_TILE` 4) | +11% (rounds 4-5) |
| C4, C2, A1, A2, E (mixer), D (mplib), `PObjSetupMtx`, GX front-end lookups | the rest, inside the two above's runs; xemu: sim -18% a tick, render -19% a draw |

Where it stands: on Fountain at 720p the GPU is still busy at ~90% of
frame starts (3.4 ms a frame waiting) and the CPU spends ~22 ms a frame;
more needs both (fill without a picture change, and the render walk).
Open: one 2-pixel speck in one round 6 shot (`roadmap.md` item 10).

## Method

- xemu: `-icount shift=1,sleep=off` makes `[PERF]`'s CPU buckets
  instruction counts (`[CAL]`: 500 a µs); `icount_report.py` gives them per
  draw and per tick (two runs agree within ~0.6%). That, `[SIMH]` and
  lockstep shots (`env MX_LOCKSTEP=1`, `testing.md` "Comparing builds by
  screenshot") are the gate.
- Console: a round is one deploy of several XBEs or `env` switches chained
  with `env MX_NEXT_XBE`, run unattended, read afterwards:
  `tools/xbox/console_round.py N`, `N report`, `N shots`
  (`docs/testing.md` "Console rounds").

## Rules

Same picture (shots byte for byte), same simulation (bit-identical
rewrites, `-ffp-contract=off`, no fast-math; x87 control word `027f`, 53-bit,
so LTO can't move a double's rounding). At least 5 MB free in a match.
`PORT:` and `decisions.md` for imported code. One xemu at a time; kill only
your own.

## Steps 0 and 1: tools and probes

Test builds only. 0.1 instruction counts (`[PERFX]`, `[CAL]`,
`icount_report.py`); 0.2 whole profile (`[PROFH]`, `prof.bin`,
`prof_report.py --full`); 0.3 draw census (`-DXGX_CENSUS=1`) and 0.4
display-list census, `census_report.py`; 0.5 `[SIMH]` every 60 ticks
(`simhash.c`). Step 1, the probe build (`-DXHW_PMC=1`, `scenarios/probe`):
1.1 P6 counters per bucket (`xhw_pmc.c`, `probe_report.py`), 1.2 a `[CPU]`
block, 1.3 rotating ablation windows (`env MX_ABLATE`, `[AB] n`): FTZ, no
shadow maps, no reflection, no back end, no dlist rechecks, no audio, later
no fill and no EFB copies.

## Console rounds

### Console round 1

v50, 303 s: 32.3 fps. IPC 0.31-0.55 (a P3 does ~1): 60-70% stalls. Render
48% of the frame (fetch stall 39%), sim 23% (data misses), draw 17%, dlist
7%, audio 5%. Picked B2, B3 (sim), B4, C3, E; not B1, B5.

### Console round 2

ThinLTO +3.5%, LTO + PGO 35.0 fps (+11.6% on the same code without). B4
-6%, B3 noise: rejected. The GPU busy at 94% of frame starts.

### Console round 3

LTO + PGO with E/D/`PObjSetupMtx`: **36.2 fps, +13% on dev**; B2 nothing.
First build of a chain ~4% slow. GPU frame ~30 ms: fill ≥ 9-10 ms,
reflection 4.4, shadow maps 2.2, EFB copies ~0. Hardware `[SIMH]` parts at
tick 4440-4500 into three outcomes (`roadmap.md` item 10): not a gate.

### Console round 4

`env MX_TILE`: the colour tile's enable bit (bit 0; pbkit sets bit 1) is
**+11%, 36.2 -> 40.2 fps**; Z compression settings nothing. CPU reads at
pbkit's address see the tiled layout (`renderer.md` "Tile regions").

### Console round 5

CPU framebuffer access through the NV2A aperture (`nv2a.c` `fb_cpu`) sees
the untiled picture. `XGX_TILE` 4 became the default.

### Console round 6

Release build, 15 runs: 39.77 / 39.97 fps (sim 4.9, render 9.5-9.7, wait
3.4-3.5 ms a frame); FD 1v1 59.7 (sim 1.84 ms); 480p 45.5 tile on, 42.5
off. `MX_TRIM`, `MX_COPY_FIX=7` nothing. One 2-pixel speck at tick 500 in
one run: open.

### Console round 7 (v53, to run)

720p depth: Z24S8 (`MX_Z24` 1, v53's default) against Z16 (0), Fountain
twice each after a warm-up, Z24S8 without Z compression (`MX_TILE` 12),
Stadium both ways, FD 1v1, then Stadium lockstep shots (the arrowheads
whole with 1) and Fountain's against r6l. Expected: Z24S8 doubles the
depth bytes a fragment before compression, against a GPU busy at ~90% of
frame starts with fill ≥ 9-10 ms: 0 to -8% at 720p (0 if Z compression,
now flagged right for the format, takes most of it); FD should hold 60.
The result decides `XGX_Z24_16BPP`'s default (roadmap item 11).

## Status

Instructions are xemu `-icount`, draw / tick, Fountain; fps console.

| item | result | effect |
|---|---|---|
| step 0 tools, round 1 probe build | kept (test builds) | measurement |
| A1 `-fno-auto-import` | kept (0005392) | 363 `.refptr` -> 1; -0.4% / 0 |
| A2 function order (`order.txt`, `make_order.py`) | kept (083f575), free | 0 with LTO + PGO, +0.9% fps without (r3) |
| A3 ThinLTO (`XBOX_LTO=1`, `thinlto_link.py`) | kept, release default | -1.8% / -2.3%; +3.5% fps (r2) |
| A4 PGO (`XBOX_PGO`, `pgo.md`) | kept, release default (b557daa) | with A3 -9.9% / -16.6%; +11.6% fps (r2), +13% on dev (r3) |
| A5 `-Os` / cold `-Oz` | not tried | |
| B1 MXCSR flush-to-zero | not built | `FP_ASSIST` < 1%, window 1 not faster |
| B2 MEM1 on 4 MB pages | reverted | 0 (r2e, r3b, r3g) |
| B3 prefetch plans | reverted | noise; sim +2.4% (r2f) |
| B4 deferred back end | reverted | -6% fps, dlist +3 ms (r2d) |
| B5 dlist validation by events | not built | window 5: 0.5 ms (bar 1 ms) |
| C1 skip invisible work | nothing found | no inactive shadow maps in 4-CPU matches |
| C2 vertex-pool key over enabled formats | kept (5be8438) | duplicates 821 -> 0, pool free 4 KB -> 1.8 MB; dlist -11% |
| C3 GPU waits (`[NV2A]` busy/wait line) | kept as a probe | GPU busy all frame: no pipelining gain |
| C4 ARQ completion in line | kept (68a9a42) | sim -15.2% a tick |
| C5 envelope blends per frame | not done | validating a cache costs what the blend does |
| C6 material memo, C7 vertex-program uploads | not started | |
| D stage collision line rejects (`mplib.c`) | kept | 93% of line tests skipped |
| E audio mixer in SSE1 | kept | mixer instructions ~halved, same bits |
| `PObjSetupMtx` (no zeroing, SSE inverse) | kept | with D, E: sim -9.2%, render -7.7% (8f3584d) |
| GX front-end lookups (texture cache, cull planes) | kept (4b42a47) | render -2%, within noise (r6) |
| colour tile `XGX_TILE` 4 + `fb_cpu` | kept, default | +11% (r4), +7% at 480p (r6) |
| Z compression settings | removed | 0 (r4) |
| lockstep shots (`MX_LOCKSTEP`) | kept (29afd58) | tool |
| state trims (`MX_TRIM`) | reverted (56d5ab1) | 0; ~no draws to trim (r6) |
| merged clear (`MX_COPY_FIX=7`) | off, default stays 5 | 0 (r6); 4 min stress clean |
| F picture trades | not built | the user takes none |

## Lessons for future perf work

- Start each console round with an uncompared warm-up (first run ~4%
  slow). Noise is ~1.5%: repeat anything under ~3%.
- The console runs 0.3-0.5 instructions a cycle and xemu has no caches,
  so instruction counts misrank console costs. Compiler work (PGO) and
  skipping work paid; structural stall fixes (B2-B4) all failed.
- `fps = (1000 - 60 T) / (D c)` (T ms a tick, D draws, c ~31 µs a draw)
  matched the console to 1%: a ms per tick is worth 60 ms a second.
- Check whether the GPU sets the pace (`[NV2A]` busy at frame start) before
  chasing CPU time; after PGO, CPU savings turned into GPU wait.
- Ablation windows price whole passes in one boot; "no fill" (1-pixel
  scissor) found the GPU's cost.
- Read hardware docs (envytools) against what pbkit sets: one enable bit
  was +11%.
- Hardware `[SIMH]` parts by itself; xemu `-icount` `[SIMH]` is the gate,
  and a mismatch gets a rerun first (a ThinLTO `fodperf` run parted once).
- Retrain PGO after game or sdk changes (`pgo.md`).
- Free RAM in a match: 6.8 MB at 720p, 7-9 MB at 480. Fountain: ~730-1025
  draws a frame (main 71%, shadow maps 13%, reflection 16%).
