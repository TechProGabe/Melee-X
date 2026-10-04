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
                    nv2a_vpmem.c (which programs are resident in program memory)
                    nv2a_rc.c    (TEV -> register combiners)
                    nv2a_fog.c   (GX fog -> vertex-program fog, final combiner)
```

`xgx.h` is compiled by both triples (game and nxdk), so its structs hold only
32-bit scalars, floats and byte arrays: no bit-fields, no 64-bit members.
memcpy, memmove, memset and memcmp are compiler builtins everywhere
(`xbox/include/xbuiltin.h`, force-included), since `-ffreestanding` would
make every fixed-size copy a call.

## State and dirty groups

Per draw, the back end rebuilds only what the front end's dirty groups say
changed: combiners and texture units (TEV, MAPS), the vertex-program key
(CHANS, TEXGEN, LIGHTS, and whether the layout has normals and colours),
texgen constants (TEXGEN, TEXMTX, or POSMTX when a texgen reads a position
matrix), fixed pixel state (PIXEL, SCISSOR, FOG) and combiner constants
(TEVREG). `s_draw_force` rebuilds everything after a GPU state reset or a
content-rect change. So every GX setter must mark its group. HSD re-sets
the whole TEV, channel and pixel state for every material, so redundant
state is filtered at every level:

- A setter whose values equal the current state returns without flushing
  or marking anything (matrix loads compare 12 words). `gx_tex_bind` marks
  the maps dirty only when the bound texture or its sampling changed.
- A texture unit built from the same inputs as the registers it last sent
  (handle, wrap, filters, LOD bias, unit kind, and an epoch bumped by every
  texture made or freed) skips the build (`emit_textures`); a draw that only
  rebinds the same set of maps keeps the derived units and combiner program.
- Combiner constants (`pack_const`) are rebuilt only when the program is
  sent or a TEV/konst colour changed; the program sent last (by cache slot)
  and the inline white vertex colour aren't re-sent.
- Light rows are built only for lights an enabled channel uses. The caches
  compare a key hash before the key; the combiner config is hashed only up
  to its used stages (`rc_used`).

## Vertices and draws

### Display-list cache (`gx_vtx.c`)

HSD's display lists are model data, so up to 2048 are cached: the first
call decodes the list into a vertex buffer from its own pool (4 MB) and
later calls replay the draws. A list may hold up to 4096 draws
(`DLC_MAX_BATCH`; Fountain of Dreams' stage is one 104 KB list of more than
512).

- **Key**: the list's address and size, the vertex descriptor and the VAT of
  the formats it uses (`fmt_key`), counting only attributes the descriptor
  has on (HSD changes the others freely). The vertex setters keep
  order-independent sums per setting, so `fmt_key`/`vtx_sig` are cheap.
- **Revalidation**: every call checks descriptor, formats and arrays
  (`vtx_sig`); once a frame a sampled hash of the list and its array ranges
  too (HSD reuses memory); after 120 passes (`DLC_STABLE`) every fourth
  frame, staggered, with 16 words (`content_qhash`). A list rewritten in
  place can show stale for up to three frames.
- **Dynamic lists**: a list whose arrays keep changing (skinned and morphed
  models) goes dynamic after four rebuilds: its decode plan, vertex indices
  and a decoded template are kept in cached RAM (1 MB for all). Each call
  re-fetches only attributes whose array moved or changed, then copies the
  template into the ring in one sequential write. Lists over the budget are
  decoded every call (volatile). `[DLC]` reports both; `[DLC] uncached:`
  names lists that could not be cached and why.
- **Eviction**: the least recently used list not drawn this frame, its
  buffer freed at once: `frame_open` (the frame's first GPU use) has waited
  for the GPU, so nothing reads it (a free before that opens the frame).

### Off-screen culling

The cache keeps each list's model-space box (lists without per-vertex
matrices); `HSD_DObjDisp` skips rigid PObjs wholly outside the view
(`gx_dl_culled`: the transformed centre's view-aligned box against five
planes made once per projection, `gx_cull.h`; never culls more than an
eight-corner test). A list is known after its first draw.

### Fewer, bigger draws

A draw costs about the same whatever its size (most in xemu on macOS, where
each one became a render pass), so draws are merged where state allows:

- Consecutive batches of one display list with the same layout and
  primitive become one draw when the list is decoded: triangle lists run
  on, strips are stitched with degenerate triangles (the last vertex again,
  then the next strip's first once or twice so it starts on an even vertex
  and keeps its winding). Dynamic lists merge the same way.
- Immediate-mode vertices (`GXBegin`..`GXEnd`) are built in cached memory
  and copied to the write-combined ring when drawn. A complete batch of a
  list primitive waits at `GXEnd` for the next `GXBegin` with the same
  primitive and layout. Any state change flushes it first; vertex
  descriptor, format and array setters only close an open batch.
- Quads and fans are sent as triangle lists (`out_prim`), the EFB-copy quad
  as a strip.
- Array offsets point at the start of the 32768-vertex window the draw
  starts in, and each draw starts at its index from there, so consecutive
  draws of one layout have no methods between them (xemu joins them). The
  console takes vertex indices up to 0xFFFF only (a larger `DRAW_ARRAYS`
  start raises a PGRAPH data error; xemu doesn't check), hence the windows
  and `JOIN_MAX`.

Immediate mode takes position components as a stream when the format is
XYZ (HSD's shadow quad is six `GXPosition2f32` calls); with no texture
coordinate in the vertex, `GXTexCoord1f32` feeds it too (`fn_80185408`).
The raw writers follow the vertex as the FIFO does: `GXTexCoord1f32` is
the next float of whatever attribute comes next (a position component, or
S then T of an F32 texture coordinate), and `GXCmd1u8` is the index of an
`INDEX8` attribute when one comes next (otherwise a matrix index). HSD's
particles (`psdisp.c`) write billboard positions as three
`GXTexCoord1f32` and the TEX0 corner index as a `GXCmd1u8`; read as whole
texture coordinates and matrix indices they never completed a vertex, and
every textured particle was dropped (issue #7, v53). `[DLC]`'s `short`
counts batches that ended with fewer vertices than `GXBegin` announced.
`[NV2A]` lines count draws by primitive and by what changed before each.

### Pushbuffer and GPU sync

- The pushbuffer is 1 MB (`pb_size` takes powers of two only and silently
  keeps 512 KB otherwise, putting `PB_GUARD` past the end), kicked every
  8192 words (`XGX_PB_KICK`; each kick flushes the write-combine cache).
- Every batch starts with `BREAK_VERTEX_BUFFER_CACHE`: the NV2A caches
  vertex data by address and reads ahead of a draw's last vertex, so a
  kicked batch can leave stale read-ahead for vertices written right behind
  it in the ring (xemu has no such cache). The EFB copy quad and the
  Z-texture mask send it again right before their draw.
- The GPU counts as idle (`wait_idle`) only once the pusher has caught up,
  CACHE1 is empty, the pusher has stopped and PGRAPH is idle, twice in a
  row (pbkit's `pb_busy` misses methods in CACHE1). Deferred frees, the
  ring's restart and EFB copy targets rely on it.
- **CPU/GPU overlap** (`XGX_OVERLAP`, default 1): `xgx_present` queues the
  frame-rate counter and pbkit's flip (triple buffering) and returns; the
  wait for idle is in `frame_open`, the next frame's first GPU use, where
  deferred frees are released and ring and pushbuffer restart.
  `GXCopyDisp`'s clear is queued until then (`frame_open`'s own full clear
  is skipped when it covers everything). Screenshot frames and the title's
  settings menu (`xgx_set_overlay`, CPU writes) still wait in the present.

### CPU cost of the back end

Besides the redundant-state filtering and sampled revalidation above:
texture-cache entries keep what a bind reads in one aligned 32-byte line
(hashes in a second array), and pool blocks sit in address order with free
blocks on their own list and allocated ones hashed by offset (small
requests take the lowest free block that fits, big ones the top of the
highest). Measurements and history: `fps-plan.md`, `decisions.md` (also
the game-side HSD matrix work).

Probes (`fps-plan.md`): `-DXGX_CENSUS=1` counts draws by pass and owner
(`xgx_census_tag`); the probe build's (`-DXHW_PMC=1`) ablation windows 4, 5,
8 and 9 skip `xgx_draw`, display-list rechecks, the fill, and EFB copies;
`[GPUP]` lines time the GPU's frame.

## Frame and output geometry

- The logical EFB is the GameCube's 640x480, mapped onto a *content rect*
  of the framebuffer: all of 1280x720 at 720p, all of 640x480, or a
  pillarboxed rect when a scene asks for the GameCube's 73:60 picture
  (`xgx_set_content_aspect`). melee-pc's `widescreen.c` does hor+ above
  this through `AuroraGetRenderSize`/`AuroraSetPresentationAspect`,
  answered from `xgx_content_size`.
- **The flip is `GXCopyDisp`**, where a GameCube frame ends: the EFB is
  copied to the XFB and cleared. `VIWaitForRetrace` only paces the game to
  60.000 Hz and runs the alarms (`vi.c`).
- Framebuffers: 640x480x32 + Z24S8, or 1280x720 R5G6B5 + Z24S8 at 720p
  (32-bit colour doesn't fit in 64 MB; Z16 until v53 and with
  `-DXGX_Z24_16BPP=0`, see Depth; `pb_DepthFmt` made settable by
  `patch_pbkit.py`). 16-bit colour paths key on the bpp, depth paths on
  the depth format, so `-DXHW_VIDEO_480_BPP=16` runs 480 as 720p does
  (R5G6B5 + Z24S8, or + Z16 with `env MX_Z24=0`).
  Clear colours go to `pb_fill` as A8R8G8B8, which converts them.

### Tile regions

The NV2A's memory controller has eight tile regions (`NV_PFB_TILE`, copied
in PGRAPH and its RDI) that lay a surface out so a 2D block falls into one
DRAM page; the same for every client, so no pixel changes. Z compression
(`NV_PFB_ZCOMP`) is lossless too, but a compressed buffer can't be read by
the CPU or sampled as a texture (see Z textures).

`pb_init` puts tile 0 over the three framebuffers and tile 1 (compressed)
over the depth buffer, pitch 2560, but writes the base word as
`base | 2 | (flags & 1)`, while bit 0 is the enable (envytools, nouveau,
xemu), and sets the Z24S8 format bit for a Z16 buffer too (right for the
Z24S8 buffer 720p has since v53).
`-DXGX_TILE=<bits>` (`env MX_TILE=`) re-programs them in `tiles_setup` at
boot: 1 the Z16 format, 2 tile 0 as `base | 1`, 4 tile 0 as `base | 3`, 8 no
Z compression; 0 leaves pbkit's setup. Default 4: console rounds
(`fps-plan.md`) found tiled framebuffers faster at 720p with the same
picture. `-DOCX_Z16_TILE_FLAGS` sets the Z16 tile's flags at build time.

With tile 0 on, the CPU must not touch a framebuffer at pbkit's 0x8xxxxxxx
address (it bypasses the tile); `fb_cpu` hands out the NV2A's aperture
(BAR1, 0xF0000000 + physical) instead, for the settings menu, screenshots
and `read_rect_cpu`. A tile is only changed when its pitch is the surface's
(not under `-DXHW_VIDEO_480_BPP=16`). `[NV2A] tiles` at boot reads them
back. xemu ignores tile regions.

## Vertex programs (`nv2a_vp.c`)

Each transform/lighting/texgen configuration (`VpKey`) gets a generated
program, cached by key (64 entries, `VPM_PROGS`). The encoder matches
[nv2a-vsh](https://pypi.org/project/nv2a-vsh/) bit for bit
(`tools/xbox/test_vp_encoder.py`): relative addressing is `c[A0+N]`; output
and write masks are bit-reversed relative to `xyzw`; MAC `ADD` takes slots A
and C, ILU ops (RCP, RSQ, LIT, ...) slot C.

Constant rows (`nv2a_vp.h`):

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
matrix index as a float. Skinning: GX selects a position matrix per vertex
(PNMTXIDX, 0..27 in steps of 3); the program loads it into `a0` and reads
`c[A0+6]`, so a whole skinned mesh is one draw.

### Shorter programs

`vp_optimize` shortens the generator's one-op-per-instruction output
without changing any value (same operations and operands, same bits): MOVs
to outputs fold into the ops that computed them, unread lanes and dead ops
go, and ops are rescheduled so an ILU op pairs with a MAC op (the ILU op
writes r1; a pair never writes a register it reads, since xemu runs the MAC
op first). The generator drops attenuation factors flagged as 1
(`VpKey.ang_one`/`dist_one`), and `vp_canon` clears key fields a program
doesn't depend on so such draws share a program. `tools/xbox/test_vp_opt.py`
compares 200000 random keys against the original generator
(`tests/xbox/vp_ref.c`) bit for bit, in hardware and xemu order; keys that
didn't fit 136 instructions then are approximated as then (`legacy_len`).

### Program memory

The NV2A holds 136 instructions, and a match frame's programs are several
times that, so some are reloaded every frame (16 bytes per instruction,
bursts of 8). `nv2a_vpmem.c` places a program in the smallest free gap
that fits, else the contiguous window whose programs are needed last.
"Needed" is Belady's rule with the previous frame as the forecast (frames
select programs in nearly the same order; ties: least recently used, then
fewest instructions overwritten). Loads can overwrite any program, the
current one included: the GPU runs the pushbuffer in order.

`tools/xbox/vp_policy.py` replays select traces (`-DXGX_DEBUG_VPTRACE`)
against flush-all, LRU and offline Belady; `[NV2A] per N frames: L vertex
programs loaded ...` reports it on the console.

### Depth

GX clip-space z/w runs from -1 (near) to 0 (far), and the depth written is
`z/w * (far - near) + far`. The NV2A takes screen-space z directly, so row 2
of the projection is replaced by

```
row2 = ZMAX * ((vf - vn) * P[2] + vf * P[3])
```

where `vn`/`vf` are the viewport depth range and ZMAX is 2^24-1 (Z24) or
65535 (Z16). Rows 0 and 1 fold in the viewport scale and offset, y flipped.

This is a z-buffer: CONTROL0's `Z_PERSPECTIVE_ENABLE` (w-buffering) must
stay off. pbkit's `pb_target_back_buffer` turns it on ("We use W") each time
it targets the back buffer, so `frame_open` sets CONTROL0 again right after.
Depth is clamped to the range (`ZMIN_MAX_CONTROL` ZCLAMP_CLAMP) as on the
GameCube, not culled (`-DXGX_DEPTH_CULL=1` culls); geometry behind the eye
is still clipped on w.

16-bit colour (720p, `-DXHW_VIDEO_480_BPP=16`) gets Z24S8 depth too since
v53 (`XGX_Z24_16BPP`, default 1): the NV2A takes colour and depth formats
of different widths (nxdk_pgraph_tests runs its suites with A8R8G8B8 + Z16
on the hardware; stock pbkit pairs R5G6B5 with Z24S8). Z16 couldn't keep
decals in front: Pokémon Stadium's red arrowheads sit 0.25 units in front
of the screen's frame and are drawn before it with LEQUAL, and a Z16 step
there is ~0.5-0.7 units even with the remap below (Z24 ~0.1, the
GameCube's), so the frame covered all but slivers of them. Cost: 1.8 MB
more contiguous memory at 720p (the depth buffer 3.5 MB instead of 1.8)
and twice the depth bytes a fragment, before Z compression, whose tile
flags (32-bit) now match the format. If `pb_init` can't get the memory it
starts again with Z16 before giving up 720p. `[NV2A] up: ... depth Z24S8`
at boot; test builds take `env MX_Z24=0|1`.

At Z16 (`-DXGX_Z24_16BPP=0`) a depth step at eye distance D is
about D^2 / (65536 * near), ~3 units where the fighters are with Melee's
near 0.1 / far 16384: enough to z-fight. So at Z16 depth is remapped once
per frame: GX depth g (0..1) is stored as (g - g0) / (1 - g0), g0 being a
camera's depth at far / `XGX_Z16_DEPTH_RATIO` (4096); of the frame's
projections whose far/near ratio exceeds that, the smallest g0 decides. The
remap is affine (still a z-buffer) and the same for every projection of the
frame, ortho too (the match timer is depth-tested against the stage).
Depth 1 stays; nearer than g0 clamps to 0. Clears and the Z-texture mask
convert through the same remap (`z_store`); fog reads GX's own row
(`s_zrow_gx`). `build_proj` collects g0 and `xgx_present` makes it the next
frame's. Z24 is untouched; `-DXGX_Z16_DEPTH_RATIO=0` turns it off (test
builds: `env MX_Z16_RATIO=n`). A smaller ratio (512) still left Stadium's
arrowheads in slivers, and things nearer than far / ratio would share
depth 0.

### Culling

The viewport y-flip is folded into the projection, so GX's front faces come
out clockwise and the front face is CW (checked in xemu against Dolphin on
the memory card screen). `GX_CULL_BACK` maps to 0x405 (back),
`GX_CULL_FRONT` to 0x404 and `GX_CULL_ALL` to 0x408 (front and back).

## TEV -> register combiners (`nv2a_rc.c`)

This comes from OpenCrossing-Xbox's compiler, generalized: up to 8 combiner
stages and 4 texture units. TEV konst colours and registers become per-draw
combiner constants (`RREF_*`). A TEV configuration that can't be represented
exactly is approximated and counted (`[NV2A] frame N: D draws (A
approximated)`, every 600 frames).

Constants are unsigned, and reading PREV back clamps at 0, so the signed,
unclamped THP YUV -> RGB recipe (`sobjlib.c`'s movie sprite) is recognised
as a whole and replaced by a three-stage program with signed registers and
fixed constants (`RREF_FIXED`).

TEV swap tables: the identity and `AAAA` cost nothing. A colour broadcast
(`RRRA`, `GGGA`, `BBBA`, GXInit's tables 1-3) or an alpha from r, g or b
costs one combiner stage in front of the TEV stage: a dot product with a
unit vector (`RREF_FIXED` 4-6) into a spare register (AB colour view, CD
alpha view). The user is the 1P clear screen's sepia freeze frame
(`lb_800122F0`). `[NV2A] per N frames: D draws through swap tables` counts
them; `tools/xbox/test_rc.py` checks against `tests/xbox/rc_ref.c` and a
combiner model.

### Current limits

- signed TEV colours (`GXSetTevColorS10` below 0) and unclamped stages
  work only in the movie recipe;
- TEV swap tables: permutations are approximated by the identity;
- indirect texturing: only what the NV2A's BUMPENVMAP can do (below);
- destination alpha: HSD only sets `GX_PF_RGB8_Z24` (no EFB alpha). R5G6B5
  (720p) has none either, so `GX_BL_DSTALPHA` reads 1; at 480 it reads what
  draws left in the 32-bit framebuffer. EFB copies always get alpha 1 for
  colour, IA and RA formats (an I4/R4 copy's alpha is its intensity), as
  from GX's RGB8 EFB (the sepia freeze frame draws with it, `lb_80012994`);
  only the Z-texture mask (`XGX_COPY_ALPHA`) reads the back buffer's alpha;
- texture-matrix index attributes (TEXnMTXIDX) are ignored.

## Indirect texturing (`nv2a.c`)

GX's indirect stages offset a TEV stage's texture coordinate by a 2x3
matrix times (s, t, u) minus a bias, read per pixel from an indirect
texture. The one user is `lbRefract` (`lbrefract.c`): a cloaked fighter is
drawn as a half-size copy of the frame offset by a 32x32 IA8 lens map
(`GX_ITF_8`, `GX_ITB_ST`, matrix 0 = -1, no wrap).

The NV2A's counterpart is a `BUMPENVMAP` unit, which adds a 2x2 matrix times
(du, dv) from an earlier unit's blue and green to its coordinate.
`derive_units` maps the stage onto two units:

- the indirect map gets one of the first units (`UNIT_IND`, 2D projective;
  a bump unit reads an earlier unit, `SET_SHADER_OTHER_STAGE_INPUT`, so unit
  0 can't be one), its texgen scaled by `GXSetIndTexCoordScale`;
- the stage's texture gets its own unit (`UNIT_BUMP`, program 6), the
  matrix in `SET_TEXTURE_SET_BUMP_ENV_MAT` (order 00, 01, 11, 10).
  `BUMPENVMAP` doesn't divide by q, so a projective texgen there divides
  per vertex in the vertex program (`VP_PROJ_DIVIDE`).

The unit reads du, dv as two's-complement bytes / 127, so the indirect map
is drawn from a copy (`ind_tex`, A8R8G8B8, once per texture and generation)
holding s / 2 and t / 2: halved, no texel crosses the wrap, filtering stays
linear as on GX, and GX's -128 bias becomes a constant in the bump unit's
texgen rows. The matrix is the GX matrix times 254 over the texture's size.
The offset is within half a step of GX's.

Drawn direct (counted as approximated): dynamic matrices, wrapping,
`add_prev`, bump alpha, the 5/4/3-bit formats, DXT maps, no free unit pair,
a non-zero u column. `-DXGX_NO_INDIRECT=1` draws every indirect stage
direct; `tools/xbox/scenarios/inv` has two of four CPUs cloaked.

## Fog (`nv2a_fog.c`)

GX applies fog after the last TEV stage, to the colour only:
`rgb = (rgb * (256 - F256) + fog * F256) >> 8` with F256 = F * 256. F comes
from the pixel's 24-bit EFB depth Zs and the registers `GXSetFog` writes (A
and C cut to 11 mantissa bits; for perspective fog B as a 24-bit magnitude
and a shift: libogc `GX_SetFog`, Dolphin's `PixelShaderGen`):

| fog | per pixel |
|---|---|
| perspective (`GX_FOG_PERSP_*`) | `ze = A * 2^24 / (b_mag - (Zs >> b_shift))`: eye depth / (end - start) when the fog's near and far are the projection's |
| orthographic (`GX_FOG_ORTHO_*`) | `ze = A * Zs / 2^24` |
| all | `x = clamp(ze - C, 0, 1)`; LIN `F = x`, EXP `1 - 2^(-8x)`, EXP2 `1 - 2^(-8x^2)`, REVEXP `2^(-8(1-x))`, REVEXP2 `2^(-8(1-x)^2)`; `GX_FOG_NONE`: off |

The NV2A has no per-pixel depth fog; its fog unit interpolates a factor
the vertex program writes to oFog.x. So:

- **Vertex program.** F per vertex from the vertex's own depth: `ze - C` is
  a ratio of two linear functions of the view-space position (rows
  `VPC_FOG`, `VPC_FOG + 1`, built by `fog_setup` when fog or projection
  change); EXP/EXP2 use `expp` (curve constants in `VPC_FOG + 2`).
  `VpKey.fog` selects the variant: 4 (LIN) to 10 (EXP2) more instructions.
- **Fog unit.** LINEAR, `FOG_PARAMS` (1, 1, 0): the linear mode computes
  `p0 + p1 * oFog - 1` (nxdk's NV10 notes, xemu), so the factor is F,
  clamped per pixel. The NV2A's EXP modes aren't used: they are per vertex
  too, and EXP's zero point measures ~1.51 on hardware, not 1.5.
- **Final combiner.** CW0 becomes A = FOG.a (the factor), B = FOG.rgb (the
  colour; `SET_FOG_COLOR` is ABGR), C = PREV, D = 0; alpha is untouched.
- **State.** Sent only when `GXSetFog` changed something (`XGX_DIRTY_FOG`)
  or the projection changed while fog is on.

Accuracy (`tools/xbox/test_fog.py`, against libogc's registers and
Dolphin's formula): at the vertices F is within 1/255 of GX, including GX's
coarse depth steps and b_mag rounding. Between vertices F is interpolated
linearly, so a polygon spanning a long depth range gets somewhat less fog
mid-span than on the GameCube (up to ~23/255). `GXSetFogRangeAdj` is a
no-op. Users (`HSD_FogSet`): stages (`Ground_801C1E94`/`Ground_801C1E2C`),
particles with `DispFog`, the title, intros, cutscenes, the trophy display.

## Textures (`gx_tex.c`, `nv2a.c`)

### Formats

- Power-of-two sizes use the NV2A's formats: CMPR -> DXT1, I4/I8 -> AY8,
  IA4/IA8 -> A8Y8, RGB565 -> R5G6B5, C4/C8 -> I8 indices with a 256-entry
  A8R8G8B8 palette (at the start of the texture's pool allocation,
  `SET_TEXTURE_PALETTE`); everything else A8R8G8B8 (`docs/architecture.md`
  has the table, `tools/xbox/test_tex_convert.py` the checks).
- Non-power-of-two images are linear textures of their own size
  (`LU_IMAGE_AY8`/`A8Y8`/`A8R8G8B8`, rows 64-byte aligned), sampled in
  texels, so `build_texgen` scales a unit's s and t rows by the size (the
  post matrix when the texgen normalizes). GX allows only clamping and no
  mipmaps at such sizes, as linear textures do.
- CMPR -> DXT1: GX's 8x8 tiles of four DXT1 blocks (TL, TR, BL, BR, small
  mips padded to a tile) are reordered into 4x4 block rows, both colours
  byte-swapped, each index byte's 2-bit fields reversed. Blocks with a
  transparent texel (three-colour mode, index 3) make the texture DXT3: GX
  decodes that index as the average colour with alpha 0, DXT1 as black.
- Levels are swizzled into a cached buffer, then copied in order (swizzled
  stores into write-combined memory defeat write combining).
- Bump texgens (`GX_TG_BUMPn`, HSD's emboss) are drawn as their source
  coordinate, and the emboss stage pair that cancels is dropped.
- Movie frames (`xbox/src/sdk/thp.c`) are decoded with stb_image's IDCT
  straight into the game's I8-tiled Y/Cb/Cr planes; the TEV makes RGB.

### Cache

- Textures are converted once and cached, keyed by data pointer, size,
  format, palette and mip count; an EFB copy by its destination pointer
  alone. Lookups hash the data pointer; binding the object a map already
  holds this frame skips the lookup (`bind_unchanged`).
- Revalidation, as for display lists: a sampled hash (whole up to 512
  bytes and palettes, else 64 words) once a frame per texture drawn; after
  120 passes (`TEX_STABLE`) every fourth frame with 16 words (`quick_hash`).
  `[TEX] changed:` reports failures; `GXInvalidateTexAll` forces nothing.
- Idle release: textures unused for 600 frames (`TEX_IDLE_FRAMES`) are
  released, EFB copies not (a screen can bind a copy's destination before
  that frame's copy; an upload would show memory the copy never wrote). A
  copy idle that long binds only at the size it was copied at (a bind at
  another size drops it and uploads) and goes at the next scene change
  (`gx_tex_scene_leave`).

### Pool and eviction

- The texture pool is contiguous memory: 8 MB at 480, 6 MB at 720p (init
  falls back 1 MB at a time to 4 MB; `-DXGX_TEX_POOL_KB=<n>` sets it).
- When it is full, `gx_tex_make_room` evicts least recently used entries
  until about the needed size is freed, waits for the GPU once and retries,
  freeing twice as much each round (the pool fragments). Textures bound to
  a map are never evicted; one drawn this frame only when nothing older is
  left. EFB copies can't be rebuilt, so among older entries they rank as
  younger (`lru_victim`): by 60 frames (`EFB_GRACE`), or 3600
  (`EFB_REPEAT_GRACE`) for a destination copied to again (a screen, a
  shadow map), so Pokémon Stadium's screen copy outlasts other views while
  the attract demo's one-off copies still go first (`decisions.md`).
- When only textures drawn this frame are left (the Trophy Collection draws
  every trophy at once), an overflow pool is taken from free RAM (up to
  8 MB, keeping 6 MB for the game's demand-committed memory) and given back
  at the next scene change (`[TEX] overflow pool ...`).
- When nothing is left to evict, the texture is dropped for that draw
  (untextured, usually black; magenta with `-DXGX_DEBUG_MAGENTA`), logged as
  `[TEX] drop:`. `[NV2A]`'s `pool allocations failed` also counts
  allocations that fit after evicting, so it is nonzero in long sessions.

### EFB copies

`GXCopyTex` is drawn by the GPU (`efb_copy_gpu`): the back buffer, bound as
a linear texture, is drawn with one quad into the destination texture as a
swizzled render target (pbkit's whole-RAM DMA object 3 as the colour
context). The cache registers the texture under the destination pointer.
These are the shadow-map notes too: HSD's shadow maps are EFB copies.

- **Channels**: the combiners keep the channels the copy format stores
  (shadow maps are `GX_CTF_R4`, sampled as I4, so red goes everywhere).
  Alpha: see "Current limits".
- **Size**: the nearest power of two per side, not the next (`copy_dim`),
  filtered linearly when smaller than the source (Pokémon Stadium's 640x406
  screen copy is 512x512).
- **Format**: the back buffer's (A8R8G8B8, or R5G6B5 at 16 bits). The
  depth surface points at the target through DMA object 3, not pbkit's zeta
  object (10, the compressed screen depth); depth is neither tested nor
  written.
- **Exact rect**: a target pixel's centre samples the source pixel under it
  (`efb_copy_gpu`'s corners, `pix_x`/`pix_y` in `read_rect_cpu`). The window
  clip's maximum is inclusive: the game's scissor sends `x1 - 1`, `y1 - 1`,
  and the copy clips to `pw - 1`, `ph - 1`. Off by one, shadow maps read
  black at their last row and column (black bands on the floor near
  fighters).
- **Ordering**: `WAIT_FOR_IDLE` on both sides orders the copy against the
  draws around it; the CPU never waits. Then the back buffer is the target
  again (`ocx_pb_retarget_back_buffer`, from `patch_pbkit.py`: surface state
  only, unlike `pb_target_back_buffer`'s four GPU-to-CPU interrupts) and
  every state group is re-sent.
- **Surface switches sent twice** (`XGX_COPY_FIX`, default 5): on the
  console a colour-side surface write right after a context-DMA switch
  occasionally doesn't take (stall: `LIMIT_ZETA` on the next clear or
  `LIMIT_COLOR` on the quad's `END`). Bit 1 re-sends the retarget's pitch
  after the format (`ocx_pb_retarget_repitch`); bit 4 re-sends the copy's
  target and the retarget's DMA objects, pitch and offsets after a wait for
  idle; bit 2 (one `CLEAR_SURFACE`) is untried. Bit 4's pitch is each
  surface's own (`zeta_pitch`): with Z24S8 behind 16-bit colour the depth
  pitch is twice the colour's, and sending the colour's for both (until
  v53, when they were always equal) addressed depth at half its pitch
  after each copy, so the copy's depth clear and the draws after it hit
  the wrong rows (black striped silhouettes over Fountain's sky, striped
  and missing Kirbys on the Classic team card, in xemu). `-DXGX_COPY_STRESS=N`
  repeats each clearing copy N times to provoke faults (`env MX_COPY_FIX=`/
  `MX_COPY_STRESS=`, `scenarios/stall`; results in `roadmap.md`).
  `[NV2A] GPU stalled` dumps PGRAPH state and the last eight copies.
- `-DXGX_EFB_GPU_COPY=0` reads back on the CPU instead (ARGB8 at every bpp;
  ~8 ms per 256x256 map, the framebuffer being write-combined).

### Z textures

`GXSetZTexture` (the Classic team cards: tiles drawn back with each
costume's copied depth so only the fighter shows) is emulated as a mask,
since the NV2A can't take a fragment's depth from a texture or sample the
compressed depth buffer. At a depth-format copy (`_GX_TF_ZTF`)
`xgx_ztex_mask` writes the rect's alpha: 1 where the stored depth is in
front of the copy's clear depth (GREATER), else 0. The copy takes that
alpha (`XGX_COPY_ALPHA`); a draw with `ztex` set gets one more combiner
stage (`APREV * TEXA`, `derive_units`) and an alpha test that drops 0
(`emit_fixed`). The pass starts with a no-op quad (depth writes on, test
`NEVER`) so xemu rebinds its depth buffer. At 16 bits the mask goes into
green (`XGX_COPY_GREEN`), which overwrites the rect's colour, so it is done
only when the depth copy clears the rect after itself (`gm_1832.c`);
otherwise the draw is unmasked. The 16-bit copy is R5G6B5, so the mask
lands in its rgb and its alpha samples 1: such a texture is marked
(`Tex.mask_rgb`) and the Z-texture stage reads its alpha from red (swap
`0x24`). Until v53 it read alpha, every tile drew whole and the 720p team
cards showed one fighter on a black right half.

## Build switches

Renderer switches (all switches: `testing.md`):

| switch | effect |
|---|---|
| `-DXGX_OVERLAP=0` | present waits for the GPU before the flip |
| `-DXGX_PB_KICK=<words>` | pushbuffer kick size (default 8192) |
| `-DXGX_VB_CACHE_BREAK=0` | no vertex cache break at each batch start |
| `-DXGX_VBUF_FREE_NOW=0` | evicted display-list buffers go through the deferred free |
| `-DXGX_COPY_FIX=<bits>`, `-DXGX_COPY_STRESS=N` | EFB copy surface re-sends, copy stress |
| `-DXGX_EFB_GPU_COPY=0` | EFB copies by CPU readback |
| `-DXGX_TILE=<bits>`, `-DOCX_Z16_TILE_FLAGS` | tile regions |
| `-DXGX_Z24_16BPP=0` | Z16 depth with 16-bit colour (720p) instead of Z24S8 |
| `-DXGX_Z16_DEPTH_RATIO=<n>` | Z16 depth remap; 0 off |
| `-DXGX_DEPTH_CULL=1` | cull depth outside the range instead of clamping |
| `-DXGX_NO_INDIRECT=1` | indirect stages drawn direct |
| `-DXGX_TEX_POOL_KB=<n>` | texture pool size |
| `-DXGX_DEBUG_MAGENTA` | dropped textures drawn magenta |
| `-DXGX_CHECK_VERTS` | log display lists with non-finite or huge positions |
| `-DXGX_DEBUG_VPTRACE` | vertex-program select traces for `vp_policy.py` |
| `-DXGX_CENSUS=1` | draw census (Probes) |

Known wrong in xemu: Mute City's distant skyline band renders as white
speckle.
