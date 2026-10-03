# Roadmap

- [x] Toolchain: nxdk + LLVM 21, `disc_lower` for DISC_STRUCT, game triple chosen and
      ABI-checked against nxdk (`docs/toolchain.md`); native Windows build (MSYS2)
- [x] All 1008 game translation units compile to i386 COFF
- [x] Platform layer
  - [x] OS: MEM1 at a fixed VA, SDK heaps, alarms, time, interrupts (recursive lock)
  - [x] DVD: FST over the user's .iso/.gcm/.ciso, async reads on a worker
  - [x] AR/ARQ: ARAM buffer, disc-backed pages
  - [x] PAD: four ports by physical port, GC-like layout, dead zones, rumble, settings.ini
  - [x] VI: 60.000 Hz pacing; 720p / 480p / 480i chosen from the dashboard
  - [x] AX/AI: melee-pc's mixer -> AC97 (OpenCrossing's polled driver; APU voice in xemu)
  - [x] CARD: slot A as big-endian .gci files in `E:\UDATA\4d580001\card_a\`
  - [x] MTX: aurora's C implementations (`C_MTXConcat` in SSE)
- [x] GX -> NV2A
  - [x] state, immediate mode, display lists (big-endian, indexed arrays), cached
  - [x] generated vertex programs: a0-indexed skinning, GX lighting (spot, distance,
        specular), texgen; encoder checked bit for bit against nv2a-vsh
  - [x] TEV -> register combiners (from OpenCrossing's compiler, 8 stages, 4 units)
  - [x] textures: CMPR -> DXT1/DXT3, I/IA -> AY8/A8Y8, RGB565 native, C4/C8 -> I8 +
        palette, the rest A8R8G8B8, NPOT as linear textures, EFB copies on the GPU
  - [x] 720p 16:9 content rect for melee-pc's hor+ widescreen
  - [x] fog: every GX fog type, per vertex from GX's registers (`nv2a_fog.c`); not yet
        compared with Dolphin on the console; no range adjustment
  - [x] TEV swap tables (broadcasts as dot-product combiner stages; permutations
        approximated)
  - [x] indirect texturing (the cloak's refraction) as BUMPENVMAP units
- [x] movie frames decoded (`thp.c`)
- [x] memory fit on hardware: native texture formats, MEM1 and ARAM committed on demand
- [x] 4-CPU VS matches stable on hardware, audio, saves, the 100% save loads
- [x] dashboard icon (`$$XTIMAGE` + `default.tbn`, own title ID 4D580001)
- [x] console screenshots (BACK -> `shotNN.bmp`, test builds) and a cache-flush
      diagnostic (BACK+Y)
- [x] release packaging (`package_release.py`, `tools/make-xiso`), release builds
      without the test tools
- [x] settings menu: BACK on the title screen opens a panel for `settings.ini`
      (video output, widescreen, FPS counter, 128 MB, rumble, per-port dead zones
      and trigger click). Counter and rumble apply live; video and RAM are saved
      for a restart, which the menu offers (`docs/platform.md`). Button mapping
      is still `settings.ini` only.
- [x] front LED effects: KOs in the port's colour, the timer's last seconds,
      a sweep on GAME!, a last-stock tick; `[system] led` and a menu row, on by
      default, written by a worker on events only (`docs/platform.md` "Front
      LED"). Needs a look on the console: xemu doesn't show the LED
- [ ] VS mode with 4 players on hardware at 60 fps
- [ ] first public release (v36 is the candidate)

## Where it stands (2026-10-01, v36)

Fully playable on the console. Menus run at 60 fps; matches run 30-60
fps depending on the stage and how busy it is, and the simulation keeps
60 ticks a second (Melee runs extra ticks before a slow frame's render, so
the game never slows down).

The v36 playtest (~30 minutes, `C:\xemu\hw\logs36`; only the last boot's
14 minutes survived, see `docs/handoff.md`):

- No crash or hang, exactly one `[AUDIO] AC97 polled` line.
- A 10-minute match: 30.0-53.9 fps over 129 five-second windows, 36.7 on
  average, never below 30. Per frame ~8 ms of simulation (1.6 ticks) and
  ~12 ms of render pass (draw 5.3, dlist 1.5), ~600 draws and ~45k
  vertices. The GPU waits are ~0: the frame is CPU-bound.
- The session's first match dipped to 24-30 fps for its first ~20 s while
  ~400 textures uploaded per 10 s and the texture pool ran down to 79 KB
  free.
- Memory in a match: ~7-9 MB of RAM free with ~24 MB of MEM1+ARAM
  committed; steady across five matches.
- The texture pool (8 MB) runs nearly full from the second match on (0.3-3
  MB free). First tries that fail are retried after evicting
  (`pool allocations failed`, up to 15 per 10 s); no texture was dropped.
- The display-list cache is at capacity in a long session: all 2048 slots
  in use and the 4 MB vertex pool down to 4-60 KB free, with 36-550
  rebuilds per 10 s from LRU evictions (no failures). A bigger pool or more
  slots would cut rebuilds, if RAM allows.
- Pushbuffer peak 630 of 1024 KB, no restarts. 0-19 approximated draws
  per frame (TEV setups the combiners can't do exactly).
- `[CARD] save data looks mixed (be 930, le 161)`: the console's copy of
  the 100% save has fields older builds wrote little-endian (`docs/platform.md`).
- `[WARN] hit` lines: the knockback diagnostic from the Corneria bug still
  logs its first 32 hits a boot (`docs/decisions.md`); harmless.

## Known issues (after v1, GitHub #5/#6)

Fix order after RC2 (2026-10-02), details in the entries below:

1. Memory leak since v39: add a per-subsystem memory breakdown to the
   log each minute, run 15-20 min in xemu, find what grows.
2. Long-uptime whole-system freeze: check whether it follows the leak
   (free memory at the freeze, v39 had no leak and froze at ~82 min).
3. Pokémon Stadium flicker on the fight-camera switch: keep EFB copies
   past the idle release without the hash test (the first try,
   `~/xemu/tools/stadium-copy-lifetime.patch`, broke Stage Clear);
   check `scenarios/clear`, `ps2` (>40 s) and the texture pool.
4. Peach's Castle Bullet Bill stuck + endless quake.
5. Particles not drawn (all of them, not only the Fire Flower's); Adventure Corneria Arwing cutscene
   silent with Falco's face frozen; trophy transition lighting.
6. 100-Man freeze: the post-copy GPU stall, fixed on dev; confirm with
   the tester on the next RC.
7. Classic Team Kirby card: some Kirbys corrupted (tester).

- Fixed on dev: the v1 release hung on the intro movie (GitHub #5, #6,
  reddit), at any video mode. `xgx_present` called pbkit's `pb_finished`
  with the frame's tail still open, so the flip overwrote unsent commands
  and the GPU then read from the middle of a vertex-program upload. The
  frame-rate counter (on in test builds, off in a release) closed the
  tail first, which is why no test build ever hung. Found with the
  first-fault pushbuffer dump on the console (v37), confirmed in xemu with
  a release build.
- A mid-match GPU stall, three times, all 480i: Classic (issue #5, v1,
  128 MB, `LIMIT_COLOR` on a game `DRAW_ARRAYS`), a VS match on Fountain of
  Dreams (v2, 128 MB) and the v2 burn-in on the user's 64 MB console (frame
  72850, ~20 min), the last two `LIMIT_COLOR` on the `END` of the EFB
  copy's quad into a swizzled 256x256 target. The GPU stops after the
  fault, so it is not a 128 MB problem (`ram128` stays as a precaution).
  v40's candidate fix (`renderer.md`): the window clip's maximum is
  inclusive, so the copy's `pw << 16` let column and row 256 through, past
  the end of the swizzled target; the copy and the Z-texture mask now clip
  to `pw - 1`, `ph - 1` and break the vertex cache right before their
  draws, and the game's scissor ends one pixel earlier, as GX's does. If it
  comes back, v39+ log PGRAPH 0x400800-0x40080C and the last copy's target
  at the first fault.
- Fixed: Classic's team cards (Team DK/Kirby/Jigglypuff, stage 8) drew the
  right half black. Two bugs: the depth plane was never primed
  (`GXTexCoord1f32` positions dropped) and `GXSetZTexture` wasn't emulated
  (now a mask, `docs/renderer.md`). Reproduce with
  `MELEE_BOOT_SCENE=classic`, `MELEE_CLASSIC_STAGE_OVERRIDE=8`,
  `MELEE_CLASSIC_TEAM=dk`.
- Fixed: star KOs (knocked off the top) could end at once instead of the
  fighter flying into the background (`ft_0D31.c`, `docs/decisions.md`).
- Fountain of Dreams runs ~40 fps on the console in a 1v1 (v2 report:
  ~55k vertices and ~540 draws a frame, render 11-12 ms). Frame rate work
  is in "Next".
- Fox costs more than other characters: four Foxes on Fountain of Dreams
  run ~10 fps lower than a mixed 4-player match (console, v43-era build).
  xemu, 4x Fox vs 4x Mario on Fountain: ~980 vs ~710 draws a frame (+38%)
  for about the same vertices (74k vs 70k); the simulation per tick is no
  dearer (~1.8 vs ~2.1 ms), so it is render-side, per draw. Fox's model has
  about twice Mario's PObjs (shadow pass 34 vs 17 draws a fighter,
  reflection 33 vs 16), drawn in four passes on Fountain (two reflection
  passes, the shadow map, the main pass), and switches material more often
  (2.8x the vertex-program switches, 2.7x the TEV/channel rebuilds). No
  effect or Fox-only code path showed up. After v43 the reflection skips
  fighters that are outside it (`decisions.md`; ~70% of reflection bodies
  in a 4-Fox xemu match, ~2% with Marios, who stay near the floor): 4x Fox
  in xemu 976 -> 767 draws a frame, back end 4.4 -> 3.5 ms, 12.2 -> 15.2
  fps. Needs a console `-DXHW_PROF=1` round for the rest.
  crown) drew black (a tester's console, RC1 = v42; xemu too). The
  1st-place branch of `fn_80179990` read its flags through a struct
  overlay that matches `lbl_8046E3AC` only in the GameCube's link order,
  so the portrait was never copied from the EFB (`docs/decisions.md`).
  `scenarios/res`: Fox (port 1) walks off Final Destination, Mario wins.
- Fixed on dev: the 1P Stage Clear / Game Clear background behind the
  bonus list was black (same tester, RC1, 480; xemu too). A 32-bit EFB
  copy kept the back buffer's alpha, often 0, and the sepia freeze frame
  is drawn with the copy's alpha; copies now have alpha 1 as from GX's
  RGB8 EFB (`renderer.md` "Current limits"). The swap tables were fine.
  `scenarios/clear` (Classic stage 1, `MELEE_INSTANT_WIN`).
- Pokémon Stadium (same tester, RC1, 480): the big screen shows corrupted
  graphics for a few frames when it switches to the fight camera. Root
  cause found in xemu (one frame there): an EFB copy unused for 600
  frames is released like a texture (`gx_tex_frame_end`), and when the
  screen goes back to the fight after ~10 s on other views the display
  binds the 640x406 destination before that frame's copy, so it uploads
  the destination's memory, which the GPU copy never writes (garbage, or
  black in xemu). Same for the 124x80 corner view. A first fix (keep EFB
  copies past the idle release, drop one only when a sampled hash of its
  destination memory shows the CPU wrote there) broke the 1P Stage Clear
  freeze frame in xemu (black again): that copy's destination memory
  changes after the copy, so the hash dropped a live copy. Not merged;
  next try: keep copies past the idle release without the hash test, and
  check Stage Clear, Stadium (>40 s) and the texture pool.
- Trophy transition (same tester, RC1, 480; the trophy-to-table view,
  e.g. after Classic): lighting looks wrong, the trophy's body dark
  (Fox's jacket near black, the stand black) under the spotlight, and
  corrupted graphics flash in the background for a moment. Suspects: GX
  spot/distance attenuation (GX_AF_SPOT, nv2a lights), and another EFB
  copy for the background. `scenarios/toy` reaches the scene in xemu: it
  copies a 490x480 colour and a Z24X8 (Z-texture mask) image twice a
  frame into ping-pong buffers, and no destination was bound before its
  copy there. Not judged against the GameCube yet. Two differences from
  GX in `nv2a_vp.c` worth checking: normals are normalized (GX doesn't,
  and HSD's inverse-transpose normal matrix scales them by 1 / the model's
  scale, which the trophy has), and the spot cosine isn't clamped at 0
  before `a0 + a1 cos + a2 cos^2` (GX_SP_COS2 lights behind the spot).
- Peach's Castle (console, v43, 480i, 4 CPUs, burn-in): ~30 min in, the
  stage or camera shakes a lot, more than usual; the game keeps running.
  The shake is constant and the Bullet Bill never leaves either (it
  stays on the stage for good; BACK screenshot on the console). The Bill
  is `grCastle_801CE260`/`801CE578` (`src/melee/gr/grcastle.c`): when its
  timer `xCA` runs out it spawns part 2 into `xCC`, and part 2's update
  (`grCastle_801CE860`) requests `QuakeKind_Loop` every frame. Part 2 is
  freed only where the Bill's first animation ends (`grAnime_801C83D0`).
  Two ways to get stuck: that animation-end check never returns true (an
  anim/frame-count port bug), or it fires before `xCA` runs out, so part 2
  is spawned in the second phase, nothing frees it and the Bill frees
  itself without it. Reproduce in xemu with Peach's Castle and Bullet
  Bills; log the phase changes, `xCA`, and the anim end check.
- Fixed on dev: results screen, everyone tied for 1st (xemu, 2026-10-02,
  4-CPU time battles ending 0-0, e.g. `scenarios/ps`): the portrait boxes
  in the bottom cards stayed black. Not a port bug: `fn_80179854` sets
  `x0_6` only when a loser slides off, and `fn_80179990` copies a winner's
  portrait only once `x0_6` is set, so with nobody below 1st the boxes keep
  the file's black placeholder (64x80 RGB565 of zeros), on the GameCube too.
  Only Debug VS reaches it (no Sudden Death state); retail ties go to Sudden
  Death. A `PORT:` deviation sets `x0_6` when nobody lost
  (`scenarios/tie`: 8 s, Final Destination, 4 CPUs, all 1st).
- Classic team card corrupted (tester, 2026-10-02, build not stated yet;
  Classic as Luigi, Team Kirby on Fountain of Dreams): on the card before
  the fight some of the Kirbys drew corrupted. The card is the Z-texture
  mask path (`xgx_ztex_mask`, then an EFB copy per fighter; earlier fixes:
  the depth plane priming and the 720p mask in green). Reproduce with
  `MELEE_BOOT_SCENE=classic`, `MELEE_CLASSIC_STAGE_OVERRIDE=8`,
  `MELEE_CLASSIC_TEAM=kirby`, at 480 32-bit and 720p 16-bit; ask the
  tester for the build, video mode and a photo.
- Particles not drawn (console). First seen as the Fire Flower (v43,
  480i, Temple): the flame stream is not drawn at all while Mario holds
  and fires it. The user found later (2026-10-02, console on v45) that
  it is not only the Fire Flower: no particle effect draws, at 480
  (32-bit, so not the 720p/Z16 depth remap). The v43 note had smoke and
  hit effects drawing: check whether it is a v44/v45 regression (bisect
  the v43..v45 commits in xemu) or was always so. Particles are
  `src/sysdolphin/baselib/psdisp.c`; check their draw path: blend/TEV
  mode, texture format, or a primitive type the back end drops.
- Whole-system freeze after a long uptime, twice: v39 (results screen,
  ~82 min) and v43 (Peach's Castle, 113 min: 60 on Fountain, 47 on
  Peach's). The log stops between two 5 s heartbeats with normal
  `[PERF]` before it; no first fault, no hang report (the watchdog thread
  stopped too), audio repeats its last buffer. Not the GPU stall path.
  Suspects: an interrupt-level or kernel lockup, or a GPU hang that
  takes the memory bus with it.
- Memory leak, new after v39: `[BEAT]` free memory falls ~55 KB a minute
  in 192 KB steps during matches (v42: 8.9 -> 5.5 MB in 33 min; v43:
  9.7 -> 3.6 MB in 110 min; v39 stayed at 8.7 MB for 75 min). MEM1+ARAM
  barely moves (+64 KB), no overflow texture pool, no new allocation in
  the v39..v43 diff; "ARAM on disc" was 12 MB vs 9.6 MB in v39. Would run
  out after ~3 hours. The v45 720p burn-in (38 min) did not leak: free
  memory fell only while MEM1+ARAM grew (+384 KB, pages first touched),
  then stayed flat for 16 min. Next: log a per-subsystem memory breakdown each
  minute and run 15-20 min in xemu. 192 KB is three 64 KB allocation
  granules, so look at kernel-side allocations (virtual memory, handles,
  threads, contiguous memory) as well as the game's heaps.
- Fixed on dev (after v45): the GPU stall after an EFB copy, the
  100-Man freeze below. The v45 720p burn-in (Fountain, 4 CPUs, items)
  stopped at 38 min with the same `LIMIT_ZETA` on the Z/stencil clear
  after a copy. A colour-side surface write after a context-DMA switch
  sometimes didn't take (the pitch here; in a 480i stress run the colour
  DMA object, `LIMIT_COLOR` on the copy quad as in the v1/v2 stalls); the
  copy and the retarget now send them again after a wait for idle
  (`renderer.md`). Stress builds (`-DXGX_COPY_STRESS`): no fix faulted
  19 s into a console match, the final fix ran 22 min clean at 480i with
  twice the stress. Its frame-rate cost (two more idle waits a copy, ~5
  copies a frame) is to be measured on the next RC. The
  same burn-in showed no memory leak (free memory flat after 22 min) and
  15-30 fps (median 22) at 720p.
- Fixed on dev (after v47): no sound and heavy hitching after the
  settings menu's Save and restart (v47 on the console). The relaunch left
  the AC97 engine half running; `xhw_audio_shutdown` now stops the pump
  and resets the bus masters first (`decisions.md`). After a crash the
  console still needs a power-off for sound (the same stuck codec).
- Front LED effects vs modchips (user report, 2026-10-02): on a console
  with a Kronos board the effects fought the chip over the front LED, and
  the console needed a Cerbios recovery. On dev the effects are off by
  default (`led_effects`, old `led` lines ignored) with a README warning.
  Before turning them on by default again: find what the chip does with
  the SMC's LED registers (0x07/0x08) and whether SMBus traffic from the
  worker can collide with it; maybe detect such chips and lock the option.
- 100-Man Melee (Multi-Man Melee) freezes (tester, RC1 = v42, 128 MB
  with `ram128 = 1`, 480 at 32 bits; not reproduced on the 64 MB console).
  The GPU stall family, logs in `~/xemu/hw/logs-tester-100man/`. First
  fault at 825 s, frame 42672 draw 82: `LIMIT_ZETA` (nsource 0x20) on
  `CLEAR_SURFACE` data 3 (Z + stencil), right after an EFB copy to
  035fc000 (256x256) in the same frame. The pushbuffer before it:
  retarget to the back buffer (`SET_SURFACE_PITCH` 0a000a00, offsets 0,
  clip 640x480, format 0x128), `WAIT_FOR_IDLE`, then the copy's
  clear-after-copy of the source rect (x 28..261, y 0..255): colour clear
  (0xF0), then the Z/stencil clear that faulted. PGRAPH 0x400800:
  4a500200 0008c0d5 0000054a 00000000. The first `ZETA` limit fault seen
  (the earlier ones were `LIMIT_COLOR` on the copy quad's `END`). v42
  predates v43's zeta-side fix (the copy's zeta moved onto the target,
  then put back to DMA 10 / offset 0 before the retarget), which v43 ran
  113 min without a stall; retest 100-Man on the next tester build. If it
  comes back: check the zeta DMA context and limit at the clear after a
  copy.
- Adventure, Corneria (console, v43): in the cutscene that cuts to the
  Star Fox team in their Arwings, nobody speaks (no voice lines) and
  Falco's face just stares (no mouth or face animation). Suspects: the
  voice clips (a separate sound bank or stream not loaded or not played)
  and the face animation that is driven with them; or the comm window is
  a render-to-texture / EFB copy showing a stale frame. Reproduce with
  Adventure's Corneria stage (or a boot-scene shortcut to it).
- Credits: the screen goes black now and then (issue #5, not reproduced
  yet; `scenarios/toy` with a second START shows ~10 s of them fine).
- 720p (console, v38, `720p = 1`): runs, but matches draw ~7.5 fps (menus
  55-59) with visual faults, and the 6 MB texture pool runs down to ~95 KB
  free. (Since v45 the default where the dashboard allows it; v44 on the
  console: clean, textures right, frame rate fine.)
  `[PERF]` had `efb 60.0` ms a frame: at 16 bits every EFB copy (the
  shadow maps, ~4 a frame) was read back on the CPU, with a GPU wait each
  (978 idle waits per 600 frames). On dev the copies are drawn by the GPU
  into R5G6B5 textures, the vertex pool is 4 MB (was 3, full),
  `frame_open` skips a redundant full clear, and 16-bit clear colours are
  no longer converted twice (they came out near black; `renderer.md`). In
  xemu with `-DXHW_VIDEO_480_BPP=16` (the same path at 640x480): `efb` ~13 -> ~0.2 ms
  a frame, 2994 -> 603 idle waits per 600 frames. Still needed: a 720p
  `-DXHW_PROF=1` round on the console (the frame rate, and what is left
  of `render`/`sim` at 720p), and an A/B of `-DOCX_Z16_TILE_FLAGS`
  (`0x80000001`, `0x00000001`) against the visual faults.
  v38 left no screenshots of the faults. Fixed on dev: Classic team cards unmasked at
  720p (the Z-texture mask now goes into green, `renderer.md`). After
  v43: the item boxes' and Fountain of Dreams' grass "wrong textures"
  (v41) were Z16 z-fighting, reproduced in xemu at 720p (an HDTV-pack
  EEPROM gives xemu 720p) and with `-DXHW_VIDEO_480_BPP=16`: the crates'
  frames through their fronts, black blotches in the grass, fighters
  see-through over the fountain floor. The match camera's near 0.1 left
  ~3 units a depth step at 16 bits; Z16 depth is now remapped as if near
  were far / 4096 (`renderer.md` "Depth"). The texture pool was not it
  (720p xemu runs: 0 drops). Check on the console; the
  `-DOCX_Z16_TILE_FLAGS` A/B is still open.
- Fixed on dev: at 16:9 (console, v38, 480p; 720p too) the in-match timer
  sat right of centre. melee-pc's wide HUD anchored it to the right edge,
  but its joint is at x = 0, top centre, which hor+ already keeps centred
  (`src/melee/if/ifall.c`). Checked in xemu at 480p 16:9; 4:3 unchanged.

## Next

1. Release: a plain build of the current `main`, `package_release.py`, a
   GitHub release (only when the user asks). v3's notes
   (`.github/release-notes.md`) thank maple72 prominently for the RC
   testing on real hardware (user's request).
2. Frame rate: the CPU is the limit everywhere (render pass ~60%, sim
   ~40%). See the plan below.
3. Rendering gaps: fog against Dolphin; the cloak's refraction (indirect
   texturing) checked on the console. (Fountain of Dreams' reflection and
   water are geometry: no indirect texturing there.)
4. Cache headroom: the display-list vertex pool and the texture pool both
   run full in long sessions; measure what more RAM for them buys before
   taking it from the game's ~7 MB.
5. Open questions: why bank 2's real SFX fill can outgrow the game's SSM
   accounting (the overflow line logs the numbers if it happens again); the
   FPS counter's options-menu toggle (now the settings menu on the title
   screen).

## Console history

Short; the details are in `renderer.md`, `platform.md` and `decisions.md`.

| build | result |
|---|---|
| v6 (09-29) | first matches on hardware, ~14 fps, ~2750 draws a frame |
| v17-v25 | Fountain of Dreams' 104 KB stage list cached (was decoded every frame, ~25% of the CPU); four-chain content hashes; pdclib `%f` crash fixed (`xsdk_vsnprintf`); shadow-map black flashes (vertex-cache read-ahead, `BREAK_VERTEX_BUFFER_CACHE`); trophy count byte order; Stadium ~28 fps, Fountain 23-28 |
| v26-v29 | GPU hang on Pokémon Stadium (v27/v28): bisect switches, `trace.log`, PGRAPH stall report; not seen since v29. Jungle Japes SFX bank-2 hang |
| v28 | intro movie right, Corneria fixed, rumble strength, Trophy Collection smooth (overflow texture pool) |
| v31 | Kirby's Falcon helmet, capsules and crates fixed (`HSD_TExpSetReg` zero-init); off-screen DObj cull; Mute City 30-40, Onett 40-50, Fountain ~15-24 |
| v32 | silent AC97 boots fixed (`aci_start`), Data -> title SFX hang fixed; Fountain 21.6 fps (sim 4.5 ms a tick at 2.7 ticks, render 17-19 ms, ~885 draws) |
| v33 | CPU/GPU overlap and cheaper back-end lookups: Fountain 24-27, no hitching |
| v34 | HSD prefetches, inline matrix setters, centre/extent cull: Fountain 28-33, sim per tick 4.7 -> 4.05 ms |
| v35 | envelope-matrix memo; stopped on Big Blue's first frame (byte-swapped stage params) |
| v36 | Big Blue fixed; release candidate (above) |

## Performance plan

A match frame on the console is CPU-bound: the GPU waits are ~0 since v33.
Melee renders once per frame and runs one simulation tick per pad poll
since the last render (up to 5), so a cheaper tick pays twice (less time per
tick, fewer ticks per render). Done, roughly by gain:

- fewer draws (display-list batches merged, immediate batches joined,
  fixed array offsets), off-screen cull of rigid DObjs;
- display lists and textures cached with staggered, sampled rechecks;
- builtin memcpy & co., the platform's own string functions, alarms
  throttled;
- shorter vertex programs with forecast residency (`renderer.md`);
- CPU/GPU overlap (v33), packed cache arrays, O(1) vertex signatures;
- bit-identical HSD rewrites: spline and `HSD_MtxSRT`, `C_MTXConcat` in
  SSE, the fused envelope blend, no calls for idle animations, prefetches
  in the list walks (v34), the envelope-matrix memo (v35).

Ideas left, in order:

1. **Render pass** (~12 ms of a ~27 ms frame in v36): HSD's per-material
   setup (`HSD_MObjSetup`, TEV/channel setters) and `PObjSetupMtx`. A
   `-DXHW_PROF=1` round on the console first: `prof_report.py` with the
   build's map and `.statics`.
2. **Simulation**: stage collision (`mpLib_*`), `sinf`/`cosf` (memoize per
   joint angle if a console count shows angles repeat), HSD animation.
   Bit-identical only (`test_anim_mtx.py`).
3. **Cache capacity**: more display-list slots or vertex pool (see above).
4. **Vertex-program loads**: capture `-DXGX_DEBUG_VPTRACE` on the console
   and replay with `vp_policy.py`; let a program that writes more outputs
   serve draws that read fewer.
5. Smaller: `-ftrivial-auto-var-init-max-size` for large locals in hot code
   (after checking which rely on the zeroing).

## Future features

- **Button mapping in the settings menu** (`xbox/src/sdk/menu.c`): a page
  per port that waits for a press, as OpenCrossing's bindings page does.
  Today the menu covers everything in `settings.ini` but the `[portN]`
  button lines.

- **Netplay** (LAN and online): melee-pc's netplay, LAN discovery and
  lobby are not built (`stubs.c` reports them off, and the lobby scene
  returns to the menu). Needs nxdk's network stack (lwIP) under
  melee-pc's `pc_net_*`/`pc_lan_*` layer, rollback's memory snapshots
  within the Xbox's 64 MB, and the simulation's timing at 60 ticks on the
  console.
