# Handoff: Pokémon Stadium texture report (branch fix-stadium-tex, uncommitted)

## Images (console, v51 dev play build, 720p 16-bit; boot.log = that session)
`C:/xemu/hw/logs-stadium-tex/` (shot00-10 .bmp + .png, boot.log; identical to the coordinator's
`logs-audio-20261004-1153` shots; its boot.log is a later boot). Zooms: `z07_top.png`, `z07_bot.png`,
`z07_noise.png`, `z08_plat.png`, `z09_plat.png`, `z10_screen.png`, `z04_line.png`.
- shot00, shot05: settings menu, fine.
- shot01-04: Mushroom Kingdom II, fine (01 is the KO blast; the thin line in 04 looks like Pidgit's carpet).
- shot06-09: Stadium with the text view on the big screen. The red lights on the frame show as slivers
  (06/07/08). The side platforms' red stripe and yellow marks are broken up (z08/z09).
- shot10: fight camera on the screen. The grid seams and grain are in the data. The red lights are broken here too.

## Causes
1. **Red lights / platform decals: Z16 depth precision at 720p.** Not a texture problem. The lights are
   vertex-coloured arrowheads (GrPs.usd map gobj 1, joint 1, first DObj) 0.25 units in front of the frame
   face. The frame is drawn after them with LEQUAL. The remap (`xbox/src/hw/nv2a.c` ~1641-1685,
   `XGX_Z16_DEPTH_RATIO` 4096) puts the effective near plane at 4 units, so one depth step is ~0.5-0.7 units
   at the screen. The GameCube's Z24 resolves ~0.07. The [DBGZ] log confirmed g0 0.975 from the match camera.
   In xemu, 32-bit (Z24) draws clean triangles (`C:/xemu/run-tex/b32/`) and 16-bit reproduces the slivers
   (`C:/xemu/run-tex/base16_*`). No safe fix in scope. Options are in docs/roadmap.md item 11.
2. **Static: intended.** JObj 1's second MObj is an XLU IA4 64x64 noise texture tiled 5x3 over the screen
   glass, in the game data. It shows at both depths.
3. **Corrupted screen frame (roadmap item 3).** This may be what the user saw as "static". An EFB copy idle
   for 600 frames was released, and the screen then bound its destination before the frame's copy, which
   uploaded garbage. Reproduced in xemu (`C:/xemu/run-tex/dbge/`, [DBGE] log `dbge_serial.log`: frame 2283
   idle release, frame 3320 eviction).

## Diff (gx_tex.c, tests/xbox/gx_tex_ref.c, tests/xbox/test_tex_cache.c, docs)
- EFB copies are no longer idle-released.
- An idle copy binds only at its copied size, and is dropped at a scene change.
- A destination that was copied to again (efb=2) gets an eviction grace of 3600 frames instead of 60.
- Docs: decisions.md, renderer.md, roadmap.md items 3 and 11.

## Tests
- `test_tex_cache.py` passes. The extended sequence reaches idle copies about 6 times. Run it with
  `CC=C:/msys64/mingw64/bin/clang.exe`.
- Not run: test_tex_convert and the other host tests.
- Not done: the xemu after-run (killed at session end) and the `clear` scenario check.

## Next steps
1. Build `XBOX_CFLAGS="-DXHW_AUTOPAD=1 -DXHW_VIDEO_480_BPP=16"`.
2. Run `scen_ps2` (scratchpad `scen_ps2`, 150 s Stadium) with a temporary [DBGE] log in
   gx_tex_note_efb_copy. Expect no idle-release replacements.
3. Run `scenarios/clear` and check the sepia freeze frame.
4. Run Stadium shots against `base16` (the picture must be unchanged).
5. Run the remaining host tests.
6. Console A/B. The PGO profile needs retraining later.
