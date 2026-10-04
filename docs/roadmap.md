# Roadmap

- [x] Toolchain: nxdk + LLVM 21, `disc_lower`, ABI-checked game triple
      (`docs/toolchain.md`); native Windows build (MSYS2)
- [x] All 1008 game translation units compile to i386 COFF
- [x] Platform: OS, DVD (.iso/.gcm/.ciso), AR/ARQ, PAD (settings.ini), VI
      (60 Hz; 720p/480p/480i from the dashboard), AX/AI -> AC97, CARD (`.gci`
      in `E:\UDATA\4d580001\card_a\`), MTX
- [x] GX -> NV2A: display lists, generated vertex programs, TEV -> register
      combiners, native texture formats, GPU EFB copies, 720p hor+, fog (not
      yet compared with Dolphin on the console), TEV swap tables, indirect
      texturing (`docs/renderer.md`)
- [x] movies (`thp.c`); memory fit on hardware; 4-CPU VS stable, audio,
      saves, the 100% save loads
- [x] dashboard icon, BACK screenshots (`shotNN.bmp`, on by default in
      test builds)
- [x] settings menu (BACK on the title, `docs/platform.md`); button
      mapping is still `settings.ini` only
- [x] front LED effects (`docs/platform.md`); off by default since v48
      (modchip report, below)
- [x] releases: v1-v3 out (`.github/release-notes.md`), built on GitHub
      with ThinLTO + PGO (`docs/toolchain.md` "Release")
- [x] a full or unwritable E: is logged, noticed on screen, and can't
      truncate a save (v52, decisions.md "Writes to a full or unwritable E:")
- [ ] VS mode with 4 players on hardware at 60 fps

## Where it stands (2026-10-04, v52)

Fully playable on the console; the simulation keeps 60 ticks a second at
any frame rate.

v52 is `dev` ef52994 (pushed), a test build (`-DXHW_PROF=1`,
ThinLTO + PGO) on two consoles: red and gold. Stage and map: `C:\xemu\hw\stage-v52`,
`C:\xemu\hw\melee_x.v52.map`. It carries the frame-rate work (Fountain
of Dreams 4-CPU 720p 32.0 -> 39.9 fps, `docs/fps-plan.md` "Outcome"), the
Stadium screen-copy fix (item 3), the AC97 boot fix (item 9) and the
full/unwritable E: handling.

- xemu gate (`C:\xemu\rc52`): Fountain lockstep shots byte-identical and
  [SIMH] equal vs dev; Stadium 16-bit lockstep [SIMH] equal (shot
  differences also seen between two dev runs); Stage Clear as dev;
  relaunch audio as designed; full-E: runs show the notices; card.c host
  harness 10/10.
- Red, first session: audio healthy (one `AC97 polled`, `[AUDIO]
  found/idle` clean), 720p 16-bit, fps median 46 (23-60) across Stadium
  and Fountain, no crash or hang, settings and card saves and 8 BACK shots
  written; the user says Fountain is fine.
- Gold: in test. Burn-in tests next on both.

## Known issues

Fix order (2026-10-04):

1. **Memory leak since v39.** `[BEAT]` free memory falls ~55 KB a minute
   in 192 KB steps during matches (v43: 9.7 -> 3.6 MB in 110 min; v39
   flat for 75 min; v45's 720p burn-in did not leak). MEM1+ARAM barely
   moves, no new allocation in v39..v43. Next: log a per-subsystem memory
   breakdown each minute, 15-20 min in xemu; 192 KB = three 64 KB
   granules, so check kernel-side allocations (VM, handles, threads,
   contiguous memory) too.
2. **Whole-system freeze after a long uptime** (v39 ~82 min, v43 113
   min): log stops between two heartbeats, no fault or hang report (the
   watchdog stopped too), audio repeats its last buffer. Suspects: an
   interrupt-level or kernel lockup, or a GPU hang taking the bus. Check
   whether it follows the leak (free memory at the freeze).
3. **Pokémon Stadium big screen garbage on the fight-camera switch**
   (tester, RC1, 480). Fix in v52, awaiting console confirmation: EFB
   copies outlive the idle release (decisions.md "EFB copies outlive the
   idle release"). Check: back to the fight camera or close-up after >10 s
   of other views, the 124x80 corner view, `scenarios/clear`, the texture
   pool.
4. **Peach's Castle: Bullet Bill stuck, endless quake** (v43, ~30 min
   burn-in). `grCastle_801CE260`/`801CE578`/`801CE860`
   (`src/melee/gr/grcastle.c`): part 2 requests `QuakeKind_Loop` every
   frame and is freed only at the Bill's first anim end
   (`grAnime_801C83D0`). Either that check never fires (anim port bug) or
   it fires before `xCA` runs out. Repro in xemu; log the phases, `xCA`,
   the anim-end check.
5. **Particles not drawn** (console, v45, 480, all particles; v43 had smoke
   and hits: bisect v43..v45 in xemu). `psdisp.c`: blend/TEV, texture
   format or a dropped primitive type. Same group, status unknown since
   v43: Adventure Corneria's Arwing cutscene silent with Falco's face
   frozen (voice bank or a stale EFB copy); trophy transition lighting dark
   and a background flash (`scenarios/toy`; suspects in `nv2a_vp.c`:
   normals normalized where GX doesn't, spot cosine not clamped at 0).
6. **100-Man freeze** (tester, RC1 = v42, 128 MB, 480): `LIMIT_ZETA` on
   the clear after an EFB copy (logs `~/xemu/hw/logs-tester-100man/`); the
   copy fix since v46 targets it. Confirm with the tester on the next RC.
7. **Classic Team Kirby card: some Kirbys corrupted** (tester, build not
   stated). Z-texture mask path. Repro: `MELEE_BOOT_SCENE=classic`,
   `MELEE_CLASSIC_STAGE_OVERRIDE=8`, `MELEE_CLASSIC_TEAM=kirby`, 480 32-bit
   and 720p 16-bit; ask for build, mode, photo.
8. Fixed for v53: **128 MB consoles always in 64 MB** (user, 2026-10-02).
   `[system] ram128` and the menu's "Use 128 MB RAM" row are gone,
   `xhw_mem_hold_upper` runs at every boot; old files' `ram128` lines are
   ignored and not written again (decisions.md, platform.md). The menu's
   press and repeat timing now counts retraces (a slow title in xemu lost
   or repeated presses); `scenarios/settings` passes in xemu with a v52
   `ram128 = 1` file staged. To check on a 128 MB console: `[MEM] 128 MB
   console: running in 64 MB` in boot.log.
9. **Silent boot after a crash or a power cycle** (`AC97 stuck: civ 0 lvi
   6`, cold resets freezing the game each ~3 s; seen on console A after a
   power-off, dashboard, launch; log `C:/xemu/hw/logs-audio-20261004-1153`).
   Fix in v52, awaiting console confirmation: the AC97 bus masters and the
   APU are idled at boot, a stuck engine is given up after one recovery
   with a notice, `lastexit.txt` records how the last boot ended
   (decisions.md "The audio hardware is brought to idle at boot"). To
   check: power off mid-match, dashboard, launch; read `[AUDIO] found`.
10. **Simulation not deterministic on the console** (fps-plan.md round 3):
    Fountain 4-CPU seed 1 parts at tick 4440-4500 into three outcomes;
    xemu `-icount` too on `lan-0a-probes` (tick 4447). Harmless to play;
    rules out replays and hardware [SIMH] gates. Next: log `HSD_Randi`'s
    callers around those ticks. Maybe related: one 2-pixel speck in one
    720p lockstep shot of 15 (round 6); repeat that run.
11. **Stadium red lights and platform marks break up at 720p** (v51,
    `C:\xemu\hw\logs-stadium-tex`). Z16 depth precision, not a texture bug:
    the lights sit 0.25 units over the frame, a Z16 step there is ~0.5-0.7
    units (GameCube ~0.07-0.1); 480 (Z24) is right. No fix yet. Options,
    none small: a per-frame near plane from what was drawn, reversed float
    Z16 (console only), or Z24S8 with 32-bit colour at 720p (+7 MB).
12. **v3 player (Reddit; 128 MB, CPU upgrade):** settings.ini not
    regenerated, no BACK .bmp, widescreen setting no effect. E: has >1 GB
    free, so not a full disk. A v52 test zip with diagnostics went to him
    (`[BOOT] save folder write test`, `[VIDEO] dashboard` flags); waiting
    for his boot.log. Widescreen at 480 also needs the dashboard's
    widescreen flag; BACK shots are on by default only in test builds
    (`[system] screenshots`, platform.md).

Other open:

- Rainbow Cruise ship flicker (v48, 720p, looks like shadows): try
  `-DXGX_COPY_FIX=0` first, then the shadow-map copy/clear order.
- Front LED vs modchips (Kronos; needed a Cerbios recovery): off by
  default; before turning it on again, find what the chip does with SMC
  LED registers 0x07/0x08.
- Credits go black now and then (issue #5, not reproduced).
- Fox costs ~10 fps vs a mixed 4-player match (v43-era): twice Mario's
  PObjs, more material switches; reflection skip since v43. Status unknown
  since the frame-rate work.
- 720p visual faults of v38 (no shots): the `-DOCX_Z16_TILE_FLAGS` A/B
  against them is open (no fps difference, fps-plan.md round 4).
- GPU stall after an EFB copy, if it comes back: PGRAPH 0x400800-0x40080C
  and the last copy's target are logged at the first fault;
  `scenarios/stall`, `MX_COPY_STRESS`.

### Fixed

- v1 hang on the intro movie (GitHub #5, #6): `xgx_present` called
  `pb_finished` with the frame's tail open; test builds' FPS counter hid it.
- Mid-match GPU stalls after an EFB copy (`LIMIT_COLOR`/`LIMIT_ZETA`, v1-v45):
  copy clipped to `pw - 1` (v40), surface switches sent twice (v46;
  decisions.md, renderer.md).
- Classic team cards half black: depth plane priming and `GXSetZTexture`
  as a mask (renderer.md).
- Star KOs ending at once (`ft_0D31.c`, decisions.md).
- Results 1st-place portrait black (RC1) and tie portraits black
  (decisions.md "Edits to imported code"; `scenarios/res`, `tie`).
- Stage Clear background black (RC1): EFB copies have alpha 1
  (`scenarios/clear`).
- 16:9 match timer right of centre (v38; `ifall.c`).
- No sound after Save and restart (v47): AC97 idled before a relaunch (v48).
- 720p at ~7.5 fps and Z16 z-fighting (v38-v41): GPU copies into R5G6B5,
  Z16 depth remap (decisions.md); 720p default since v45.
- 128 MB consoles always run in 64 MB, no `ram128` setting (v53, item 8).

## Next

1. Console confirmation of items 3 and 9 on red and gold.
2. Burn-in on both consoles (items 1, 2, GPU stalls).
3. The Reddit player's boot.log (item 12).
4. PGO retrain: `gx_tex.c` changed since the profile (`docs/pgo.md`).
5. Then the fix-order list above. Later: fog vs Dolphin and the cloak's
   refraction on the console; whether more RAM for the texture and
   vertex pools (both full in long sessions) is worth it; why bank 2's SFX
   fill can outgrow SSM's accounting; a release from `main` when the user
   asks.

## Console history

Details in `renderer.md`, `platform.md`, `decisions.md`, `fps-plan.md`.

| build | result |
|---|---|
| v6 (09-29) | first matches on hardware, ~14 fps |
| v36 (10-01) | release candidate; 10-min match 30-54 fps, CPU-bound |
| v38 | 720p runs at ~7.5 fps (CPU EFB readbacks) |
| v43 | 113 min without a stall; long-uptime freeze (item 2) |
| v45 | 720p burn-in 38 min, `LIMIT_ZETA` stall, no leak |
| v48 | sound kept through restarts |
| v51 | Stadium 720p shots (items 3, 11) |
| v52 (10-04) | frame-rate work, items 3/9 fixes, E: handling; red fps median 46 |

## Performance plan

A busy 720p match is limited by the CPU (~22 ms a frame) and the GPU
about equally. A cheaper simulation tick pays twice (Melee runs up to 5
ticks per render). Done: fewer draws, cached display lists and textures,
shorter vertex programs, CPU/GPU overlap, bit-identical HSD rewrites
(v33-v35), then the frame-rate work (`docs/fps-plan.md`): ThinLTO + PGO,
the colour tile region, function order, mixer in SSE and more.

Left, each measured in a console round (`docs/testing.md` "Console
rounds"):

1. Render walk (~9.6 ms a frame, Fountain 720p): HSD material setup and
   the scene walk.
2. GPU fill: reflection pass (~4.4 ms), shadow maps (~2.2 ms).
3. Simulation (~4.9 ms): `sinf`/`cosf`, HSD animation; bit-identical only.
4. Vertex-program loads: `-DXGX_DEBUG_VPTRACE` + `vp_policy.py`.

## Future features

- Button mapping in the settings menu (`xbox/src/sdk/menu.c`), a page per
  port.
- Netplay: melee-pc's netplay/LAN isn't built; needs lwIP under
  `pc_net_*`/`pc_lan_*`, rollback within 64 MB. LAN plan: `docs/lan-plan.md`.
