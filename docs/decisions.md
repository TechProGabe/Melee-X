# Decisions

These are the choices the port rests on and why each was made. When
changing one, update this file.

## Scope, agreed at the start

| question | decision |
|---|---|
| Game data | Read the user's own `GALE01` (NTSC-U 1.02, revision 2) disc image at runtime, from `.iso`, `.gcm` or `.ciso` next to `default.xbe`. Nothing from the disc is converted ahead of time or committed. |
| 720p aspect | 16:9 widescreen (hor+) at 720p, using melee-pc's widescreen code. 480 follows the dashboard's 4:3 / 16:9 setting. |
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
compares the result with GCC.

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
Docker build when Pillow is there.

**Vanilla gameplay.** melee-pc's UCF, free camera, frozen stadium,
unlock-all, netplay, Slippi and launcher are off or not built.

## Edits to imported code

Imported files are kept as they are upstream except for these edits, each
marked `PORT:`:

- `src/pc/libm/pc_libm.h`, `pc_rem_pio2f.c`: accept `FLT_EVAL_METHOD == -1`
  under `TARGET_XBOX`. clang reports -1 for SSE float with x87 double, and
  melee-pc's libm is still exact there.
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
- `src/melee/mp/mpisland.c` (`mpIsland_8005A728`, `mpIsland_8005B004`): the
  1.5 KB `visited` arrays, which the code `memzero`s itself, are exempt from
  `-ftrivial-auto-var-init=zero` (it zeroed them a second time per call).
- `src/melee/lb/lb_00B0.c` (`memzero`): `memset` instead of the byte loop,
  which `-ffreestanding` keeps as written (~1% of a console match frame).
- `src/melee/gm/gmscene.c` (`gm_801A4D34`): the render pass is bracketed
  with `xsdk_perf_render_begin/end` (`xbox/src/sdk/vi.c`), so `[PERF]`
  separates simulation ticks from rendering and counts ticks per render.

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

- **Hardware coverage is thin.** It boots on a console and plays VS
  matches; menus run at 60 fps.
- **Performance.** On hardware a 4-player VS match started at 4.5 fps,
  CPU-bound (the `[PERF]` and `[PROF]` lines, `docs/testing.md`). Faster libc
  routines, cached display lists and dirty-driven draw state brought Green
  Greens to 7 fps; the CPU readback of the four shadow maps was then about
  half the frame, and the copies now run on the GPU (unmeasured on the
  console yet).
- **A freeze at TIME! on the console** (4-player Green Greens, results
  never came; xemu reaches them). Not reproduced. The log now flushes
  urgent lines at once and the rest within a second, logs scene and
  match-end transitions with free memory, and the watchdog also reports a
  game that keeps pacing retraces without drawing, writes its report into
  `boot.log` itself, and logs a `[BEAT]` every 5 s. Fixed on the way: the
  profiler thread did file I/O on a 16 KB stack (the kernel's file-system
  path runs on it; now 64 KB, as the watchdog's); the flip and vblank waits
  could spin forever if pbkit masked the GPU interrupt after an interrupt
  storm (now timed, logged, and the interrupt is turned back on); each EFB
  copy no longer costs four GPU-to-CPU interrupts to re-target the back
  buffer.
- **The demand-commit fault handler** relies on the Xbox kernel sending
  kernel-mode access violations on reserved memory to the thread's SEH
  chain. That is how nxdk's `__try` works, but it hasn't been exercised.
