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
  - [ ] indirect texturing
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
- Credits: the screen goes black now and then (issue #5, not reproduced yet).
- 720p (console, v38, `720p = 1`): runs, but matches draw ~7.5 fps (menus
  55-59) with visual faults, and the 6 MB texture pool runs down to ~95 KB
  free. Experimental and opt-in only; a dashboard set to 720p gets 480.
- Fixed on dev: at 16:9 (console, v38, 480p; 720p too) the in-match timer
  sat right of centre. melee-pc's wide HUD anchored it to the right edge,
  but its joint is at x = 0, top centre, which hor+ already keeps centred
  (`src/melee/if/ifall.c`). Checked in xemu at 480p 16:9; 4:3 unchanged.

## Next

1. Release: a plain build of the current `main`, `package_release.py`, a
   GitHub release (only when the user asks).
2. Frame rate: the CPU is the limit everywhere (render pass ~60%, sim
   ~40%). See the plan below.
3. Rendering gaps: indirect texturing (Fountain of Dreams' reflection,
   water), fog against Dolphin.
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
