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
  - [x] textures: CMPR -> DXT1, I/IA -> AY8/A8Y8, RGB565 native, C4/C8 -> I8 +
        palette, the rest A8R8G8B8 (NPOT resampled), EFB copies (drawn by the GPU)
  - [x] 720p 16:9 content rect for melee-pc's hor+ widescreen
- [x] default.xbe links
- [x] boots to the title screen in xemu ("TechProGabe Presents..." card first)
- [x] attract-demo VS matches run in xemu (slowly: about 5 fps there)
- [x] memory fit, first pass: native texture formats; MEM1 and ARAM committed on demand
- [x] memory fit measured on hardware (boot.log `[MEM]`); disc-backed ARAM pages
- [x] 4-CPU VS matches stable on hardware (Pokémon Stadium with transformations,
      Fountain of Dreams, Green Greens), audio, saves, 100% save file loads
- [x] fog: every GX fog type, per vertex from GX's registers (`nv2a_fog.c`); not yet
      compared with Dolphin on the console; no range adjustment
- [ ] indirect texturing, TEV swap tables beyond alpha broadcast
- [ ] VS mode with 4 players on hardware at 60 fps
- [x] movie frames decoded (`thp.c`)
- [x] dashboard icon (`$$XTIMAGE` + `default.tbn`, own title ID 4D580001)
- [x] console screenshots (BACK -> `shotNN.bmp`) and a cache-flush diagnostic (BACK+Y)
- [ ] release packaging

## Where it stands (2026-09-30, build v16)

On the console (`-DXHW_PROF=1` builds, `[PERF]` lines): menus and
character select run at 60 fps. A 4-CPU match keeps the simulation at 60
ticks a second and draws 25-30 fps on Pokémon Stadium and 13-15 fps on
Fountain of Dreams (its reflections: more EFB copies and draws); Stadium
started this round at 4 fps with a freeze. Two-round sessions end stable.

Fixed on the console this round (details in `renderer.md`,
`platform.md` and `decisions.md`):

- pushbuffer overrun: pbkit silently kept 512 KB for a non-power-of-two
  size, frames ran past it (GPU fault, freeze when Stadium's screen came on);
- depth: pbkit re-enabled the w-buffer every frame, and with the z-buffer
  out-of-range depth was culled (black bands over Stadium's floor); now a
  z-buffer with depth clamped as on the GameCube;
- `wait_idle` counted methods still in PFIFO's CACHE1 as done;
- texture pool pressure: EFB copies at the nearest power of two, C4/C8 as
  palette textures, P8 palettes re-sent for every new texture;
- AC97 bus master halted for a whole boot (silent): halt/stall recovery;
- imported-code bugs: Stadium transformations (bad kind, double parse and
  hang), Kirby copy abilities with parts (crash on knockback), the online
  lobby on a build without netplay (crash);
- the dashboard showed another nxdk title's icon (shared title ID).

## Next

1. **Texture accuracy.** Fountain of Dreams' water and other reflections
   need indirect texturing (the NV2A's texture-shader bump modes may do
   it); TEV swap tables beyond the alpha broadcast. Fog is new: compare
   stage backgrounds, the title screen and the Classic/All-Star intros
   with Dolphin (`docs/renderer.md` lists where Melee uses it). Item crates
   still show the background through their dark gaps. Short black flashes
   remain on Stadium's floor. For each, take a console screenshot (BACK)
   and compare with xemu at the same scene (`docs/testing.md`).
2. **Texture pool pressure** on Stadium: 10-65 failed allocations and
   ~500 uploads per 10 s. Non-power-of-two C8 textures still expand to
   A8R8G8B8 at the next power of two (1.5 of Stadium's 6 MB). Candidates:
   NV2A linear (rect) textures at their real size (texcoords scaled in the
   vertex program; GX allows only clamp for NPOT), P8 for NPOT by nearest
   resampling, or a larger pool now that disc-backed ARAM frees RAM.
3. **One `[NV2A] GPU stalled` in menus** (v12, recovered on its own).
4. **Performance**, as planned below.

## Performance plan

Where it stood in xemu (4-CPU timed match on Green Greens):

| | hardware (v6, 2026-09-29) | xemu 2026-09-30 start | xemu now |
|---|---|---|---|
| match fps | ~14 | 4.0 | ~13.5 |
| results screen fps | | 2.0 | ~9 |
| draws per frame | ~2750 | ~2950 | ~730 |
| sim ticks per second | ~60 | ~20 | ~60 |

In order, measured against the console's `[PERF]`/`[PROF]` lines
(`prof_report.py`):

1. **Fewer draws** (both targets; xemu is bound by them): ~27% of draws
   change no state and ~13% only a position matrix (`[NV2A] per N draws`).
   Candidates: join consecutive cached lists with identical state into one
   draw (they sit in different buffers today, so the fixed-base scheme would
   need them adjacent, or a copy to the ring); for position-matrix-only
   changes, give each draw its own matrix slot and index it per vertex.
2. **Simulation CPU**: stage collision (`mpLib_*Wall`, `mpCheck*`; ~15% of
   the CPU work in xemu), musl `sinf`/`cosf` (double math on x87), HSD
   animation (`fobj`, `jobj`). Only bit-identical rewrites (the simulation
   must round like other builds), as `C_MTXConcat` was.
3. **Render-pass CPU**: `PObjSetupMtx` and envelope skinning matrices,
   GX setter traffic per material.
4. **CPU/GPU overlap** at present: the frame waits for the GPU to go idle,
   so the GPU sits idle during the next frame's simulation. Worth ~20% in
   xemu; on hardware the GPU wait was under 1 ms, so only once the CPU side
   is fast.
5. Smaller: stagger the display-list content check (a sampled hash per list
   per frame), `-ftrivial-auto-var-init-max-size` for large locals in hot
   code (after checking which rely on the zeroing).

## Future features

- **Front LED effects**: the SMC takes a custom four-step red/green pattern
  over SMBus (`HalWriteSMBusValue(0x20, 0x08, 0, pattern)` then register
  0x07 = 1; register 0x07 = 0 hands the LED back to the SMC). Ideas: a
  flash when a player loses a stock (colour per port), the timer's last
  seconds, a pulse on Game! Keep it an option in `settings.ini`, and send
  only on events: each write is an SMBus transaction.

- **Netplay** (LAN and online): melee-pc's netplay, LAN discovery and
  lobby are not built (`stubs.c` reports them off, and the lobby scene
  returns to the menu). Needs nxdk's network stack (lwIP) under
  melee-pc's `pc_net_*`/`pc_lan_*` layer, rollback's memory snapshots
  within the Xbox's 64 MB, and the simulation's timing at 60 ticks on the
  console.
