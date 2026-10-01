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

v32 on the console (2026-10-01, `~/xemu/hw/logs32`, map `melee_x.v32.map`,
commit b55012d, `-DXHW_PROF=1`): **sound fixed** (boot.log: one `AC97
polled` line, no halt, stuck or cold reset); the Data -> title hang could
not be reproduced and no SFX overflow was logged; stable, no texture
problems. Fountain of Dreams, the open performance item: 75 five-second
`[PERF]` windows of a match average 21.6 fps (18-24), sim 12.3 ms a frame
at 2.7 ticks per render (~4.5 ms a tick), render 17-19 ms (draw 8.3,
dlist 4.6), ~885 draws and 79k verts a frame; the user saw 15 fps
sustained in busy moments and 9 fps when a fighter flew off stage (the
camera pulls back; five-second windows hide such dips). The profile is
flat: game-side HSD animation/matrix/material setup about half, our
`xgx_draw` + `emit_vc` 7%, texture bind/lookup 7% (by caller), the cull's
content recheck (`sample_hash`, `dlc_content_changed`) ~5%. A slow frame
runs more sim ticks before the next render, so dips feed themselves.
The CPUs were on high level and fighting hard, which explains sim at
~4.5 ms a tick against ~3.5 ms in v31's matches: the v32 numbers are a
heavier scene, not a regression.
Ideas, in order: cheaper cull recheck (hash a sample less often or only
lists that were culled last frame), texture bind fast path, cap sim ticks
per render on a slow frame, then the HSD setup paths. The FPS counter's
options-menu toggle is still to do.

v32 round: fixes for the two v31 console problems, below.
Check audio first on the console (`[AUDIO]` lines; rule: every console
build gets an audio review of what changed since the last good boot).

- **No sound for the whole boot** (v31, v27 before it): `AC97 stuck: civ 0
  lvi 6 sr 00/00`, restarts and 31 cold resets never recovered. The run bit
  could be set while descriptor 0 was still empty (boot raced the pump
  thread; a restart only toggled the run bit; a cold reset zeroed the
  descriptors and ran at once). Now every start resets the bus masters,
  queues seven buffers, then runs (`aci_start`, `docs/platform.md`). In xemu
  with `-DXHW_AUDIO_APU=0` the AC97 started once with no `halted` restart
  (every earlier console boot logged one).
- **Hang going from the Data menu back to the title** (hang.log: the title's
  `lbAudioAx_80027648` waiting on `HSD_SynthSFXWaitForLoadCompletion`):
  `Can't load SFX file; bank(id=2) buffer overflow`, then its callback
  queued the same SSM again, overflowing again on the DVD thread forever.
  The v30 reload guard therefore never ran (the v29 Jungle Japes hang was
  the same loop). Overflows now drop the load without the callback, the
  guard reloads bank 2 from empty, and the report gives the bank's use
  (look for it, and for `[WARN] SFX bank 2 load failed`, in v32's log). The
  sound test also waits for in-flight loads before emptying bank 2. Why the
  bank's real fill outgrew the game's accounting is still open; the
  per-SSM size table is never smaller than the US files (checked against
  the disc), so it isn't that.

v31 on the console (2026-10-01, `~/xemu/hw/logs31`, map `melee_x.v31.map`):
Kirby's Falcon helmet, capsules and crates fixed; Mute City 30-40 fps (was
the slowest stage), Onett 40-50, Fountain of Dreams looks right but dips to
~15 fps at moments (to look at); no crashes over many matches; "with audio
fixed, almost a release candidate". To do later: the FPS counter's
options-menu toggle (now `settings.ini` `[video] fps`).

v31 round: fixed in xemu: Kirby's grey Falcon helmet during
Falcon Punch (`HSD_TExpSetReg` konst halves), black capsules (same), crate
fronts (bump texgen + emboss pair), CMPR transparent texels (DXT3);
off-screen rigid DObjs culled (Mute City 5.6 -> 15.5 fps in xemu). To do
before the v31 hardware build: an on-screen FPS counter, toggled in
`settings.ini` (done: `[video] fps`, default on; an options-menu toggle
later). Fountain of Dreams: a 4-CPU frame in xemu has no indirect stages at
all (`nind` 0 in every draw), so the "blocky reflection" needs a console
BACK shot of it before anything is changed.

v29 on the console (2026-10-01, `~/xemu/hw/logs29`, map `melee_x.v29.map`):
a 5-minute Pokémon Stadium run was stable (no GPU stall). Jungle Japes hung
on entry after four matches: `Can't load SFX file; bank(id=2) buffer
overflow`, then `onEnterVs` -> `lbAudioAx_80027648` waited forever for the
SSM (v30's reload guard never ran: see v32 above). Shots: shot20 = Kirby's
grey Falcon helmet (trace section 3 of `trace.log`), shot61 = the capsule
drawn near black (section 9; draws #372/#501: texgen NRM x TEXMTX0,
normalized, post PTTEXMTX0 -> env map 64x64 DXT1, TEV konst x ras x tex),
shot66/shot81 = crates with dark, black-striped faces. The Falcon helmet
is an ordinary hat (`ftKb_LoadHat`): the parts hats are DK, Jigglypuff,
Mewtwo, Falco and G&W (our `hats[k]` is upstream's `copies[k + 1]`), so
the costume matanim idea doesn't apply. Mute City runs at a lower frame
rate than the other stages.

v28 on the console (2026-10-01, logs in `~/xemu/hw/logs28`, map
`melee_x.v28.map`): intro movie perfect, audio back (no cold reset was
needed: the AC97 started after its usual first restart), frame rate "great"
(4-player Fountain ~30 fps: render 14.5 ms, sim 3.8 ms a tick x 2, draw 6.5,
dlist 2.6; menus 55-60), no stage texture problems, Corneria fixed, rumble
strength right, Trophy Collection smooth (`[TEX] overflow pool 6144 KB`,
released on leaving). Open, in order:

1. **GPU hang on Pokémon Stadium** (~500 s into the session, VS match):
   the game thread spins in `xgx_present` -> `wait_idle` -> `pb_busy`, and
   the retrace stopped too (`[WDOG] frames stopped`, hang.log). No `[NV2A]
   GPU stalled` line survived: boot.log had hit its 2 MB cap at ~145 s
   (the `-DXGX_DEBUG_TRACE` `[DRAW]` dumps fill it). v27 had the same kind
   of stop once (PGRAPH error source 0x20, LIMIT_ZETA, then `pgraph
   18000001`). Neither appears in any log before v26, so suspect what v26
   changed on the GPU side: `BREAK_VERTEX_BUFFER_CACHE` at every batch
   start, vertex-pool buffers freed at once on eviction
   (`xgx_vbuf_free_now`), kicks every 32 KB. One theory: a draw reading
   garbage vertices (huge or NaN positions) makes the rasterizer run past
   the depth surface (LIMIT_ZETA). Plan: (a) keep boot.log usable: send
   `[DRAW]` trace lines to a separate trace.log, raise or ring-buffer the
   boot.log cap; (b) build switches to undo each v26 GPU change
   (`-DXGX_VB_CACHE_BREAK=0`, `-DXGX_VBUF_FREE_NOW=0`, `-DXGX_PB_KICK=4096`)
   so the console can bisect; (c) a debug check that every position in a
   built display list is finite; (d) on a stall, log PGRAPH's trapped
   method/data (v28 added) plus the surface and clip registers. Ship the
   next test build without `-DXGX_DEBUG_TRACE` unless BACK dumps are needed.
   **Done for v29** (one bundled console round): (a) `[DRAW]` lines go to
   `trace.log` (64 MB, restarted), `boot.log` keeps 4 MB and continues in
   `boot2.log`/`boot3.log` (2 MB each, alternating); (b)
   `-DXGX_PB_KICK`, `-DXGX_VB_CACHE_BREAK=0`, `-DXGX_VBUF_FREE_NOW=0`
   (not used in v29: bisect only if the stall comes back); (c)
   `-DXGX_CHECK_VERTS` (`[WARN] dlist`); (d) the stall report adds PGRAPH
   intr/nsource/trap/surface/clear/window-clip/raster. v29 is built with
   `-DXHW_PROF=1 -DXGX_DEBUG_TRACE -DXGX_CHECK_VERTS`, so BACK on the grey
   Kirby hat and the capsule gives their `[DRAW]` traces in the same round.
   Checked meanwhile: `xgx_vbuf_free_now` only frees lists not drawn since
   the last `xgx_present` (which waits for idle), and the EFB copy's zeta
   extent (pitch pw*4 x ph <= 1 MB) stays inside the 640x480x4 depth
   surface, so neither explains LIMIT_ZETA on its own.
2. **Kirby's copy hats**: right most of the time, but the hat's texture
   sometimes disappears. logs28 `shot31.bmp` (Fountain, ~1:19 left): the
   blue Kirby's Captain Falcon helmet is flat grey-white (untextured look)
   while Falcon's own helmet beside it is red and yellow. `shot63.bmp`
   (Stadium, 1:53): the yellow Kirby's Falcon helmet is grey too, while the
   blue Kirby's Pikachu hat is right. So it looks consistent for Falcon's
   hat (a parts hat), not random. Neither `[DRAW]` trace survived (boot.log
   hit the cap at shot30). Parts hats go through `ftKb_LoadHatParts`, whose
   part visibility and costume texture list (`u.kb.x44`,
   `ftAnim_80070200`, with Kirby's `costume_id`) were added under PORT in
   v19 (b7e3ec2): compare with upstream doldecomp, check the hat's texture
   list for Kirby's costume, and test the other parts hats (Ganondorf,
   Yoshi, Jigglypuff, Dr. Mario). Reproduce in xemu if a debug VS can give
   Kirby Falcon's ability. Checked against the DOL (0x800F0FC0): the game
   passes the hat itself as the FtPartsDesc, hat+8 to ftAnim_80070200 and
   Kirby's costume_id, exactly as the PORT code does, so the data setup is
   right; both grey hats were on non-default Kirby costumes (blue, yellow).
   Other suspects: the texture-revalidation stagger (a hat
   texture loaded where another one lived, same pointer/size/format, served
   stale for up to 3 frames: try `TEX_STABLE` off), the hat's costume
   texture list (`u.kb.x44`, `ftAnim_80070200`, TObj image switching), or a
   dropped texture (`[TEX] drop`). A BACK trace with the hat missing shows
   the hat's draws and whether their texture is bound (`tex 0x0`).
3. **Capsule item ("pill canister") texture looks off**: get a BACK shot
   and its `[DRAW]` lines; check its TEV setup (swap tables, approximated
   stages), texture format and the new linear (non-power-of-two) path.
4. **Fountain of Dreams' reflection** is blocky: indirect texturing isn't
   implemented (the 80x60 -> 64x64 scene copy is drawn straight).
5. **Texture accuracy**: indirect texturing, TEV swap tables, item crates,
   fog checks against Dolphin.
6. **Performance**: 4-player matches ~30 fps on the console; per frame the
   render pass (HSD scene walk, GX setters) is ~14.5 ms, the simulation
   ~3.8 ms a tick (2 ticks a render at 30 fps), draw submission ~6.5 ms,
   display lists ~2.6 ms. Plan below.

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
   must round like other builds), as `C_MTXConcat` was. First pass done
   (after v26, unmeasured on the console; `docs/decisions.md`, "HSD
   animation and matrices"): the spline's divide and call, a shared
   `sinf`/`cosf` per angle in `HSD_MtxSRT`, no calls for objects with
   nothing playing. Estimated at a few tenths of a ms per frame; compare
   the `[PROF]` share of `fobj`, `HSD_MtxSRT`, `pc_sinf`/`pc_cosf` and
   `HSD_AObjInterpretAnim` with v25. What is left is the arithmetic itself:
   the next step would be memoizing `sinf`/`cosf` per joint angle (pure
   functions, so exact), if a console count shows angles repeat between
   ticks.
3. **Render-pass CPU**: `PObjSetupMtx` and envelope skinning matrices,
   GX setter traffic per material. First pass done (after v26): the envelope
   blend's concat and scaled add fused into one SSE step.
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

- **Settings menu**: an in-game screen for what only `settings.ini` sets
  today (`docs/platform.md`): video mode (720p / 480p / 480i, widescreen),
  rumble, per-port button mapping and dead zones, plus port toggles worth
  exposing (screenshot button, `[PERF]` overlay, texture pool size, the
  performance trade-offs as they appear). Options: a page in Melee's own
  Options menu (imported menu code, `PORT:` edits), or a separate Melee-X
  screen before the title (held button at boot, drawn with the splash
  code). Writes `settings.ini`; a video-mode change needs a restart (the
  NV2A and pools are sized at boot).

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
