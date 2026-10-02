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
decodes the list into a vertex buffer from its own pool (4 MB; 720p had 3
MB, which the first 720p match on the console filled, v38) and later calls
replay the draws. One 4-CPU match fills about 3 MB; over a long session
(several stages and characters) all 2048 slots and the whole pool stay in
use, and the LRU eviction below rebuilds a few dozen
to a few hundred lists every 10 s (v36 console log). Entries are keyed by the
list's address and size and by the vertex descriptor and the formats (VAT)
the list uses: HSD draws some small lists under different formats (shared
by models quantized differently), and one entry per list flipped between
them until ~100 lists were decoded on every call. Now a 4-CPU match keeps
1400-2048 lists cached, none volatile. An entry is checked against the
vertex descriptor, formats and arrays on every call, and against a sampled
hash of the list and of the array ranges it indexed once a frame; a list
that passed 120 checks in a row is checked every fourth frame, staggered by
slot (the checks were ~4% of the console's CPU in a match, where no list
changed content; a list rewritten in place would show stale for up to three
frames). A list
whose arrays keep changing (skinned and morphed models, whose positions and
normals HSD rewrites or re-points every frame) goes dynamic after four
rebuilds: its decode plan, every vertex's indices and a decoded template in
cached RAM are kept (1 MB for all of them). Each call re-fetches only the
attributes whose array moved or whose sampled hash changed (once one has
changed it is fetched on every call), then copies the template into the
vertex ring in one sequential write. Lists that don't fit the budget are
decoded every call (volatile). `[DLC]` lines report both, and `[DLC]
uncached:` names the first lists that could not be cached and why. A list
may hold up to 512 draws: Fountain of Dreams' stage is one 104 KB list of
more than 64, and at the old limit of 64 it was decoded on every call
(~15 ms of a console frame). When the pool is full, the least recently
used list not drawn this frame is evicted and its buffer freed at once:
the frame's first GPU use (`frame_open`) waits for the GPU, so nothing
reads it any more (a free before that point opens the frame first). Freeing them only after the next wait for idle cost Pokémon
Stadium ~4 extra waits a frame on the console, each one stopping the CPU
until the GPU had drawn everything queued. The content hashes (display lists and
textures) run four FNV chains side by side over the same words, so the
loads and multiplies overlap instead of waiting on one serial chain.

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
- `GXLoadTexMtxImm` compares the new rows first and changes nothing when
  they match: HSD loads every texture's matrix before each draw, and it
  marked the texture and position matrices dirty on ~77% of a match's draws
  (rebuilding the texgen rows); now ~9%.
- The pushbuffer is kicked every 32 KB (was 16 KB): each kick runs pbkit's
  `pb_start`, which flushes the NV2A's write-combine cache and spins until
  it is done (~2% of the console's CPU).
- Every pushbuffer batch starts with `BREAK_VERTEX_BUFFER_CACHE`. The NV2A
  caches vertex data by address and a draw's fetch reads ahead of its last
  vertex; once a batch is kicked the GPU can run it before the CPU writes
  the next draw's vertices into the ring right behind it, and that draw
  then takes its first vertices from the stale read-ahead. On the console
  each EFB copy waits for idle, and the next shadow map's white background
  quad lost its first triangle (black wedges above its diagonal, black
  flashes on the stage surfaces near fighters that multiply the maps in).
  xemu has no such cache and drew them right.
- The window clip's maximum is inclusive (xemu adds 1 to it as well):
  the game's scissor sends `x1 - 1`, `y1 - 1`, and the EFB copy into a
  swizzled texture clips to `pw - 1`, `ph - 1`. With `pw` a pixel in column
  or row `pw` lands past the swizzled target. The copy and the Z-texture
  mask also send `BREAK_VERTEX_BUFFER_CACHE` right before their draw: the
  batch they join may be open already, and a stale read of their four
  vertices puts the quad anywhere. Both are the v40 candidate fix for the
  console's `LIMIT_COLOR` stall on the copy quad's `END` (`roadmap.md`).
- The EFB copy's depth side points at the copy target through the
  whole-RAM DMA object (3), not pbkit's zeta object (10, only the screen's
  compressed depth buffer at the back buffer's shape); depth is neither
  tested nor written. v42 still stalled on the copy quad's `END`, then with
  `LIMIT_ZETA` as well as `LIMIT_COLOR`: v43's attempt. The first fault now
  also logs PGRAPH 0x400700-0x4008FC and the last eight copies with where
  each one's `END` sits in the pushbuffer.
- An EFB copy reads exactly its source rect: a target pixel's centre
  samples the source pixel under it (`efb_copy_gpu`'s corners, `pix_x`/
  `pix_y` in `read_rect_cpu`). Until v40 both paths sampled one pixel right
  and down; the scissor's extra column had covered that, and with the exact
  scissor the shadow maps' last row and column read black past their white
  background (black bands across the stage floor near fighters).
- Build switches undo these v26 GPU-side changes one at a time, to bisect
  the console's GPU stalls (Pokémon Stadium, v27/v28): `-DXGX_PB_KICK=4096`,
  `-DXGX_VB_CACHE_BREAK=0`, `-DXGX_VBUF_FREE_NOW=0`. A stall report
  (`[NV2A] GPU stalled`) also logs PGRAPH's interrupt, trap, surface, clear,
  window-clip and raster registers; `-DXGX_CHECK_VERTS` logs display lists
  with non-finite or huge positions.
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

### CPU cost of the back end (v33-v35)

The v32 console profile of a 4-CPU Fountain of Dreams match (21 fps, ~47 ms
a frame) had the back end at about a quarter of the CPU, mostly cache
misses in lookups and revalidation rather than useful work. v33:

- **CPU/GPU overlap** (`XGX_OVERLAP`, default 1): `xgx_present` no longer
  waits for the GPU. It queues the frame-rate counter (GPU colour fills, it
  was CPU writes into the back buffer) and pbkit's flip, which the GPU runs
  after the frame's draws (triple buffering), and returns. The wait for idle
  moved to `frame_open`, the next frame's first GPU use, so the GPU finishes
  while the CPU runs the next simulation ticks; deferred frees are released
  there. Everything after `frame_open` still sees an idle GPU at the start
  of the frame (the vertex ring and pushbuffer restart, `xgx_vbuf_free_now`).
  `GXCopyDisp`'s clear, which comes right after the present, is queued
  until the frame opens. `frame_open`'s own clear of the whole
  framebuffer (the bars around a pillarboxed content rect, a defined EFB)
  is skipped when the first queued clear writes colour and depth over all
  of it anyway: there are no bars then, and it was a second full fill and
  depth clear (and a GPU wait) per frame. A screenshot frame still waits
  in the present. The console waited ~3.6 ms a frame there on Fountain of
  Dreams; xemu, whose GPU is slow, went from 19 to ~33 fps in the standard match.
  `-DXGX_OVERLAP=0` restores the old order.
  The settings menu on the title screen (`xgx_set_overlay`,
  `xhw_overlay.c`) is the exception: its text is CPU writes into the
  finished frame, so a present that shows it waits for the GPU before
  writing (only there; the overlay lapses two presents after the title
  stops refreshing it).
- Display-list eviction scans a packed array of every slot's last use
  (8 KB) instead of a word from each 180-byte entry: 2048 cache misses per
  eviction, a few evictions a frame on Fountain of Dreams (~1.5% of the CPU).
- The display-list cache's per-call key and check (`fmt_key`, `vtx_sig`)
  were FNV hashes over the descriptor, the formats and the arrays (~250
  bytes, ~700 calls a frame). The vertex setters now keep sums of a mixed
  term per attribute setting, so both are a few multiplies; a sum doesn't
  depend on the order of changes, so `GXClearVtxDesc` and the
  `GXSetVtxDesc` calls that restore the same descriptor cancel out.
- Revalidation of stable display lists and textures (passed 120 checks)
  samples 16 words per range instead of 64 (`content_qhash`, `quick_hash`):
  the samples are scattered cache misses. Textures above 512 bytes are
  sampled (64 words) instead of hashed in full up to 4 KB (most of a
  match's textures). The schedule is unchanged: every fourth frame,
  staggered, once stable.
- The combiner-program and vertex-program caches keep their entries'
  hashes (and the programs' valid flags and last use) in packed arrays:
  their lookups scanned a word from each 624-byte or 2 KB entry.
- `gx_tex_bind` marks the maps dirty only when the bound texture or its
  sampling changed: HSD rebinds the same texture through a new texture
  object per material, and ~40% of a match's draws rebuilt the texture
  units and combiners for nothing.
- Light rows are built only for the lights an enabled channel uses (a
  channel change rebuilds them), and colours are scaled by 1/255 with a
  multiply instead of a divide (~20% of draws rebuild lights or channels).
- v34: the off-screen cull (`gx_dl_culled`) transforms the box's centre
  and tests the view-aligned box around it against each clip plane (one
  transform and five plane tests instead of eight corners); it can only cull
  less than the corner test, never more. `GXLoadPosMtxImm` and
  `GXLoadTexMtxImm` compare and copy their 3x4 matrices inline as 12 words
  (48 bytes was a memcmp and a memcpy call each, ~2% of the CPU).
- Game side (imported code, `docs/decisions.md`): v34 prefetches the next
  node in HSD's animation and display list walks (the P3 has a 128 KB L2
  and no hardware prefetcher); v35 reuses an envelope's matrices when a
  later PObj of the same DObj uses the same joints and weights (~1250
  envelope matrices a frame in a 4-CPU match, ~630 distinct).
- After v45, per draw: a texture unit built from the same inputs as the
  registers it last sent (texture handle, the map's wrap, filters and LOD
  bias, the unit kind, and an epoch every texture made or freed bumps)
  skips the `s_tex` read and the register build (`emit_textures`): most
  draws that rebuild the units rebind one map, not all of them. The
  combiner constants (`pack_const` per stage) are rebuilt only when the
  program is sent or a TEV colour or konst colour changed
  (`XGX_DIRTY_TEVREG`), not on every map or TEV change.

`xgx.h` is compiled by both triples (game and nxdk), so its structs hold only
32-bit scalars, floats and byte arrays: no bit-fields, no 64-bit members.

The GPU counts as idle (`wait_idle`) only once the pusher has caught up,
PFIFO's CACHE1 is empty, the pusher has stopped and PGRAPH is idle, seen
twice in a row. pbkit's `pb_busy` checks the first and last only, so
methods still in CACHE1 passed as done whenever PGRAPH was between two of
them. Deferred frees, the vertex ring's restart at every frame (in
`frame_open`) and EFB copy targets all rely on this wait. xemu runs methods as they arrive and
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
  answers them from `xgx_content_size`. The player HUDs are spread toward
  the edges; the match timer is left at the top centre (`ifall.c`, `PORT:`).
- **The flip is `GXCopyDisp`**, where a GameCube frame ends: the EFB is
  copied to the XFB and cleared. `VIWaitForRetrace` only paces the game to
  60.000 Hz and runs the alarms (`vi.c`).
- Framebuffers: 640x480x32 + Z24S8, or 1280x720 R5G6B5 + Z16 at 720p
  (32-bit colour doesn't fit in 64 MB next to the game). `pb_DepthFmt` is
  made settable by `tools/xbox/patch_pbkit.py`. Everything 16-bit keys on
  the mode's bpp, not its size, so `-DXHW_VIDEO_480_BPP=16` runs 640x480 as
  R5G6B5 + Z16 with 720p's pool sizes: the 720p path in xemu, which has no
  720p. pbkit gives the depth buffer's tile compression tags with the
  32-bit flag (`0x84000001`) also for Z16; `-DOCX_Z16_TILE_FLAGS` sets the
  Z16 tile's flags for a console A/B (`docs/testing.md`), the default is
  unchanged. Clear colours go to pbkit's `pb_fill` as A8R8G8B8, which
  converts them to the surface's format: `clear_fb` converted them to
  R5G6B5 first as well, so every 16-bit clear colour (a stage's fog-coloured
  clear) came out near black.
  Z16 with Melee's near 0.1 / far 16384 has about `d^2 / 6550` units of
  depth resolution at distance `d`; xemu 0.8 floors depth to 16 bits as
  the hardware does, and the 720p Dream Land and Pokémon Stadium runs show
  no z-fighting.

## Vertex programs (`nv2a_vp.c`)

Each transform/lighting/texgen configuration (`VpKey`) gets a generated
program, cached by key (64 entries). The instruction encoder matches
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

### Shorter programs

The generator writes one op per instruction over named temporaries;
`vp_optimize` then shortens the program without changing any value in it.
Every step keeps each value's operation and operands, so the results are
the same bits:

- a MOV to an output goes, and the ops that computed its source write the
  output themselves (also the temporary if it is read again: a MAC op can
  write both);
- lanes nobody reads aren't written, and ops left writing nothing go;
- MOVs of the same constant or input row into one destination merge;
- the ops are scheduled again so an ILU op (RCP, RSQ, EXP, or a MOV) shares
  an instruction with a MAC op, and temporaries are allocated again for
  that order. The paired ILU op writes r1 (nv2a-vsh, xemu). Lanes built by
  several ops and read together (RPOS, a texgen's source) stay in one
  register. A register isn't written in an instruction of a pair that reads
  it: xemu runs the MAC op before the ILU op.

The generator itself also computes less: a channel whose colour and alpha
halves have the same lights and functions lights all four lanes in one loop;
the spot and specular vectors (1, cos, cos^2), (1, d, d^2) are built where
the values land, the constant 1 set once per channel; a texgen from the
position or normal reads the attribute directly (declared with 3 components,
so w reads 1). And `VpKey.ang_one`/`dist_one` flag lights whose angle or
distance attenuation rows are (1, 0, 0), as HSD sets them for infinite
lights (both) and point lights (angle): in a spot channel that factor is
1 + 0 cos + 0 cos^2 = 1 and rcp(1) = 1, so it and its product (x * 1 = x)
are left out, 9 instructions per infinite light. `vp_canon` clears the key
fields a program doesn't depend on (the second channel with one channel on,
a disabled half's lighting, unused texgens and flags), so draws that differ
only there share one program and don't switch.

`tools/xbox/test_vp_opt.py` runs the programs of 200000 random keys, and the
programs of the generator before all this (`tests/xbox/vp_ref.c`), through an
interpreter of the encoded words, in the hardware's order and in xemu's, and
requires the same bits in every output lane. Programs average 48 instructions
there, 68 before; a lit and specular fighter program with two infinite lights,
fog and a texture went from 96 to 54. Keys the old generator couldn't fit
(more than 136 instructions: lights past the first two and spot attenuation
dropped) are still approximated exactly as then (`legacy_len`), so the
picture is unchanged; about a third of them would fit now.

### Program memory

The NV2A holds 136 instructions, and the programs a match frame selects are
several times that, so some are loaded again every frame (through the
pushbuffer: 16 bytes per instruction, bursts of 8). `nv2a_vpmem.c` decides
where: the smallest free gap that fits, else the contiguous window whose
programs are needed last. "Needed" is Belady's rule with the previous frame
as the forecast: frames select their programs in nearly the same order, so a
resident program's next use is where the previous frame selected it next
(ties: least recently used, then fewest instructions overwritten). Before,
program memory was packed from 0 and flushed whole when the next program
didn't fit. Loads can overwrite any program, the current one included: the
GPU runs the pushbuffer in order.

`tools/xbox/vp_policy.py` replays select traces (`-DXGX_DEBUG_VPTRACE`, or a
synthetic 4-CPU Fountain of Dreams frame) through that code and through
flush-all, LRU and an offline Belady for comparison. On the synthetic frame
(two infinite diffuse lights, one specular): 106 loads and 7200 instructions
a frame before, 75 / 2830 with the shorter programs alone, 46 / 1730 with the
residency policy too (LRU 57 / 2210; offline Belady 43). `[NV2A] per N
frames: L vertex programs loaded (I instructions), S program switches`
reports it on the console.

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

At 16 bits (720p, or `-DXHW_VIDEO_480_BPP=16`) the depth buffer is Z16,
and a z-buffer step at eye distance D is about D^2 / (65536 * near).
Melee's match camera has near 0.1 and far 16384: ~3 units a step where the
fighters are, so crates, Fountain of Dreams' grass and floor layer and the
fighters in front of it z-fought (Final Destination, near 1, didn't). At
Z16 the depth is therefore remapped once per frame: GX depth g (0..1) is
stored as (g - g0) / (1 - g0), with g0 a camera's depth at far /
`XGX_Z16_DEPTH_RATIO` (4096), i.e. as if its near plane were at far / 4096
(~0.1 units a step at D ~150); of the frame's projections whose far/near
ratio is past 4096, the one with the smallest g0 decides. The remap is affine, so row 2 still
gives a z-buffer, and it is the same for every projection of the frame,
ortho too: the match timer's camera is depth-tested against the stage's
(a remap per projection hid the timer). Depth 1 (far, the copy clears)
stays; what lies nearer than g0 is clamped to 0, not clipped, and keeps
only draw order among itself. Clears and the Z-texture mask convert their
z24 through the same remap (`z_store`), fog still reads GX's own row
(`s_zrow_gx`). `build_proj` collects the frame's g0 and `xgx_present` makes
it the next frame's, rebuilding the projection rows when it changes; a
frame with no extreme projection uses none. Z24 is untouched.
`-DXGX_Z16_DEPTH_RATIO=0` turns it off.

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

TEV swap tables (`GXSetTevSwapModeTable`, `GXSetTevSwapMode`): a stage
sees its texture and rasterised colour through one of four tables, TEXC as
(src[r], src[g], src[b]) and TEXA as src[a]. A combiner input reads a
register's rgb or its alpha broadcast, so the identity and an alpha
broadcast (`AAAA`) cost nothing. A colour broadcast (GXInit's tables 1-3,
`RRRA`, `GGGA`, `BBBA`, which the front end now sets up as GXInit does;
they were all identity) or an alpha taken from r, g or b costs one combiner
stage in front of the TEV stage: a dot product of the source with a unit
vector (`RREF_FIXED` 4-6) into a spare register, AB for the colour view and
CD for the alpha view (CD's blue copied to its alpha), so up to two views
of one source per extra stage. The stage only exists for views the TEV
stage reads, and it counts against the 8 combiner stages (a configuration
that no longer fits is approximated, as before). Permutations (`GBRA` and
the like) are approximated by the identity: nothing in Melee's code sets
one. The user in the code is the 1P clear screen's sepia freeze frame
(`lb_800122F0`: three stages read one texture through `RRRA`, `GGGA`,
`BBBA` and sum K0 r + K1 g + K2 b, six combiner stages); HSD's TEV
descriptors (`HSD_SetupTevStage`) can name any table from model data.
`[NV2A] per N frames: D draws through swap tables (S combiner stages
added)` counts them. `tools/xbox/test_rc.py` checks that configurations
without a swizzle compile to the same combiner words as before
(`tests/xbox/rc_ref.c` is the compiler before swap tables), that swizzled
configurations run through a model of the combiners give what the identity
gives on pre-swizzled inputs, and the sepia case.

Current limits:

- signed TEV colours (`GXSetTevColorS10` below 0) and unclamped stages
  work only in the movie recipe;
- TEV swap tables: permutations are approximated by the identity;
- indirect texturing: only what the NV2A's BUMPENVMAP can do (below);
- destination alpha: R5G6B5 (720p) has none, so `GX_BL_DSTALPHA` reads 1
  there. That is what the GameCube does too: HSD only ever sets
  `GX_PF_RGB8_Z24` (`HSD_StartRender`), an EFB without alpha. The 32-bit
  framebuffer keeps an alpha channel GX wouldn't have, so `GX_BL_DSTALPHA`
  reads what the draws left there at 480. EFB copies don't: a colour,
  IA or RA copy has alpha 1 at every bpp, as from GX's RGB8 EFB (an I4/R4
  copy's alpha is its intensity, as on the GameCube; HSD's shadow maps
  take alpha from APREV, not the map). Until v43 a 32-bit copy kept the
  back buffer's alpha, often 0: the 1P clear's sepia freeze frame, drawn
  with the copy's alpha (`lb_80012994`), was transparent and the
  background behind the bonus list black. Only the Z-texture mask
  (`XGX_COPY_ALPHA`) reads the back buffer's alpha;
- texture-matrix index attributes (TEXnMTXIDX) are ignored.

## Indirect texturing (`nv2a.c`)

GX's indirect stages (`GXSetNumIndStages`, `GXSetIndTexOrder`,
`GXSetIndTexCoordScale`, `GXSetIndTexMtx`, `GXSetTevIndirect`) offset a TEV
stage's texture coordinate, in texels, by a 2x3 matrix times (s, t, u)
minus a bias, read per pixel from an indirect texture's alpha, blue and
green. The one user in Melee's code is `lbRefract` (`lbrefract.c`): a
cloaked fighter (the Cloaking Device, Invisible Melee) is drawn as the
stage behind it, a half-size copy of the frame (`lbRefract_80022560`)
sampled at the fighter's projected position and offset by a 32x32 IA8
lens map looked up by the view-space normal (`GX_ITF_8`, `GX_ITB_ST`,
matrix 0 = -1, no wrap). No stage uses it: Fountain of Dreams' reflection
and water are geometry (`[NV2A]` counts no indirect draws there), and
HSD's texture objects have no indirect path (`TEX_BUMP` is the emboss
handled under Textures).

The NV2A's texture shader has the counterpart: a `BUMPENVMAP` unit adds a
2x2 matrix times (du, dv), read from an earlier unit's blue and green, to
its coordinate. `derive_units` maps a TEV stage with an indirect offset
onto two units:

- the indirect map gets one of the first units (`UNIT_IND`, 2D projective),
  since a bump unit reads an earlier unit (`SET_SHADER_OTHER_STAGE_INPUT`
  for units 2 and 3; unit 1 reads unit 0) and unit 0 can't be one. Its
  texgen is scaled by `GXSetIndTexCoordScale`.
- the stage's own texture gets a unit of its own (`UNIT_BUMP`, program 6),
  with the matrix in `SET_TEXTURE_SET_BUMP_ENV_MAT`. `BUMPENVMAP` reads s
  and t without dividing by q, so a projective texgen on that unit divides
  in the vertex program (`VP_PROJ_DIVIDE`; per vertex, so the coordinate is
  interpolated as s/q, which differs from GX's per-pixel divide by the
  perspective across one triangle: small at a fighter's size;
  `test_vp_opt.py` checks the divide against the projective program).
  The matrix method takes 00, 01, 11, 10 (xemu's register order).

The unit reads du and dv as two's-complement bytes / 127 (xemu's `sign3`).
The indirect map is drawn from a copy (`ind_tex`: A8R8G8B8, s / 2 in blue
and t / 2 in green, made from the pool's texels once per texture and
generation; AY8, A8Y8, R5G6B5, A8R8G8B8 and P8 sources, up to 64K texels):
halved, no texel crosses the two's complement wrap, so filtering between
texels stays linear as GX's does, and GX's -128 bias becomes a constant
added to the bump unit's texgen rows (s + c q). The matrix is the GX matrix
times 254 over the GX texture's size (a swizzled texture is sampled over
[0, 1]; a linear one in texels). Halving loses the lowest bit: the offset
is within half a step of GX's (`2^scale` texels per step; Melee's lens
map: half a texel of the 320x240 copy, a pixel on screen).

Drawn direct (and counted as approximated, `[NV2A] per N frames: ...
indirect (M more drawn direct)`): dynamic matrices (`GX_ITM_S*`,
`GX_ITM_T*`), wrapping, `add_prev` (accumulated offsets), bump alpha,
the 5/4/3-bit formats, DXT maps, a stage without a free unit pair, and the
matrix's u column (ignored when not zero). Draws without an indirect stage
send exactly what they did before; `-DXGX_NO_INDIRECT=1` draws every
indirect stage direct (v40's behaviour) for bisecting on the console.
`tools/xbox/scenarios/inv` (`MELEE_DEBUG_VS_INVISIBLE=3`) is Fountain of
Dreams with two of four CPUs cloaked.

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
  becomes A8R8G8B8. Non-power-of-two images are linear textures of their own
  size (`LU_IMAGE_AY8`/`A8Y8`/`A8R8G8B8`, rows 64-byte aligned, one row copy
  each): I4/I8 and IA4/IA8 keep AY8/A8Y8, the rest A8R8G8B8. They are
  sampled in texel coordinates, so `build_texgen` scales the s and t rows of
  a unit that binds one by its width and height (the post matrix when the
  texgen normalizes); GX allows only clamping and no mipmaps at such sizes,
  which is what linear textures do. Before, they were resampled to the next
  power of two: a little blur, up to eight times the memory (the Trophy
  Collection's I4 textures), and ~18 ms a frame for the movie's 640x480
  Y/U/V planes on the console. Movie frames now cost ~2-3 ms of texture
  work in xemu (was 17-22) and the movie runs at ~59 fps there (was ~33).
  Power-of-two sizes that GX wraps are untouched; an EFB copy is still drawn
  to a power-of-two texture (`copy_dim`). On Pokémon Stadium, C8 as A8R8G8B8 took 2.1 MB of the pool. A
  texture unit is re-sent whenever a different texture binds, even one made
  at a freed texture's address with the same format and size, so a P8
  palette is always loaded again (xemu reads palettes at each draw).
  `docs/architecture.md` has the table and `tools/xbox/test_tex_convert.py`
  the checks.
- Off-screen culling: the display-list cache keeps each list's model-space
  box (lists without per-vertex matrices); `HSD_DObjDisp` asks
  `gx_dl_culled` with the DObj's model-view matrix and the current
  projection and skips DObjs of rigid PObjs that are wholly outside (the
  box's centre is transformed and the view-aligned box around it tested
  against the four side planes and the camera plane, so nothing visible is
  dropped; v34, was eight corners). A list is known after its first draw.
- Bump texgens (`GX_TG_BUMPn`, HSD's emboss) are drawn as their source
  coordinate (no binormal/tangent offset), and the emboss stage pair
  "prev + h(tc)*ras, prev - h(bump)*ras" is dropped since it cancels; the
  combiners would clamp the sum at 1 first (the crates' fronts).
- CMPR textures with a transparent texel (a three-colour block using index
  3) go to DXT3 instead: GX decodes that index as the average colour with
  alpha 0, DXT1 as black. The software decoder matches GX now too.
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
  them magenta. The `[NV2A]` line's `pool allocations failed` counts every
  allocation that didn't fit at first, including the ones that fit after
  evicting: with a full pool (every long session, v36) it is a few to ~15
  per 10 s while `drops` stays 0.
- When only textures drawn this frame are left to evict, the frame's working
  set is bigger than the pool: an overflow pool is taken from the RAM free
  at that moment (up to 8 MB, keeping 6 MB free for the game's
  demand-committed memory; `[TEX] overflow pool N KB`), and given back at
  the next scene change after its textures are dropped (`[TEX] overflow
  pool released`). The Trophy Collection, which draws every trophy at once,
  ran at 1.4 fps on the console re-uploading most textures every frame
  (650 ms a frame in texture conversion).
- Pool blocks are kept in address order, the free ones also on a list of
  their own and the allocated ones in a hash by offset; placement is
  unchanged (small requests take the lowest free block that fits, big ones
  the top of the highest). Walking every block per allocation and twice per
  free took ~5% of the console's CPU on Pokémon Stadium (~2000 blocks).
- Cache lookups go through a hash of the data pointer. Binding the object a
  texture map already holds this frame skips the lookup entirely. Textures
  are revalidated (sampled hash) once a frame, and every fourth frame
  (staggered by address) once one has passed 120 checks in a row: the
  hashes were ~4% of the console's CPU in a match, where no texture changed
  (one rewritten in place shows stale for up to three frames; `[TEX] ...
  N changed (M palette only)` and one `[TEX] changed:` line per interval
  report revalidation failures); `GXInvalidateTexAll`, which
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
  ARGB8 (2 MB of the pool) and is now 512x512. The target has the back
  buffer's format: A8R8G8B8 with a Z24S8 surface format, or R5G6B5 with
  Z16 at 16 bits (720p; the NV2A wants colour and depth surfaces of the
  same width even with depth off), and the back buffer is bound as
  `LU_IMAGE_R5G6B5` there. An R5G6B5 copy has no alpha: it samples 1
  ("Current limits"). Until this, 720p's 16-bit depth kept it on the CPU
  readback: a 4-CPU match there spent ~60 ms a frame in it (`efb`, v38
  console, 7.5 fps; 378 copies and ~380 extra GPU waits per 600 frames).
  The CPU readback (`-DXGX_EFB_GPU_COPY=0`, ARGB8 at every bpp) cost ~8 ms
  per 256x256 shadow map on the console: the framebuffer is
  write-combined, so each read is an uncached bus cycle. Reading it
  through 0x80000000 | physical does not help: contiguous memory already
  lives there, and the write-combine attribute is on those same page-table
  entries.
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
  With no texture coordinate in the vertex, `GXTexCoord1f32` feeds that
  stream too: the Classic team card primes its depth plane with a
  position-only quad written as twelve `GXTexCoord1f32` (`fn_80185408`),
  and dropping them left the plane unprimed.
- `GXSetZTexture` (the Classic team cards: each costume's depth is copied,
  and the tiles are drawn back with that depth so only the fighter shows)
  is emulated as a mask. The NV2A can't replace a fragment's depth from a
  texture, and pbkit's depth buffer is compressed, so it can't be sampled
  either. At a depth-format copy (`GXCopyTex` with a `_GX_TF_ZTF` format)
  `xgx_ztex_mask` draws the copy rect into the framebuffer's alpha: 0, then
  1 where the depth test (GREATER) says the stored depth is in front of a
  quad just before the copy's clear depth. The copy takes that alpha
  (`XGX_COPY_ALPHA`); a draw with `ztex` set gets one more combiner stage
  (`APREV * TEXA` of the last stage's texture, `derive_units`) and an alpha
  test that drops 0 (`emit_fixed`). The mask pass starts with a quad that
  has depth writes on and `NEVER` as its test: it changes nothing, but
  xemu only rebinds its depth buffer for a draw that may write depth, and
  after the EFB colour copy just before (a swizzled target) it had none,
  so every pixel passed. At 720p (R5G6B5, no alpha) the mask goes into
  green instead and the copy reads it as `XGX_COPY_GREEN` (which also
  fills the copy's alpha). That overwrites the rect's colour, so it is only
  done when the depth copy clears the rect after itself, as the team
  card's does (`gm_1832.c`: colour copy first, then the depth copy with
  clear); otherwise the copy stays a colour copy and the draw is unmasked.
- Known wrong in xemu: Mute City's distant skyline band renders as white
  speckle.
