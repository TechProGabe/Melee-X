# Decisions

These are the choices the port rests on and why each was made. When
changing one, update this file.

## Scope, agreed at the start

| question | decision |
|---|---|
| Game data | Read the user's own `GALE01` (NTSC-U 1.02, revision 2) disc image at runtime, from `.iso`, `.gcm` or `.ciso` next to `default.xbe`. Nothing from the disc is converted ahead of time or committed. |
| 720p aspect | 16:9 widescreen (hor+) at 720p, using melee-pc's widescreen code. 480 follows the dashboard's 4:3 / 16:9 setting. |
| Video defaults | 480i/480p from the dashboard; 720p experimental and opt-in (`720p = 1`). v1 used 720p whenever the dashboard allowed it, and only 480i 4:3 had been tested on hardware (the intro hang in #5/#6 turned out to be the flip bug in `xgx_present`, not the mode). `progressive = 0` forces 480i. |
| OpenCrossing-Xbox | Reuse its port layer where it applies: the audio drivers, the crash reporter, the TEV -> register combiner compiler, pbkit patches, video mode rules and hardware notes. |
| Controller layout | GameCube-like by position: A=A, X=B, B=X, Y=Y, White/Black=Z, triggers=analog L/R, right stick=C-stick. It can be remapped per port. |
| Players | 4, one per physical controller port. |

## Technical decisions

**Start from melee-pc, not raw doldecomp.** melee-pc has already made the
decomp run on little-endian hosts. Disc structs are marked `DISC_STRUCT`
and stay big-endian in memory; `DISC_PTR` handles pointers in disc data;
and `src/pc` provides the audio mixer, widescreen, libm and more. The
upstream commit is recorded in `src/UPSTREAM_COMMIT`.

**Lower DISC_STRUCT at build time.** melee-pc uses GCC's
`scalar_storage_order`, and nxdk is clang-only. `tools/lower/disc_lower`
(LibTooling, taken from melee-pc's browser build) rewrites those accesses
into explicit big-endian loads and stores. `tools/lower/test_lower.py`
compares the result with GCC. Static initializers of disc structs are
stored big-endian too; for structs with bit-fields only the bit-fields were
(`tests/lower/disc_bitfield_init.c`), so every other field of such a static
table was byte-swapped garbage. `ityaku.c`'s `ItemAttr` for stage items (the
Great Fox's gun, Green Greens' blocks) read `x60_scale` 1.0 as ~4.6e-41,
and each hitbox was scaled by its inverse: Corneria's gun hit every fighter
on the stage from the start (console and xemu).

**The game triple is `i686-pc-windows-gnu -mno-ms-bitfields`.** It gives
GameCube bit-field layout and 8-byte `long long` alignment, and it has the
same calling and struct-return ABI as nxdk's `i386-pc-win32`, which was
checked in the generated assembly. `docs/toolchain.md` has the full table.

**LLVM 21.1.8.** nxdk warns that clang 19.x-20.1.2 miscompile it
(llvm/llvm-project#134607), and the release tarball includes the clang
libraries that `disc_lower` needs. nxdk is pinned to the same commit
OpenCrossing uses.

**Our own GX implementation, not aurora's.** aurora records the GX FIFO
and compiles TEV to WebGPU through Dawn, none of which exists on the Xbox.
Its public headers are kept, so the game compiles unchanged, and GX is
implemented directly on the NV2A (`docs/renderer.md`).

**Generated vertex programs.** GX lighting, skinning and texgen are more
than the NV2A's fixed function can do. Programs are generated per
configuration, and skinning uses `a0` to index the matrices.

**The frame boundary is `GXCopyDisp` and `VIWaitForRetrace`** (melee-pc's
model). The flip happens at `GXCopyDisp`, while the VI wait paces to
60.000 Hz and runs the alarms.

**Alarms and interrupts run on the game thread.** `OSDisableInterrupts` is
a recursive lock. This avoids true asynchrony in code that was written for
a single-core console with interrupt handlers.

**Memory: a GameCube-sized MEM1 and ARAM, committed on demand.**
- Shrinking the arena would change the game's heap layout, and untested
  that's riskier than committing only what the game touches.
- 720p runs at 16-bit colour, as OpenCrossing measured it had to.
- Textures use native NV2A formats.
- ARAM pages that hold bytes straight from the disc image stay on the disc
  (below).

**Memory-card files are big-endian, as on the GameCube.** The port stores
.gci files so that saves move to and from Dolphin and real cards, and that
includes the contents: Melee's save data and name tags are converted at the
card boundary (`xbox/src/sdk/card_endian.c`, called from `lbcardgame.c`)
from field tables built with `offsetof`/`sizeof` of the game's structs, so
a layout change stops the build or `test_card_endian.py` instead of
mis-swapping. Writes go out from a big-endian copy, never by swapping the
live buffer, because lbcardnew's write runs over several frames while the
game keeps using its data. Saves written by earlier builds are
little-endian; they are recognised on load by a plausibility vote over the
fields and read as they are, and the next save converts them.

**Our own memcpy, memmove, memset and memcmp** (`xbox/src/hw/xhw_string.c`).
nxdk's pdclib implements them as byte loops, and on hardware they took about
40% of a VS match's frame. The platform's versions move 32 bits at a time
and win the link over libpdclib.

**Disc image reads go straight to the kernel** (`xhw_file_read`, used by
`dvd.c`). pdclib's `fread` reads 1 KB per `ReadFile` and copies byte by
byte; a load is now a few 1 MB reads into the destination.

**Disc-backed ARAM pages** (`ar.c`). Melee preloads files into ARAM while
the menus idle; with faster disc reads it filled MEM1 and ARAM to 31 MB
committed by the first match on the console, 1.3 MB from out of memory.
A copy into ARAM of bytes the DVD worker just read (devcom's relay buffer,
posted from the read's callback) only records per 4 KB page where on the
image they are. A 64 KB chunk whose pages are all on the disc is
decommitted; ARAM -> MEM1 transfers read the image; a CPU touch (the audio
mixer reads samples in ARAM) faults and the chunk is refilled from the
image under the region's fill hook (`xhw_lazy_set_fill`).

**Compiler builtins for memcpy & co.** (`xbox/include/xbuiltin.h`). Every
unit is built `-ffreestanding` (nxdk-cc's flags, and the game's to match),
which implies `-fno-builtin`: a `memcpy(v, out, 12)` was a real call and a
`rep movsb`. The header, force-included after `<string.h>`, maps the four to
`__builtin_*`, so constant sizes are inlined and the rest still calls
`xhw_string.c`. On the console those calls were ~18% of a match frame.
`sqrtf`/`sqrt` are inlined the same way (`sqrtss` / `fsqrt`, the instructions
pdclib's out-of-line versions wrap, so results are unchanged).

**Alarms at most every ~0.3 ms between frames** (`os.c` `deliver_pending`).
The port delivers alarms (pad polling, card and DVD completions) when the
game thread re-enables interrupts; HSD does that thousands of times a frame,
and each time converted the time and walked the alarm list. Now an rdtsc
check skips it within ~0.3 ms of the last run; the frame boundary still runs
them every frame.

**Fewer, bigger draws** (`gx_vtx.c`, `docs/renderer.md`). A display list's
batches are merged into one draw (strips stitched with degenerate
triangles), finished immediate-mode batches wait to be continued by the next
one with the same state, quads and fans go out as triangle lists, and array
offsets stay fixed so back-to-back draws send nothing in between. In xemu on
macOS every draw is a geometry-shader compute pass and a render pass of its
own; a 4-CPU match went from ~2950 to ~800 draws and from 4 to ~12 fps
there. On the console it saves the per-draw state work and pushbuffer
traffic. The cost: 1.5x vertices for quads, 2-3 extra per stitched strip.

**Dashboard icon.** `tools/xbox/xbe_title_image.py` (from OpenCrossing-Xbox)
adds a `$$XTIMAGE` section (128x128 DXT1 XPR0) after the link, and writes
`default.tbn` for XBMC-style dashboards, from `xbox/assets/logo.png`, which
`tools/xbox/make_logo.py` draws (original art). Runs on the host after the
Docker build when Pillow is there. It also sets the certificate's title ID
to 4D580001, the `E:\UDATA` folder the port saves to: cxbe leaves nxdk's
FFFF0002, shared by every nxdk title, and the console's dashboard showed
another homebrew's icon and name cached under that ID
(`E:\UDATA\ffff0002\TitleImage.xbx`, `TitleMeta.xbx`). The script writes
both files for 4D580001 next to `default.tbn`.

**GX fog per vertex, from GX's own registers** (`nv2a_fog.c`,
`docs/renderer.md`). The NV2A has no per-pixel depth fog: its fog unit
interpolates a factor the vertex program writes. The vertex program
computes GX's fog amount from each vertex's depth with the registers
`GXSetFog` would write (A and C cut to 11 mantissa bits, b_mag and
b_shift), and the fog unit passes it through (LINEAR, parameters 1, 1, 0);
the final combiner mixes that much fog colour into the colour. Rebuilding
the registers, not the textbook formula, matters for Melee: with its
0.1..16384 camera GX's b_mag rounding makes the fog depth ze up to 1.57
times smaller at the far plane (thinner fog). The cost: GX evaluates fog per pixel, so a polygon spanning a long
depth range gets less fog mid-span than on the GameCube (up to ~23/255 on
long edges), and the EXP curves are linear between vertices.
`GXSetFogRangeAdj` is not applied.

**Shorter vertex programs, and program memory by forecast** (`nv2a_vp.c`,
`nv2a_vpmem.c`, `docs/renderer.md`). The console loaded ~80 programs (~60
instructions each) per frame of a 4-CPU match, because the 136-instruction
program memory was packed and flushed whole. Programs are now optimized
(outputs written directly, dead lanes dropped, ILU ops paired with MAC ops),
lighting skips the attenuation factors that are exactly 1 for infinite and
point lights, keys are canonical, and a program is placed by Belady's rule
with the previous frame as the forecast. Only rewrites that give the same
bits were allowed: `test_vp_opt.py` compares every output with the old
generator's through an interpreter, and keys the old generator had to
approximate are approximated the same way (most would fit now: a possible
accuracy change for later). The cost: generating a program takes ~40 µs on
the host (~8x the old generator; once per new key, 64 cached), and the
attenuation shortcut relies on rcp(1.0) being exactly 1 on the NV2A.

**Vertex cache breaks, overflow texture pool, cheaper pools** (v26,
`docs/renderer.md`). Each pushbuffer batch starts with
`BREAK_VERTEX_BUFFER_CACHE` (shadow-map wedges on the console). A scene
whose textures outgrow the pool gets an overflow pool from free RAM until
the next scene change (Trophy Collection). Display lists evicted when not
drawn this frame are freed without waiting for the GPU; the pool allocator
keeps a free list and an offset hash; stable display lists are checked
every fourth frame, and so are stable textures; non-power-of-two textures
are linear textures sampled in texels (texgen rows scaled), not resampled; `GXLoadTexMtxImm` changes
nothing when the matrix is the same; kicks every 32 KB. Non-power-of-two
intensity textures stay AY8/A8Y8.
The cost: up to 8 MB more RAM while the overflow pool exists, and a list
or texture rewritten in place may draw stale for up to three frames.
`-DXGX_PB_KICK`, `-DXGX_VB_CACHE_BREAK=0` and `-DXGX_VBUF_FREE_NOW=0` undo
the GPU-side changes one at a time, to bisect the console's GPU stalls.

**The GPU finishes a frame while the CPU starts the next (v33).** The wait
for GPU idle moved from `xgx_present` to the next frame's first GPU use
(`frame_open`), so the GPU draws the last frame and runs the queued flip
during the next simulation ticks; the frame-rate counter is drawn by the
GPU. The invariant every frame relies on (an idle GPU when the frame
opens) is kept. `-DXGX_OVERLAP=0` restores the old order. With it, v33's
other back-end changes (`docs/renderer.md` "CPU cost of the back end"):
stable lists and textures are revalidated with 16 samples instead of 64
(same schedule), and textures above 512 bytes are sampled rather than
hashed in full; the cost is a lower chance of noticing a partial in-place
rewrite of a list or texture that had stayed the same for two seconds.

**Release builds have no test tools (after v36).** A plain build is a release:
BACK does nothing and the frame-rate counter is off unless `settings.ini`
turns it on. The profiler and autopad builds, which every console test
round uses, imply `XHW_TEST_BUILD` and keep BACK screenshots and the
counter (`docs/testing.md`).

**Logs that survive a long session.** `boot.log` keeps the first 4 MB, then
the log alternates between `boot2.log` and `boot3.log` (2 MB each), and
`[DRAW]` trace lines go to `trace.log`: v28's trace filled the old 2 MB cap
at ~145 s and its GPU hang at ~500 s left no stall report.

**128 MB consoles run in 64 MB by default (after v2).** Both mid-match GPU
stalls reported so far (`LIMIT_COLOR`) came from a 128 MB console, and
every console and emulator this was tested on has 64 MB. At boot,
`xhw_mem_hold_upper` allocates the RAM above 64 MB and never frees it, so
the kernel and the game only get pages in the low 64 MB, as on a stock
console. `settings.ini` `[system] ram128 = 1` skips that. xemu with
`mem_limit = '128'` still reports 64 MB with a stock BIOS, so this path is
untested there.

**`GXSetZTexture` as a mask (after v2).** The depth test at a depth copy
writes a 0/1 mask into the framebuffer's alpha, the copy carries it, and
the Z-texture draw multiplies its alpha by it and alpha-tests it away
(`docs/renderer.md`). That is exact for what Melee uses it for (the
Classic team card: fighter in front of a cleared background) but not a
general depth replace: the draw keeps its own depth. At 16-bit colour
(720p) there is no alpha, so the mask is written into green, and only for
a depth copy that clears its rect afterwards (the colour there is lost).

**The settings menu is the platform's, over the title screen (after v2).**
BACK on the title opens it (`xbox/src/sdk/menu.c`, `docs/platform.md`).
Melee's Options menu is models and prebaked SIS strings: a page there
would mean new menu data and many imported-code edits. The title is one
scene with one input handler, so one `PORT:` hook there freezes it (no
START, no attract demo) while the menu is up, and BACK is unmapped by
default. The text is CPU writes into the finished frame with the boot
card's 8x16 font (`xhw_overlay.c`): no GPU state, the same at 480 and at
720p in 16-bit. A video mode, widescreen or 128 MB change is saved and
waits for a restart (the mode, the NV2A's buffers and the pools are set up
at boot; OpenCrossing-Xbox does the same), and "Save and restart"
relaunches the XBE. `settings.ini` is written to a temporary file, read
back and then renamed over the old one, so a failed write can't leave an
empty or half file. `ram128` can only be on where the kernel counts more
than 64 MB of physical pages: the menu row is locked off on a 64 MB
console, and a hand-edited `ram128 = 1` there is ignored at boot.

**TEV swap tables as dot products (after v40).** A swizzled source
(`RRRA` and the other GXInit tables) costs one extra combiner stage that
dots it with a unit vector into a spare register; the identity and the
alpha broadcast stay free. A per-texture channel-shuffled copy would cost
no stage but a texture per table, and tables are per TEV stage (the 1P
clear screen reads one texture through three of them). Draws without a
swizzle compile to the same combiner words as before
(`tools/xbox/test_rc.py`). The front end's default tables were all
identity; they are GXInit's now.

**Indirect texturing as BUMPENVMAP (after v40).** The refraction of a
cloaked fighter (`lbRefract`, Melee's only indirect user) maps onto the
NV2A's bump-environment unit: the indirect map on an earlier unit, drawn
from a copy that holds its offsets halved so filtering never crosses the
unit's two's-complement wrap (GX's -128 bias becomes a texgen constant),
and the perturbed texture on a bump unit whose projective coordinate is
divided per vertex. The alternatives were a dependent read (`DEPENDENT_AR`
replaces the coordinate instead of offsetting it) or dot-product stages
(three units for one offset). What BUMPENVMAP can't do (dynamic matrices,
wrap, accumulation) draws direct, as before; `-DXGX_NO_INDIRECT=1` turns
it all off.

**720p frame rate, first batch (after v40).** EFB copies at 16 bits are
drawn by the GPU into R5G6B5 textures, as at 32 bits into A8R8G8B8: the
v38 console match at 720p spent ~60 ms of a 133 ms frame reading the
framebuffer back on the CPU. The cost: an EFB copy at 720p samples alpha
1 (no alpha in R5G6B5), where the CPU path gave I/R copies their
intensity as alpha; Melee's shadow maps take alpha from APREV, and the
Z-texture mask was already off at 720p. 720p's display-list pool is 4 MB
like 480's (1 MB more RAM; the 3 MB one was full in the first match), and
`frame_open` skips its full clear when `GXCopyDisp`'s queued clear covers
the whole framebuffer (16-bit clears also lose a second colour
conversion that made them near black). The Z16 depth tile's flags are a
switch (`-DOCX_Z16_TILE_FLAGS`) with pbkit's value as the default until a
console A/B, and `-DXHW_VIDEO_480_BPP=16` puts the 16-bit path in reach of xemu.

**Vanilla gameplay.** melee-pc's UCF, free camera, frozen stadium,
unlock-all, netplay, Slippi and launcher are off or not built.

## Edits to imported code

Imported files are kept as they are upstream except for these edits, each
marked `PORT:`:

- `src/pc/libm/pc_libm.h`, `pc_rem_pio2f.c`: accept `FLT_EVAL_METHOD == -1`
  under `TARGET_XBOX`. clang reports -1 for SSE float with x87 double, and
  melee-pc's libm is still exact there.
- `src/melee/ft/ft_0D31.c` (`ftCo_DeadUpStar_Anim`): the star KO reads
  `p_ftCommonData->x508`..`x51C_radians` by name. The imported function
  copied `x504` into a local and indexed past it (`data[1]`, `data + 3`,
  `data + 6`), so the KO's frame counts, speeds and spin came from the
  stack: a fighter knocked off the top could vanish at once instead of
  flying into the background (v2 player report).
- `src/melee/gm/gmclassic.c`: melee-pc's `MELEE_CLASSIC_STAGE_OVERRIDE` /
  `MELEE_CLASSIC_TEAM` test hooks moved into `pc_classic_stage_override`,
  which the `MELEE_BOOT_SCENE=classic` shortcut now calls too (it skips the
  character select, where the hook was the only call). Test harness only.
- `src/pc/discfont.c`: the Xbox reads the DOL through its DVD layer
  (`DVDGetDOLLocation`), as the emscripten build does, not through `nod`.
- `src/melee/lb/lbfile.c`, `ft/ftdata.c` (two places), `gr/grdisplay.c`:
  "is this ARAM?" tests used the GameCube's `addr < 0x80000000`. Main memory
  sits at 0x10000000 here, so they use `PC_IS_ARAM_ADDR`.
- `src/sysdolphin/baselib/hsd_3A76.c`: the default kerning table is
  indexed by glyph number. It was indexed by a byte offset, twice too far.
- `src/melee/ft/ftparts.c` (`ftPartsRemap`): the joint byte is read
  unsigned, as on GameCube. Sign-extended, "no such joint" became -1,
  passed the callers' `!= 0xFF` tests and indexed `fp->parts[-1]`. It
  crashed when a fighter was thrown by a different character.
- `src/melee/ft/kinds/ftPikachu/ftpikachuspeciallw.c`: Thunder's entry
  clears `speciallw.x0` by name. On GameCube, the `specialhi.x0 = 0` just
  before it cleared that pointer too. melee-pc moved it to +08, so a stale
  word from the previous state was read as the thunder gobj.
- `src/melee/gm/gmvsmode.c`: `MELEE_DEBUG_VS_TIME=<seconds>` next to
  melee-pc's other debug-VS hooks, so a scripted match can end on TIME!.
- `src/melee/gm/gm_1A3F.c` (`gm_801A4014`): every scene enter and leave is
  logged with the memory picture (`xsdk_scene_log`, `xbox/src/sdk/log.c`), so
  a console that freezes during a transition leaves the scene in `boot.log`.
- `src/melee/gm/gmvs.c`: `[GAME]` log lines where a match ends (TIME!,
  GAME!, no contest) and where the end banner hands over to the results.
- `extern/aurora/lib/dolphin/mtx/mtx.c` (`C_MTXConcat`): four columns at a
  time in SSE under `TARGET_XBOX`, same operations in the same order per
  lane, so bit-identical (checked against the C on 5M random and special
  inputs); it was the hottest math routine in a match profile.
- HSD animation and matrices, rewritten for the Pentium III with the same
  bits (checked by `tools/xbox/test_anim_mtx.py` against a copy of the code
  before, `tests/xbox/anim_mtx_ref.c`; a NaN only has to stay a NaN):
  - `src/sysdolphin/baselib/fobj.c`: the spline's `1.0 / fterm` is a float
    division (for every u16 the double quotient rounds to the same float,
    at x87 double and extended precision; it was `fild`+`fidiv`), and
    `splGetHelmite` is inlined operation for operation (`FObjHermite`);
    `parseFloat` reads a float key in one load and scales a fixed-point key
    by an exact power of two instead of dividing.
  - `src/pc/libm/pc_sincosf.c` (new) and `pc_trig.h`: sinf and cosf of one
    argument, branch for branch `pc_sinf`'s and `pc_cosf`'s, with the same
    double arguments to the same out-of-line kernels (so the x87 precision
    doesn't matter); `src/sysdolphin/baselib/mtx.c` (`HSD_MtxSRT`) uses it
    under `TARGET_XBOX`. LLVM already turns its `(float)(1.0 / x)` into
    `divss`, which is exact for every float.
  - `src/sysdolphin/baselib/mtx.h` (`HSD_MtxConcatScaledAdd`): the envelope
    blend's `MTXConcat` + `HSD_MtxScaledAdd` as one SSE step per row, the
    lanes of `C_MTXConcat`'s SSE path then `acc + w * row`; used by
    `pobj.c` (`SetupEnvelopeModelMtx`) and `src/melee/ft/ftparts.c`
    (`ftPartsSetupEnvelopeMtx`).
  - `aobj.h` (`HSD_AObjIsPlaying`), `jobj.c` (`HSD_JObjAnim`), `mobj.c`,
    `tobj.c`, `robj.c`, `pobj.c` (their `*Anim`): no call into
    `HSD_AObjInterpretAnim`, `HSD_RObjAnimAll`, `HSD_DObjAnimAll` or
    `HSD_TObjAnimAll` for an object they would return from at once (no
    AObj, or one that has stopped; no list).
- `src/melee/mp/mpisland.c` (`mpIsland_8005A728`, `mpIsland_8005B004`): the
  1.5 KB `visited` arrays, which the code `memzero`s itself, are exempt from
  `-ftrivial-auto-var-init=zero` (it zeroed them a second time per call).
- `src/melee/lb/lb_00B0.c` (`memzero`): `memset` instead of the byte loop,
  which `-ffreestanding` keeps as written (~1% of a console match frame).
- `src/melee/if/ifall.c` (`ifAll_802F370C`): the wide HUD doesn't move the
  match timer. `pc_widescreen_hud_timer_x` anchors it to the right edge,
  but its joint sits at x = 0, top centre, so at 16:9 it landed ~100
  pixels (of 640) right of centre; hor+ keeps x = 0 centred by itself.
- `src/melee/gm/gmscene.c` (`gm_801A4D34`): the render pass is bracketed
  with `xsdk_perf_render_begin/end` (`xbox/src/sdk/vi.c`), so `[PERF]`
  separates simulation ticks from rendering and counts ticks per render.
- `src/melee/gr/grpstadium.c` (`grStadium_801D4548`): a transformation is
  picked from `{3, 4, 6, 9}` as upstream doldecomp now does. The imported
  loop used the random index (0-3) as the kind, so Pokémon Stadium's first
  transformation hit `HSD_ASSERT(0xA44)` three times out of four.
- `src/melee/gr/grpstadium.c` (`grStadium_801D42B8`): the transformation
  file is parsed once, after its load has finished. The imported function
  parsed it in the upstream if/else and again after melee-pc's rollback
  block, unconditionally, so the first transformation relocated a
  half-read or already relocated archive and hung in
  `HSD_ArchiveLocateExtern`.
- `src/melee/ft/kinds/ftKirby/ftkirby.c` (`ftKb_LoadHatParts`): the
  copy ability's part visibility (`ftParts_8007487C`) and costume texture
  list (`ftAnim_80070200` into `u.kb.x44`) are set up, as upstream doldecomp
  now does. The imported version left `x44` empty, and Kirby taking
  knockback with such an ability crashed in `ftAnim_80070458`.
- `src/melee/gm/gmonlinemode.c` (`gm_Scene_OnlineLobby_OnFrame`): under
  `TARGET_XBOX` the online/LAN lobby goes straight back to the menu.
  Netplay isn't built, and the lobby formatted the stubs' NULL strings
  (console crash in `snprintf` when LAN play was picked).
- `src/melee/ty/toy.c` (`Toy_8030813C`, `_Toy_8030663C`): under
  `TARGET_XBOX` a trophy id missing from the model table shows the first
  trophy instead of panicking. The gallery panicked on the console with a
  100% save ("Not Found Toy Model!(-15356)": a sort row that was never
  filled); `_Toy_8030663C` logs a `[TOY] sort:` line (trophies owned, rows
  each sort column got, trophy count, language) to find why.
- `src/melee/lb/lbcardgame.c`: under `TARGET_XBOX` the save data and
  name-tag banks go to the card big-endian, from a static copy
  (`card_image`, `toCardOrder`), and are converted to native after a read
  (`fromCardOrder`), both through `xbox/src/sdk/card_endian.c`; older
  little-endian saves are recognised and read as they are
  (`docs/platform.md`, CARD). `lb_803BAB60` (the new save's
  `CardIconInfo`, read by HSD as bytes) is spelled as the GameCube's bytes:
  as `u32`s it gave the Xbox's saves no banner or icon.
- `src/melee/lb/lbsnap.c` (`lbSnap_8001DC0C`): under `TARGET_XBOX` the
  snapshot header's `x14`, which `it_8026C47C` fills natively through a
  plain pointer, is swapped to big-endian like the rest of the header.
- `src/melee/ft/ftparts.c` (`ftParts_80074D7C`): under `TARGET_XBOX` a
  fighter-parts visibility group whose table or index list points outside
  MEM1/ARAM is skipped and logged once per fighter kind (`[WARN] ftParts:`).
  A 4-player Fountain of Dreams match (Pichu, Game & Watch, Ness, Kirby)
  crashed there reading 0x07080900. A group with no DObjs has a NULL index
  list and is legal; the guard took it for a bad pointer and stopped at it,
  leaving the table's later groups visible (Kirby, Samus, Game & Watch:
  black shapes on Kirby's copy hats), so the list is checked only when the
  group has entries.
- `src/melee/ft/fighter.c` (knockback): under `TARGET_XBOX` the first 32
  hits that didn't come from a fighter (or had no source) or whose
  knockback magnitude exceeds 200 are logged (`[WARN] hit:` with the
  fighter, position, damage, angle, element and the source's GObj class and
  player; `[WARN] hit by item kind` for items). On Corneria every fighter
  was at 90% by "Go!" and was launched to a KO on its first landing on the
  Great Fox, on the console only.
- `src/sysdolphin/baselib/dobj.c` (`HSD_DObjDisp`): a DObj whose PObjs
  are all rigid (`POBJ_SKIN` without a joint list) and whose cached display
  lists' boxes (`gx_dl_culled`, gx_vtx.c) lie wholly outside the view is
  skipped before its material setup. The GameCube let the GPU clip them; on
  the Xbox each draw's CPU cost (HSD setup plus submission) sets the frame
  rate. Mute City in xemu: 1391 -> ~525 draws a frame, 5.6 -> 15.5 fps.
  A culled list is never called, so `gx_dl_culled` runs its content hash
  itself once the last check is 16 frames old and stops culling it if the
  contents changed (memory reused for another model).
- `src/sysdolphin/baselib/texp.c` (`HSD_TExpSetReg`): `reg[8]` is static
  (starting white). An RGB-only or alpha-only constant keeps the other half
  of the register from `reg[]`, which HSD never initializes: on the
  GameCube the stack still held the previous call's values, while our
  zero-initialized locals (`-ftrivial-auto-var-init=zero`) made it 0. That
  blacked out material colours (the capsule) and dropped textures blended
  in by a konst alpha (Kirby's Falcon helmet during Falcon Punch, whose K1
  RGB is the glow and K1 alpha the texture blend).
- `src/melee/gm/gmvsmode.c` (`onEnterDebugVs`): `MELEE_DEBUG_VS_CHARS`
  (fighter kinds, costumes, human or CPU), `MELEE_DEBUG_VS_ITEMS` (item mask, top
  frequency) and `MELEE_DEBUG_VS_INVISIBLE` (cloaked players, as Invisible Melee)
  for scripted xemu runs; `src/melee/ft/kinds/ftKirby/ftkirby.c`
  (`ftKb_Init_OnDeath`): `MELEE_DEBUG_KIRBY_HAT` loads that hat's archive
  and spawns Kirby with the copy ability. All under `TARGET_PC`, inert without the variables.
- `src/melee/lb/lbaudio_ax.c` (`lbAudioAx_80027648`): when a needed SSM
  failed to load (bank 2 "buffer overflow") and nothing is pending, bank 2
  is reloaded from empty once, then the missing SSMs are dropped, instead
  of waiting forever (v29: hang entering Jungle Japes after four matches).
- `src/sysdolphin/baselib/synth.c` (`HSD_SynthSFXHeaderLoadCallback`): an
  SSM that overflows its bank is dropped without calling its callback.
  `fn_80026C04` queued the same SSM again, so it overflowed forever on the
  DVD thread and the guard above never ran (v31: hang going from the Data
  menu back to the title). The overflow report gives the bank's use.
  `lbaudio_ax.c` (`lbAudioAx_80023B24`, the sound test) waits for loads in
  flight before emptying bank 2, so a late one can't fill it uncounted.
- `HSD_PREFETCH` (v33, `xbox/include/game/xbox_game_prelude.h`): cache-line
  prefetches of the next nodes in HSD's list walks: `jobj.c`
  (`JObjAnimAll`, `HSD_JObjDispAll`), `dobj.c` (`HSD_DObjAnimAll`,
  `HSD_DObjDisp`), `pobj.c` (`HSD_PObjAnimAll`), `mobj.c` (`HSD_MObjAnim`),
  `tobj.c` (`HSD_TObjAnimAll`), `fobj.c` (`HSD_FObjInterpretAnimAll`),
  `aobj.c` (`HSD_AObjInterpretAnim`), `displayfunc.c` (`HSD_JObjDispDObj`'s
  DObj loop). Hints only: no result changes, the simulation stays bit for
  bit. v33's console profile had the simulation's animation pass stalling on
  the first load from each node (128 KB L2, no hardware prefetcher).
- `src/sysdolphin/baselib/pobj.c` (`SetupEnvelopeModelMtx`) and `dobj.c`
  (`HSD_DObjDisp`), v35: within one DObj, an envelope (same joints and
  weights, same view matrix and setup flags) that an earlier PObj already
  set up reuses its position and normal matrices instead of blending,
  concatenating and inverting again (~1250 envelope matrices a frame in a
  4-CPU match, ~630 distinct). The same bits and the same GX loads; the memo
  is cleared before each DObj's PObjs, where no joint can move.
- `src/melee/gr/grbigblue.c` (`grBb_YakumonoParam`), v36: the stage's
  parameters from the disc are `DISC_STRUCT` (and `x134_translate` a
  `DiscVec3`). Every other stage's parameter struct already was; this one
  was read byte-swapped, the platforms' x came out near -4e8 and
  `lbVector_WorldToScreen`'s range assert stopped the console on the first
  frame of a Big Blue match.
- `src/melee/gm/gmtitle.c` (`gm_Scene_Title_OnFrame`), after v2: calls
  `xsdk_menu_title_frame` (the Melee-X settings menu, BACK) first each
  frame, under `TARGET_XBOX`; while the menu is up it returns early and
  holds the attract timer at 0.

Game files are compiled with `-Werror=implicit-function-declaration`. The
prelude renames `acosf`, `atan2f`, `asinf`, `expf` and `powf` after
`<math.h>` is already in, so their prototypes have to be in the prelude.
Without them, every call read its float result from EAX. That gave
garbage angles, and the sword afterimage then overran its stack buffer.

To sync a newer melee-pc:
1. Copy its `src/melee`, `src/sysdolphin`, `src/pc` and the headers again.
2. Re-apply the `PORT:` edits.
3. Update `src/UPSTREAM_COMMIT`.
4. Regenerate `xbox/src/sdk/stubs.c` if the link reports new `pc_*` hooks.
5. Run the lowering tests.

## Known risks

- **Hardware coverage.** VS matches on many stages (Pokémon Stadium with
  transformations, Fountain of Dreams, Green Greens, Mute City, Onett,
  Corneria, Big Blue and more), menus, the Trophy Collection, saves and a
  100% save run on the console, over 30-minute sessions. Single-player
  modes and many stages have seen less play. A stage-specific crash has so
  far meant a disc struct missing `DISC_STRUCT` (Big Blue, v35). xemu
  differs from the console in ways that hid real bugs (w-buffer, PFIFO
  timing, AC97): check rendering on the console, not only in xemu.
- **Performance.** Matches draw 30-60 fps on the console (busy 4-CPU
  stages in the low 30s) while the simulation keeps 60 ticks
  (`docs/roadmap.md`).
- **Rendering gaps**: indirect texturing only as BUMPENVMAP does it (the
  cloak's refraction: offsets to half a step, s/q per vertex; checked in
  xemu, whose BUMPENVMAP reads unsigned textures as two's complement: the
  console is unchecked), fog
  per vertex (long polygons get less fog mid-span; no range adjustment),
  TEV swap-table permutations; the texture pool and the
  display-list vertex pool run full in long sessions (they evict and
  rebuild; an overflow texture pool takes free RAM for a scene whose
  working set outgrows it).
- **Netplay** is not built; the online/LAN lobby returns to the menu.
- **The demand-commit fault handler** relies on the Xbox kernel sending
  kernel-mode access violations on reserved memory to the thread's SEH
  chain. Every console boot depends on it (MEM1 and ARAM commit this way)
  and it has held since the first hardware builds; a fault at raised IRQL
  still can't be handled (`docs/testing.md`, Crashes).
