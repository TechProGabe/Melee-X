# Renderer: GX on the NV2A

The game calls the Dolphin GX API (aurora's headers). `xbox/src/sdk/gx/`
implements it as a front end that keeps an `XgxState` (`xbox/include/xgx.h`)
current and decodes vertices. `xbox/src/hw/nv2a.c` is the back end: at every
draw it diffs that state against what the GPU already has and pushes only
the difference through pbkit.

```
game -> GX calls -> gx_state.c   (XgxState, dirty bits)
                    gx_vtx.c     (immediate mode + display lists -> canonical vertices)
                    gx_tex.c     (texture objects, TLUTs, decode cache)
                    gx_copy.c    (EFB copies, GXCopyDisp = end of frame)
                          |
                       xgx.h     (plain scalars and byte arrays only)
                          |
                    nv2a.c       (state diff, pushbuffer, texture pool, vertex ring)
                    nv2a_vp.c    (vertex programs generated from the transform state)
                    nv2a_rc.c    (TEV -> register combiners)
                    nv2a_fog.c   (GX fog -> vertex-program fog, final combiner)
```

Per draw, the back end rebuilds only what the front end's dirty groups
say changed: the combiner setup and texture units (TEV, MAPS), the
vertex-program key (CHANS, TEXGEN, LIGHTS, and whether the layout has
normals and colours), texgen constants (TEXGEN, TEXMTX, or POSMTX when a
texgen reads a position matrix), fixed pixel state (PIXEL, SCISSOR, FOG) and
combiner constants (TEVREG). `s_draw_force` rebuilds everything after a GPU
state reset or a content-rect change. So every GX setter must mark its
group; `gx_state.c`'s all do. A setter whose values equal the current state
returns without flushing or marking anything: HSD re-sets the whole TEV,
channel and pixel state for every material, and those redundant calls used
to make most draws rebuild and re-hash the combiner setup. The back end also
skips re-sending a combiner program it sent last (by cache slot, not by
comparing the program) and the inline white vertex colour while it is still
there.

memcpy, memmove, memset and memcmp are compiler builtins everywhere
(`xbox/include/xbuiltin.h`, force-included): nxdk and the game build with
`-ffreestanding`, which made every fixed-size copy in the decoder and HSD a
real call. That was ~18% of a match frame on the console.

Display lists are cached (`gx_vtx.c`, up to 2048 lists): the first call
decodes the list into a vertex buffer from its own pool (4 MB at 480, 3 MB
at 720p; with every list cached, a 4-CPU match fills 3 MB) and later calls replay the draws. Entries are keyed by the
list's address and size and by the vertex descriptor and the formats (VAT)
the list uses: HSD draws some small lists under different formats (shared
by models quantized differently), and one entry per list flipped between
them until ~100 lists were decoded on every call. Now a 4-CPU match keeps
~1400 lists cached, none volatile. An entry is checked against the
vertex descriptor, formats and arrays on every call, and against a sampled
hash of the list and of the array ranges it indexed once a frame. A list
whose arrays keep changing (skinned and morphed models, whose positions and
normals HSD rewrites or re-points every frame) goes dynamic after four
rebuilds: its decode plan, every vertex's indices and a decoded template in
cached RAM are kept (1 MB for all of them). Each call re-fetches only the
attributes whose array moved or whose sampled hash changed (once one has
changed it is fetched on every call), then copies the template into the
vertex ring in one sequential write. Lists that don't fit the budget are
decoded every call (volatile). `[DLC]` lines report both.

Draws are merged where the state allows, because a draw costs about the
same whatever its size. In xemu on macOS it costs most: xemu's GL renderer
sends every non-point draw through a geometry shader, and macOS's GL runs a
geometry shader as a compute pass that ends the render pass, so each draw
was a render pass of its own (~75 µs of emulation, ~90% of a match frame).

- The batches of one display list share its material, so consecutive ones
  with the same vertex layout and primitive become one draw when the list is
  decoded: triangle lists run on, strips are stitched with degenerate
  triangles (the last vertex again, then the next strip's first once or
  twice so it starts on an even vertex and keeps its winding). Dynamic lists
  merge the same way when their template is copied out.
- Immediate-mode vertices (`GXBegin`..`GXEnd`) are built in cached memory and
  copied to the write-combined ring when drawn. A batch of a list primitive
  that got all its vertices isn't drawn at `GXEnd`: it waits, and the next
  `GXBegin` with the same primitive and layout continues it. Anything that
  changes state flushes it first: every setter that changes a value,
  `GXLoadTexObj`/`GXLoadTlut` when the binding changes, display-list calls,
  copies and draw-done. Vertex descriptor, format and array setters only
  close an open batch: a finished one already holds its vertices.
- Quads and fans are sent as triangle lists (`out_prim` in `gx_vtx.c`), and
  the EFB-copy quad as a strip.
- Array offsets point at the start of the 32768-vertex window of the vertex
  ring or the vertex pool that the draw starts in, and each draw starts at
  its first vertex's index from there (both place draws at a multiple of
  their stride). Consecutive draws of one layout with nothing else changed
  then have no methods between them, which xemu joins into one draw and
  which saves the console the offset writes. The windows are there because
  the console takes vertex indices up to 0xFFFF only: a larger
  `DRAW_ARRAYS` start raises a PGRAPH data error per draw (xemu doesn't
  check), and joined draws stop at 32768 vertices (`JOIN_MAX`).

Per-draw bookkeeping in the back end stays small: the vertex-program cache
compares a key hash before the key, the combiner config is zeroed, hashed and
compared only up to its used stages (`rc_used`), and dirty constant rows are
compared as words inline.

`[NV2A]` lines count draws by primitive and by what changed before each
(`changed nothing`, `only a position matrix`, then per dirty group); the
`[DLC]` line counts joined batches and names the calls that drew a waiting
immediate batch.

`xgx.h` is compiled by both triples (game and nxdk), so its structs hold only
32-bit scalars, floats and byte arrays: no bit-fields, no 64-bit members.

The GPU counts as idle (`wait_idle`) only once the pusher has caught up,
PFIFO's CACHE1 is empty, the pusher has stopped and PGRAPH is idle, seen
twice in a row. pbkit's `pb_busy` checks the first and last only, so
methods still in CACHE1 passed as done whenever PGRAPH was between two of
them. Deferred frees, the vertex ring's restart at every frame and EFB
copy targets all rely on this wait. xemu runs methods as they arrive and
can't show the difference.

The pushbuffer is 1 MB. pbkit's `pb_size` takes powers of two only and
silently keeps its 512 KB default otherwise: the 1.5 MB asked for until
v11 left `PB_GUARD` (restart at the head when a frame gets within 192 KB of
the end) beyond the real end, a Pokémon Stadium frame ran past it into the
memory after the pushbuffer, and the GPU fetched texture data as methods
(DMA pusher error, `GPU fault kind 2`, frozen). `[NV2A] frame` lines report
the interval's peak and mid-frame restarts.

## Frame and output geometry

- The logical EFB is the GameCube's 640x480. The back end maps it onto a
  *content rect* of the framebuffer: all of 1280x720 at 720p (16:9), all of
  640x480, or a pillarboxed rect when a scene asks for the GameCube's own
  73:60 picture (`xgx_set_content_aspect`).
- melee-pc's `widescreen.c` handles hor+ widescreen above this: it widens
  the camera and anchors the HUD. It asks for the render size through
  `AuroraGetRenderSize` and `AuroraSetPresentationAspect`, and the SDK side
  answers them from `xgx_content_size`.
- **The flip is `GXCopyDisp`**, where a GameCube frame ends: the EFB is
  copied to the XFB and cleared. `VIWaitForRetrace` only paces the game to
  60.000 Hz and runs the alarms (`vi.c`).
- Framebuffers: 640x480x32 + Z24S8, or 1280x720 R5G6B5 + Z16 at 720p
  (32-bit colour doesn't fit in 64 MB next to the game). `pb_DepthFmt` is
  made settable by `tools/xbox/patch_pbkit.py`.

## Vertex programs (`nv2a_vp.c`)

Each transform/lighting/texgen configuration (`VpKey`) gets a generated
program, cached and kept resident. The instruction encoder matches
[nv2a-vsh](https://pypi.org/project/nv2a-vsh/) bit for bit
(`tools/xbox/test_vp_encoder.py`). Details that took some work to get right:

- relative addressing is `c[A0+N]`;
- the hardware output and write masks are bit-reversed relative to `xyzw`;
- the MAC `ADD` takes its operands from slots A and C, and ILU ops (RCP,
  RSQ, LIT, ...) take theirs from slot C.

Constant rows:

| rows | contents |
|---|---|
| 0-3 `VPC_PROJ` | projection, with the viewport, content rect and depth range folded in |
| 4 `VPC_K` | (0, 1, 0.5, 2) |
| 6-35 `VPC_POS` | 10 position matrices x 3 rows, indexed by `a0` = PNMTXIDX |
| 36-65 `VPC_NRM` | 10 normal matrices x 3 rows, same indexing |
| 66-69 `VPC_CHAN` | material 0, ambient 0, material 1, ambient 1 |
| 70-109 `VPC_LIGHT` | 8 lights x 5 rows: position, direction, colour, angle attenuation, distance attenuation |
| 110-121 `VPC_TEXGEN` | 4 units x 3 rows (s, t, q), post matrix folded in when there is no normalize |
| 122-133 `VPC_POSTMTX` | 4 units x 3 rows, the post-transform matrix after normalize |
| 134-136 `VPC_FOG` | fog numerator, denominator and curve (see Fog) |

Inputs: v0 position, v1 matrix index, v2 normal, v3/v4 colours 0/1, GX
TEX0..6 -> v9..v15, TEX7 -> v8. The front end writes canonical vertices
(`XgxLayout`): float positions, normals and texcoords, RGBA8 colours, and the
matrix index as a float.

Skinning: GX selects a position matrix per vertex (PNMTXIDX, 0..27 in steps
of 3). The program loads it into `a0` and reads `c[A0+6]`, so a whole
skinned mesh is one draw.

### Depth

GX clip-space z/w runs from -1 (near) to 0 (far), and the depth written is
`z/w * (far - near) + far`. The NV2A takes screen-space z directly, so row 2
of the projection is replaced by

```
row2 = ZMAX * ((vf - vn) * P[2] + vf * P[3])
```

where `vn`/`vf` are the viewport depth range and ZMAX is 2^24-1 (Z24) or
65535 (Z16 at 720p).

This is a z-buffer: CONTROL0's `Z_PERSPECTIVE_ENABLE` (w-buffering) must
stay off. pbkit's `pb_target_back_buffer` turns it on ("We use W") each
time it targets the back buffer, so `frame_open` sets CONTROL0 again right
after. With the w-buffer the console took depth from the interpolated w
and Pokémon Stadium's floor showed black bands that moved with the
camera. With the z-buffer really in use, xemu showed the same bands: the
floor's pixels got depth just outside the clip range and were culled
(`ZMIN_MAX_CONTROL` CULL_NEAR_FAR). Depth is now clamped to the range
(ZCLAMP_CLAMP), as the GameCube's 24-bit depth is; `-DXGX_DEPTH_CULL=1`
restores culling. Geometry behind the eye is still clipped on w. Rows 0 and 1 fold in the viewport scale and offset,
with y flipped.

### Culling

The viewport y-flip is folded into the projection, and GX's front faces then
come out clockwise, so the front face is CW. (OpenCrossing's GL shim uses
CCW; its projection differs. Checked in xemu against Dolphin on the memory
card screen, whose panels use `GX_CULL_BACK`.) `GX_CULL_BACK` maps to 0x405 (back),
`GX_CULL_FRONT` to 0x404 and `GX_CULL_ALL` to 0x408 (front and back).

## TEV -> register combiners (`nv2a_rc.c`)

This comes from OpenCrossing-Xbox's compiler, generalized. There are up to
8 combiner stages and 4 texture units. The TEV konst colours and registers
become per-draw combiner constants (`RREF_*`). When a TEV configuration
can't be represented exactly, it is approximated and counted. The
`[NV2A] frame N: D draws (A approximated)` log line reports that count
every 600 frames (10 seconds).

Constants are unsigned, and reading PREV back clamps at 0. So the signed,
unclamped arithmetic of Nintendo's THP YUV -> RGB recipe (used for
`sobjlib.c`'s movie sprite) is recognised as a whole. It is replaced by a
three-stage program that does the same maths with signed registers and
fixed constants (`RREF_FIXED`).

Current limits:

- signed TEV colours (`GXSetTevColorS10` below 0) and unclamped stages
  work only in the movie recipe;
- TEV swap tables: only the alpha broadcast (`AAAA`) is supported;
- indirect texturing is ignored (some stage effects);
- destination alpha is missing at 720p (R5G6B5 has no alpha);
- texture-matrix index attributes (TEXnMTXIDX) are ignored.

## Fog (`nv2a_fog.c`)

GX applies fog after the last TEV stage, to the colour only:
`rgb = (rgb * (256 - F256) + fog * F256) >> 8` with F256 = F * 256. The
fog amount F comes from the pixel's 24-bit EFB depth Zs and the registers
`GXSetFog` writes: A and C cut to 11 mantissa bits, and for perspective fog
B as a 24-bit magnitude and a shift (libogc `GX_SetFog`, Dolphin's
`PixelShaderGen`):

| fog | per pixel |
|---|---|
| perspective (`GX_FOG_PERSP_*`) | `ze = A * 2^24 / (b_mag - (Zs >> b_shift))`: eye depth / (end - start) when the fog's near and far are the projection's |
| orthographic (`GX_FOG_ORTHO_*`) | `ze = A * Zs / 2^24` |
| all | `x = clamp(ze - C, 0, 1)`; LIN `F = x`, EXP `1 - 2^(-8x)`, EXP2 `1 - 2^(-8x^2)`, REVEXP `2^(-8(1-x))`, REVEXP2 `2^(-8(1-x)^2)`; `GX_FOG_NONE`: off |

The NV2A has no per-pixel depth fog. Its fog unit takes oFog.x from the
vertex program per vertex and interpolates the factor (xemu evaluates it
per vertex; `FOG_PARAMS` also goes to the transform engine). So:

- **Vertex program.** F is computed per vertex from the vertex's own depth
  with the same registers. Zs / 2^24 is `(a . P) / (w . P)` for the
  view-space position P and the projection's depth and w rows (`VPC_PROJ`
  + 2 and + 3), so `ze - C` is a ratio of two linear functions of P: rows
  `VPC_FOG` and `VPC_FOG + 1`, built by `fog_setup` when the fog or the
  projection changes. EXP and EXP2 clamp x and evaluate `2^(-8u)` with
  `expp` (the REV types use 1 - x; the curve's constants are in
  `VPC_FOG + 2`). `VpKey.fog` selects the variant (off, linear, exp,
  exp2); fog makes a program 4 (LIN) to 10 (EXP2) instructions longer, so
  a program already near the NV2A's 136 drops lights sooner.
- **Fog unit.** LINEAR, `FOG_PARAMS` (1, 1, 0): the linear mode computes
  `p0 + p1 * oFog - 1` (nxdk's NV10 notes, xemu's `fogFactor -= 1.0`), so
  the factor is F, clamped to 0..1 per pixel. nxdk_pgraph_tests' vertex
  shader fog tests use the same setup; they also found that the gen mode
  doesn't matter with a vertex program. The NV2A's EXP modes aren't used:
  they are evaluated per vertex as well, and EXP's zero point measures
  ~1.51 on hardware where the formula says 1.5.
- **Final combiner.** CW0 becomes A = FOG.a (the factor), B = FOG.rgb (the
  fog colour, `SET_FOG_COLOR` is ABGR), C = PREV, D = 0; alpha (CW1's G)
  is untouched. Without fog CW0 is what `nv2a_rc.c` compiled, and
  unfogged vertex programs are the same as before (checked on 200000
  random keys).
- **State.** Fog is sent only when `GXSetFog` changed something
  (`XGX_DIRTY_FOG`; `GXSetFog` returns early on equal values like the other
  setters, as HSD calls it for every camera pass and particle kind) or the
  projection changed while fog is on.

Accuracy (`tools/xbox/test_fog.py`, against libogc's registers and
Dolphin's formula): at every vertex F is within 1/255 of what GX gives
within one of its own depth steps. With Melee's 0.1..16384 game camera GX
resolves fog coarsely: `b_mag - (Zs >> 2)` is 99 steps at depth 5000 and 41
at the far plane, and b_mag rounds B up (8388638 / 2^23), which makes GX's
ze up to 1.57 times smaller at depth than the textbook formula (thinner
fog). The port reproduces both at the vertices. Between vertices the NV2A
interpolates F linearly in view space, which is exact for LIN fog while
b_mag's rounding is negligible (the test checks edges for most cameras).
With Melee's camera GX's denominator grows by ~57% from near to far, so a
polygon that spans a long depth range gets less fog mid-span than on the
GameCube: up to ~23/255 on an edge from depth 2000 to 14000 through a
narrow fog range. The EXP curves are also linear between vertices.

`GXSetFogRangeAdj` (a horizontal correction toward radial distance, which
HSD sets when a fog has a FogAdj descriptor) is not applied.

Melee sets fog in these places (`HSD_FogSet`), all compared best against
Dolphin:

- stages: `Ground_801C1E94` loads the stage's fog descriptor (scaled by the
  stage's `param->y`; its colour is also the clear colour) and
  `Ground_801C1E2C` applies it for the stage's camera passes while
  `stage_info.unk8C.b2` holds (set at stage load; Final Destination's
  `grlast.c` clears it). The camera turns it off for its later passes
  (`camera.c`);
- particles whose kind has `DispFog` (`psdisp.c`);
- the title screen (`ScTitle_fog`), the Classic and All-Star intros
  (`gm_1832.c`, `gm_186E.c`), Tournament mode (`gmtou_*.c`), the 1P ending
  (`gmregenddisp.c`), the staff roll, Adventure cutscenes (`vi0102`,
  `vi0401`, `vi0501`, `vi0502`, `vi0801`, `vi1201v1`, `vi1201v2`) and the
  trophy display (`ScMenDisplay_fog`, `tydisplay.c`; `toy.c`'s own white
  fog is a debug-ROM feature).

## Textures (`gx_tex.c`, `nv2a.c`)

- GX textures are converted once and cached, keyed by data pointer, size,
  format, palette and mip count. A sampled hash revalidates them at most
  once a frame, because HSD reuses archive memory.
- For power-of-two sizes the NV2A's own formats are used: CMPR -> DXT1,
  I4/I8 -> AY8, IA4/IA8 -> A8Y8, RGB565 -> R5G6B5, C4/C8 -> I8 indices with
  a 256-entry A8R8G8B8 palette (the TLUT decoded; it sits at the start of
  the texture's pool allocation, `SET_TEXTURE_PALETTE`). Everything else
  becomes A8R8G8B8; non-power-of-two images are resampled to the next power
  of two. On Pokémon Stadium, C8 as A8R8G8B8 took 2.1 MB of the pool. A
  texture unit is re-sent whenever a different texture binds, even one made
  at a freed texture's address with the same format and size, so a P8
  palette is always loaded again (xemu reads palettes at each draw).
  `docs/architecture.md` has the table and `tools/xbox/test_tex_convert.py`
  the checks.
- CMPR -> DXT1 conversion:
  - GX stores 8x8 tiles, each holding four DXT1 blocks (TL, TR, BL, BR),
    and pads small mip levels to a full tile; they are reordered into
    DXT1's 4x4 block rows;
  - both colours are byte-swapped;
  - each index byte has its 2-bit fields reversed (GX puts pixel 0 in bits
    7-6, DXT1 in bits 1-0).
- The texture pool is contiguous memory: 8 MB at 480, 6 MB at 720p (it was
  6 / 5 MB and ran out on the console at the start of a match; init falls
  back 1 MB at a time to 4 MB, `-DXGX_TEX_POOL_KB=<n>` sets it). When it is
  full, `gx_tex_make_room` evicts least recently used cache entries until
  about the needed size is released, then the GPU is waited on once and the
  allocation retried; each round frees twice as much, since the pool
  fragments. Textures bound to a texture map are never evicted, and a
  texture drawn this frame only goes when nothing older is left. EFB copies
  are evicted too (a stale one would otherwise pin the pool: the attract demo
  copies to a new address every frame), but among the older entries they
  count as 60 frames younger, since they can't be rebuilt from memory. When
  nothing is left to evict the texture is dropped for that draw (drawn
  untextured, usually black) instead of waiting forever. Drops are counted
  in the `[TEX]` line, the first of each interval gets a `[TEX] drop:` line
  (size, pool free, largest free block), and `-DXGX_DEBUG_MAGENTA` draws
  them magenta.
- Cache lookups go through a hash of the data pointer. Binding the object a
  texture map already holds this frame skips the lookup entirely. Textures
  are revalidated (sampled hash) once a frame; `GXInvalidateTexAll`, which
  HSD calls after each of its four shadow copies, no longer forces another
  round.
- EFB copies (`GXCopyTex`) are drawn by the GPU (`efb_copy_gpu`): the back
  buffer, bound as a linear texture, is drawn with one quad into the
  destination texture as a swizzled render target (pbkit's DMA object 3,
  which spans all of RAM, as the colour context). The cache registers the
  texture under the copy's destination pointer. The combiners keep the
  channels the copy format stores: the shadow maps are `GX_CTF_R4`, sampled
  as I4, so red goes to every channel. `WAIT_FOR_IDLE` on both sides orders
  the copy against the draws before and after it; the CPU never waits.
  Afterwards the back buffer is the target again
  (`ocx_pb_retarget_back_buffer`, added by `tools/xbox/patch_pbkit.py`: it
  re-sends the surface state only, where `pb_target_back_buffer` rewrites
  DMA object 9 through four GPU-to-CPU interrupts) and every state group is
  re-sent. The copy's texture is the nearest power of two per side, not
  the next one (`copy_dim`), filtered linearly when that is smaller than
  the source: Pokémon Stadium's screen copies 640x406, which was 1024x512
  ARGB8 (2 MB of the pool) and is now 512x512. The CPU readback
  (`-DXGX_EFB_GPU_COPY=0`, and 720p, whose 16-bit
  depth buffer can't pair with a 32-bit texture target) cost ~8 ms per
  256x256 shadow map on the console: the framebuffer is write-combined, so
  each read is an uncached bus cycle. Reading it through 0x80000000 |
  physical does not help: contiguous memory already lives there, and the
  write-combine attribute is on those same page-table entries.
- Levels are swizzled into a cached buffer and then copied in order:
  swizzled stores straight into write-combined texture memory defeat write
  combining.
- Movie frames (`xbox/src/sdk/thp.c`) are baseline JPEGs without byte
  stuffing. They are decoded MCU by MCU, with stb_image's IDCT, straight
  into the game's I8-tiled Y/Cb/Cr planes. The TEV then converts them to
  RGB.
- Immediate mode takes position components as a stream when the format is
  XYZ: HSD's shadow code writes its background quad as 12 floats in six
  `GXPosition2f32` calls. Taking each call as a vertex left the shadow maps
  black, and the stages that multiply them in (Mute City's road) went black.
- Known wrong in xemu: Mute City's distant skyline band renders as white
  speckle.
