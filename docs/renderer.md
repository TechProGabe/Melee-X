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
```

`xgx.h` is compiled by both triples (game and nxdk), so its structs hold only
32-bit scalars, floats and byte arrays: no bit-fields, no 64-bit members.

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
65535 (Z16 at 720p). Rows 0 and 1 fold in the viewport scale and offset,
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
- fog is off;
- destination alpha is missing at 720p (R5G6B5 has no alpha);
- texture-matrix index attributes (TEXnMTXIDX) are ignored.

## Textures (`gx_tex.c`, `nv2a.c`)

- GX textures are converted once and cached, keyed by data pointer, size,
  format, palette and mip count. A sampled hash revalidates them at most
  once a frame, because HSD reuses archive memory.
- For power-of-two sizes the NV2A's own formats are used: CMPR -> DXT1,
  I4/I8 -> AY8, IA4/IA8 -> A8Y8, RGB565 -> R5G6B5. Everything else becomes
  A8R8G8B8; non-power-of-two images are resampled to the next power of two.
  `docs/architecture.md` has the table and `tools/xbox/test_tex_convert.py`
  the checks.
- CMPR -> DXT1 conversion:
  - GX stores 8x8 tiles, each holding four DXT1 blocks (TL, TR, BL, BR),
    and pads small mip levels to a full tile; they are reordered into
    DXT1's 4x4 block rows;
  - both colours are byte-swapped;
  - each index byte has its 2-bit fields reversed (GX puts pixel 0 in bits
    7-6, DXT1 in bits 1-0).
- The texture pool is contiguous memory: 6 MB at 480, 5 MB at 720p. When it
  is full, the least recently used cache entries are evicted and the GPU is
  waited on.
- EFB copies (`GXCopyTex`) read the framebuffer back on the CPU and make a
  texture, which the cache registers under the copy's destination pointer.
  This is slow; GPU-side copies are on the roadmap.
- Movie frames (`xbox/src/sdk/thp.c`) are baseline JPEGs without byte
  stuffing. They are decoded MCU by MCU, with stb_image's IDCT, straight
  into the game's I8-tiled Y/Cb/Cr planes. The TEV then converts them to
  RGB.
- Known wrong in xemu: Mute City's road renders as bright static, and a
  black wedge covers part of that stage (not yet diagnosed).
