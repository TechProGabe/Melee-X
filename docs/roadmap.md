# Roadmap

- [x] Toolchain: nxdk + LLVM 21, `disc_lower` for DISC_STRUCT, game triple chosen and
      ABI-checked against nxdk (`docs/toolchain.md`)
- [x] All 1008 game translation units compile to i386 COFF
- [x] Platform layer, first pass
  - [x] OS: MEM1 at a fixed VA, SDK heaps, alarms, time, interrupts (recursive lock)
  - [x] DVD: FST over the user's .iso/.gcm/.ciso, async reads on a worker
  - [x] AR/ARQ: ARAM buffer
  - [x] PAD: four ports by physical port, GC-like layout, dead zones, rumble, settings.ini
  - [x] VI: 60.000 Hz pacing; 720p / 480p / 480i chosen from the dashboard
  - [x] AX/AI: melee-pc's mixer -> AC97 (OpenCrossing's polled driver; APU voice in xemu)
  - [x] CARD: slot A as .gci files in `E:\UDATA\4d580001\card_a\`
  - [x] MTX: aurora's C implementations
- [x] GX -> NV2A, first pass
  - [x] state, immediate mode, display lists (big-endian, indexed arrays)
  - [x] generated vertex programs: a0-indexed skinning, GX lighting (spot, distance,
        specular), texgen; encoder checked bit for bit against nv2a-vsh
  - [x] TEV -> register combiners (from OpenCrossing's compiler, 8 stages, 4 units)
  - [x] textures: CMPR -> DXT1, I/IA -> AY8/A8Y8, RGB565 native, the rest A8R8G8B8
        (NPOT resampled), TLUTs, EFB copies (drawn by the GPU)
  - [x] 720p 16:9 content rect for melee-pc's hor+ widescreen
- [x] default.xbe links
- [x] boots to the title screen in xemu ("TechProGabe Presents..." card first)
- [x] attract-demo VS matches run in xemu (slowly: about 5 fps there)
- [x] memory fit, first pass: native texture formats; MEM1 and ARAM committed on demand
- [ ] memory fit measured on hardware (boot.log `[MEM]`); disc-backed ARAM if needed
- [ ] fog, indirect texturing, TEV swap tables beyond alpha broadcast
- [ ] VS mode with 4 players on hardware at 60 fps
- [x] movie frames decoded (`thp.c`)
- [x] dashboard icon (`$$XTIMAGE` + `default.tbn`)
- [ ] release packaging

## Performance

Where it stands (4-CPU timed match on Green Greens):

| | hardware (v6, 2026-09-29) | xemu 2026-09-30 start | xemu now |
|---|---|---|---|
| match fps | ~14 | 4.0 | ~13.5 |
| results screen fps | | 2.0 | ~9 |
| draws per frame | ~2750 | ~2950 | ~730 |
| sim ticks per second | ~60 | ~20 | ~60 |

Done in this round (details in `renderer.md` and `decisions.md`): a
display list's batches drawn as one, immediate-mode batches continued while
the state holds, quads and fans as triangles, fixed array bases, the list
cache keyed by vertex formats (no volatile lists left), `[PERF]` split into
`sim` and `render` with ticks per render, SSE `MTXConcat`, inline `sqrt`,
`memzero` via `memset`, alarm delivery gated, back-end bookkeeping trimmed.
None of it has been measured on the console yet.

Plan for the next run, in order:

1. **Measure on hardware** when a console is available: deploy an
   `-DXHW_PROF=1` build with `default.tbn` (built from `8a175c4` as
   `~/xemu/hw/default.v9.xbe`, map `melee_x.v9.map`), and
   play a 4-CPU match for a minute. `[PERF]` now says how the frame splits
   into `sim` (x ticks per render) and `render`; `[PROF]` (192 buckets) with
   `prof_report.py` says what to cut. That decides steps 3-4.
2. **Fewer draws** (both targets; xemu is bound by them): ~27% of draws
   change no state and ~13% only a position matrix (`[NV2A] per N draws`).
   Candidates: join consecutive cached lists with identical state into one
   draw (they sit in different buffers today, so the fixed-base scheme would
   need them adjacent, or a copy to the ring); for position-matrix-only
   changes, give each draw its own matrix slot and index it per vertex.
3. **Simulation CPU**: stage collision (`mpLib_*Wall`, `mpCheck*`; ~15% of
   the CPU work in xemu), musl `sinf`/`cosf` (double math on x87), HSD
   animation (`fobj`, `jobj`). Only bit-identical rewrites (the simulation
   must round like other builds), as `C_MTXConcat` was.
4. **Render-pass CPU**: `PObjSetupMtx` and envelope skinning matrices,
   GX setter traffic per material.
5. **CPU/GPU overlap** at present: the frame waits for the GPU to go idle,
   so the GPU sits idle during the next frame's simulation. Worth ~20% in
   xemu; on hardware the GPU wait was under 1 ms, so only once the CPU side
   is fast.
6. Smaller: stagger the display-list content check (a sampled hash per list
   per frame), `-ftrivial-auto-var-init-max-size` for large locals in hot
   code (after checking which rely on the zeroing).
