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

## Where it stands (2026-09-30, build v25, tested)

On the console (`-DXHW_PROF=1` builds, `[PERF]` lines): menus and
character select run at 60 fps. A 4-CPU match keeps the simulation at 60
ticks a second and draws ~28 fps on Pokémon Stadium and ~23-28 fps on
Fountain of Dreams (15 before its 104 KB stage list could be cached:
`DLC_MAX_BATCH` was 64, now 4096 with scratch that grows). Fog, shorter
vertex programs with a forecast residency policy, and big-endian memory
card files (GameCube/Dolphin saves load; all characters unlocked with the
user's 100% save) are in.

Found and fixed on the console in v17-v25 (details in `renderer.md`,
`platform.md`, `decisions.md`):

- Fountain of Dreams decoded its stage list every frame (more than 64
  draws in one list): ~25% of the CPU;
- texture and display-list content hashes: four FNV chains (same words);
- nxdk's pdclib printf has no `%f`: a `%f` followed by `%s` crashed (my
  own log did, in sudden death); `xsdk_vsnprintf` formats floats;
- black flashes on surfaces near fighters (Fountain's corner and
  platforms, Corneria's nameplate, Stadium's floor): the four shadow maps
  are drawn in one corner, copied and cleared one after another, and the
  next map's white background quad lost its first triangle to the clear
  (BACK on a `-DXGX_DEBUG_TRACE` build writes the EFB copies: black wedges
  above the quad's diagonal). v23 waits for idle after every clear and
  after the copy's retarget; **v24 still showed black on the console**:
  check the v25 BACK dumps (`[DRAW] efb copy` lines) before trying more.
- trophy gallery: the save's trophy count was byte-swapped (see the memory
  card PR); the user's console save had been rewritten by older builds and
  was replaced with the Dolphin original (`~/Downloads/...35037.gci`).

## Next

1. **Corneria: every fighter is KO'd at the start.** v25 found it: the
   Great Fox gun (item 0xA0, stage-owned, fire, `grcorneria.c`
   `left_cannon`/`right_cannon`, made by `grMaterial_801C8CFC`) has
   `x5D4_hitboxes[0].hit.scale` = garbage (`scale x100 -2147483648`: NaN,
   infinite or huge), while its damage (30) and position (-108,240) are
   right. A hitbox that big hits all four fighters wherever they are. Find
   where the stage item's hitbox scale is loaded (the stage item's
   attributes or hitbox descriptor from disc, likely a field read without
   the big-endian conversion or a wrong struct offset) and fix it.
2. **Black shadow-map wedges, not fixed by WAIT_FOR_IDLE** (v25 dumps: 25
   of 33 maps lost the background quad's first triangle). The first maps of
   a session were clean; the wedges start once the stage's other EFB copy
   (an 80x60 scene copy to 64x64, filtered, `efb_copy_gpu`) has run, so
   suspect GPU state that copy leaves behind (texture unit 0's linear/rect
   setup, filter, window clip, surface or combiner state) rather than a
   race. Try in xemu with an autopad `BACK` late in a Fountain match (the
   dumps work there too); then re-send or reset that state after
   `efb_copy_gpu`.
3. **Kirby's copy hats**: `ftParts: kind 4 costume 0: visibility table 3
   points at 0` (also Samus, Mewtwo); Kirby with Pikachu's ability showed
   black shapes on the hat. The guard skips the table; find why the table
   pointer is 0 (`ftKb_LoadHatParts`, `ftParts_8007487C`).
4. **Trophy Collection runs at ~1.4 fps** (`tex 649 ms` a frame: every
   trophy at once overflows the texture pool and textures re-upload every
   frame). The gallery and collection work with the restored save.
5. **Intro movie (THP) plays choppily** on the console.
6. **Texture accuracy**: indirect texturing (Fountain's water), TEV swap
   tables, item crates, fog checks against Dolphin.
7. **Texture pool pressure** on Stadium (NPOT C8 as ARGB8).
8. **Performance**, as planned below: on the console the simulation is
   ~4 ms a tick, display lists 4-5 ms and draw submission 6-9 ms a frame;
   ~50 vertex-program loads a frame remain.

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
5. **Vertex-program loads** (done: `docs/renderer.md` "Program memory").
   Next: capture a console trace (`-DXGX_DEBUG_VPTRACE`) and replay it with
   `vp_policy.py`; if loads still matter, let a program that writes more
   outputs serve draws that don't read them (fog off, fewer texture units,
   COLOR1 unused), at some cost in vertex work.
6. Smaller: stagger the display-list content check (a sampled hash per list
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
