# Decisions

These are the choices the port rests on and why each was made. When
changing one, update this file. How each works is in `renderer.md` (GX on
the NV2A), `platform.md` (SDK, settings, audio, cards), `testing.md`
(switches, logs, console rounds), `toolchain.md` (build) and `fps-plan.md`
(frame-rate work); `roadmap.md` has the build history.

## Scope, agreed at the start

| question | decision |
|---|---|
| Game data | Read the user's own `GALE01` (NTSC-U 1.02, revision 2) disc image at runtime, from `.iso`, `.gcm` or `.ciso` next to `default.xbe`. Nothing from the disc is converted ahead of time or committed. |
| 720p aspect | 16:9 widescreen (hor+) at 720p, using melee-pc's widescreen code. 480 follows the dashboard's 4:3 / 16:9 setting. |
| Video defaults | 720p where the dashboard allows it (`720p = 1`, the default since v45), else 480p/480i from the dashboard; `progressive = 0` forces 480i. BACK held at boot gives 480i and saves `720p = 0`, `progressive = 0`, so a TV that can't show 720p is never stuck on a black screen. |
| OpenCrossing-Xbox | Reuse its port layer where it applies: the audio drivers, the crash reporter, the TEV -> register combiner compiler, pbkit patches, video mode rules and hardware notes. |
| Controller layout | GameCube-like by position: A=A, X=B, B=X, Y=Y, White/Black=Z, triggers=analog L/R, right stick=C-stick. It can be remapped per port. |
| Players | 4, one per physical controller port. |

## Technical decisions

### Toolchain

**Start from melee-pc, not raw doldecomp.** melee-pc already runs the
decomp on little-endian hosts: disc structs are marked `DISC_STRUCT` and
stay big-endian in memory, `DISC_PTR` handles pointers in disc data, and
`src/pc` provides the audio mixer, widescreen, libm and more. The upstream
commit is in `src/UPSTREAM_COMMIT`.

**Lower DISC_STRUCT at build time.** melee-pc uses GCC's
`scalar_storage_order`; nxdk is clang-only. `tools/lower/disc_lower`
(LibTooling, from melee-pc's browser build) rewrites those accesses into
explicit big-endian loads and stores, checked against GCC by
`tools/lower/test_lower.py`. Static initializers of disc structs are stored
big-endian in every field (`tests/lower/disc_bitfield_init.c`; when only a
bit-field struct's bit-fields were, Corneria's gun hit every fighter).

**The game triple is `i686-pc-windows-gnu -mno-ms-bitfields`**: GameCube
bit-field layout, 8-byte `long long` alignment, and the calling and
struct-return ABI of nxdk's `i386-pc-win32` (`toolchain.md`).

**LLVM 21.1.8.** nxdk warns that clang 19.x-20.1.2 miscompile it
(llvm/llvm-project#134607), and the release tarball has the clang
libraries `disc_lower` needs. nxdk is pinned to OpenCrossing's commit.

**Compiler builtins for memcpy & co.** (`xbox/include/xbuiltin.h`). Every
unit is `-ffreestanding`, hence `-fno-builtin`; the force-included header
maps memcpy, memmove, memset and memcmp to `__builtin_*` (constant sizes
inline, the rest call `xhw_string.c`) and `sqrtf`/`sqrt` to `sqrtss`/`fsqrt`,
the instructions pdclib's versions wrap.

**Code laid out by profile; releases are ThinLTO + PGO (2026-10-03, the
user's call).** The console spent a third of its render and back-end cycles
on instruction fetch, so `xbox/order.txt` orders the link; ICF stays off
because folded functions would compare equal. ThinLTO (`XBOX_LTO=1`,
inlining only up to 10 instructions: code size costs the 16 KB code cache)
and PGO (`XBOX_PGO`) keep that order and the freestanding image
(`toolchain.md`). They measured +13% on Fountain 4-CPU 720p with `[SIMH]`
and lockstep shots unchanged; the `build` workflow passes both and
`package_release.py` refuses a build without them. Local builds stay
plain. Retrain the profile (`pgo_train.sh`, `docs/pgo.md`) after game or
sdk changes, or the changed functions build without it.

### Runtime model

**The frame boundary is `GXCopyDisp` and `VIWaitForRetrace`** (melee-pc's
model): the flip at `GXCopyDisp`, the VI wait pacing to 60.000 Hz and
running the alarms.

**Alarms and interrupts run on the game thread.** `OSDisableInterrupts` is
a recursive lock, which avoids true asynchrony in code written for a
single-core console. Alarms (pads, card and DVD completions) are delivered
when interrupts are re-enabled, at most every ~0.3 ms (`os.c`
`deliver_pending`; HSD re-enables thousands of times a frame), and at every
frame boundary.

**Memory: a GameCube-sized MEM1 and ARAM, committed on demand.** Shrinking
the arena would change the heap layout, riskier untested than committing
what the game touches. 720p runs at 16-bit colour, as OpenCrossing measured
it had to; textures use native NV2A formats.

**Disc-backed ARAM pages** (`ar.c`). Preloads into ARAM had committed 31 MB
by the first match, 1.3 MB from out of memory. A copy into ARAM of bytes
the DVD worker just read only records where on the image each 4 KB page
is; a 64 KB chunk wholly on the disc is decommitted, ARAM -> MEM1 transfers
read the image, and a CPU touch faults and refills the chunk
(`xhw_lazy_set_fill`). Numbers: `architecture.md`.

**128 MB consoles always run in 64 MB.** The early mid-match GPU stalls
came from a 128 MB console, and every test machine has 64 MB:
`xhw_mem_hold_upper` takes the RAM above 64 MB at every boot, first thing
in `xsdk_early`. Up to v52 `[system] ram128 = 1` skipped that; v53 drops
the setting and its menu row (user's call, 2026-10-02): the game's 64 MB
budget gains nothing from more RAM, and one memory layout is one less
thing to rule out in a report. Old files' `ram128` lines are ignored and
not written again. Untested in xemu (a stock BIOS reports 64 MB).

**Our own memcpy, memmove, memset and memcmp** (`xhw_string.c`, 32 bits at
a time; pdclib's byte loops were ~40% of a VS frame), and **disc image
reads straight to the kernel** (`xhw_file_read`: a few 1 MB reads, not
pdclib's 1 KB `ReadFile`s copied byte by byte).

**Memory-card files are big-endian, as on the GameCube**, contents
included, so saves move to and from Dolphin and real cards. Save data and
name tags are converted at the card boundary (`card_endian.c`) by field
tables built with `offsetof`/`sizeof`, so a layout change fails the build
or `test_card_endian.py`. Writes go from a big-endian copy, never by
swapping the live buffer (the write spans frames). Early builds'
little-endian saves are recognised by a plausibility vote and converted by
the next save (`platform.md` CARD).

**Logs that survive a long session.** `boot.log` keeps the first 4 MB, then
the log alternates between `boot2.log` and `boot3.log` (2 MB each); the
previous boot's is kept as `boot_prev.log`; `[DRAW]` traces go to
`trace.log` (`testing.md` "Logs").

### Rendering

**Our own GX implementation, not aurora's.** aurora records the GX FIFO
and compiles TEV to WebGPU through Dawn, none of which exists on the Xbox.
Its public headers are kept, so the game compiles unchanged, and GX is
implemented directly on the NV2A (`docs/renderer.md`).

**Generated vertex programs**, per configuration: GX lighting, skinning and
texgen exceed the NV2A's fixed function (skinning indexes matrices with
`a0`). They are optimized and placed in the 136-instruction program memory
by Belady's rule with the last frame as the forecast (the console had been
reloading ~80 programs a frame). Only rewrites with the same output bits
are allowed (`test_vp_opt.py`); the attenuation shortcut relies on
rcp(1.0) being exactly 1. `renderer.md` "Shorter programs".

**Fewer, bigger draws** (`gx_vtx.c`): a display list's batches merge into
one draw (strips stitched with degenerate triangles), immediate-mode
batches with the same state join, quads and fans go out as triangle lists,
array offsets stay fixed. Per-draw cost dominates on the console and in
xemu (a 4-CPU match: ~2950 -> ~800 draws); the cost is 1.5x vertices for
quads, 2-3 more per stitched strip.

**Immediate mode is a typed API over a byte stream.** Each `GXPosition*`,
`GXColor*`, `GXTexCoord*` call fills its attribute, but the raw writers
(`GXTexCoord1f32`, `GXCmd1u8`) fill whatever the vertex descriptor puts
next, as the GameCube FIFO would: game code uses them as plain f32/u8
writes (HSD's particles, the Classic card's depth plane). Decoding the
whole FIFO by bytes would be exact but slower on every call; the typed
path plus these two cases covers what the game does, and `[DLC]`'s `short`
count shows a batch that still doesn't fit (`renderer.md`).

**Pools and GPU overlap** (`renderer.md` "CPU cost of the back end"). Each
pushbuffer batch breaks the vertex cache (shadow-map wedges otherwise); a
scene whose textures outgrow the pool gets an overflow pool (up to 8 MB)
until the next scene change; stable lists and textures are revalidated
every fourth frame by sampling, so one rewritten in place may draw stale
for up to three frames. The GPU finishes a frame and its flip while the CPU
starts the next (the wait for idle is at the next frame's first GPU use,
`frame_open`). `-DXGX_OVERLAP=0`, `-DXGX_PB_KICK`, `-DXGX_VB_CACHE_BREAK=0`
and `-DXGX_VBUF_FREE_NOW=0` undo these one at a time.

**GX fog per vertex, from GX's own registers** (`nv2a_fog.c`). The NV2A has
no per-pixel depth fog, so the vertex program computes GX's fog amount from
the registers `GXSetFog` would write (A and C cut to 11 mantissa bits,
b_mag, b_shift: with Melee's camera that makes far fog up to 1.57x thinner
than the textbook formula). The cost: less fog mid-span on long polygons
(up to ~23/255), EXP curves linear between vertices, no `GXSetFogRangeAdj`.

**`GXSetZTexture` as a mask.** The depth test at a depth copy writes a 0/1
mask into alpha (green at 16-bit, only for a copy that clears its rect),
and the Z-texture draw alpha-tests by it. Exact for Melee's use (the
Classic team card), not a general depth replace.

**TEV swap tables as dot products.** A swizzled source (GXInit's `RRRA`
and the like) costs one combiner stage dotting it with a unit vector;
identity and alpha broadcast are free; permutations are approximated by the
identity (Melee sets none). Per-table texture copies would not fit tables
being per TEV stage. Unswizzled draws compile as before (`test_rc.py`).

**Indirect texturing as BUMPENVMAP.** The cloak's refraction (`lbRefract`,
Melee's only indirect user) maps onto the bump-environment unit, from a
copy of the indirect map holding its offsets halved (filtering never
crosses the unit's two's-complement wrap). A dependent read replaces the
coordinate instead of offsetting it, and dot-product stages take three
units. Anything else draws direct; `-DXGX_NO_INDIRECT=1` turns it off.

**720p: EFB copies on the GPU, Z16 depth remapped.** 16-bit EFB copies are
drawn by the GPU into R5G6B5 textures (the CPU readback cost ~60 ms of a
133 ms frame); they sample alpha 1, which Melee doesn't rely on. R5G6B5
forces Z16 depth, and Melee's 0.1..16384 camera then z-fights near the
fighters. Z24S8 needs 32-bit colour (~7 MB more), float Z16 needs reversed
depth everywhere and xemu can't check it; so one affine remap of GX's depth
per frame, (g - g0) / (1 - g0), from the frame's projections (`build_proj`,
`-DXGX_Z16_DEPTH_RATIO`, `renderer.md` "Depth"), one mapping for every
camera since the game depth-tests the timer's camera against the stage's.
480 is unchanged; `-DXHW_VIDEO_480_BPP=16` brings the 16-bit path to xemu.

**The colour tile is on (`XGX_TILE` 4, 2026-10-03).** pbkit puts the
framebuffers in tile 0 without its enable bit; enabled, it measured +11% at
720p on Fountain (console round 4) with byte-identical lockstep shots. Z
compression stays as pbkit sets it. CPU access to a framebuffer (settings
menu, screenshots, CPU EFB readback) goes through the NV2A aperture at
0xF0000000 + physical (`nv2a.c` `fb_cpu`), which sees through the tile; the
kernel's crash and error screens are outside tile 0. `env MX_TILE=` and
`-DXGX_TILE` pick other masks (`OCX_Z16_TILE_FLAGS` is superseded).
Unchecked on the console at 480p. `renderer.md` "Tile regions".

**EFB copies send their surface switches twice.** GPU stalls after an EFB
copy (`LIMIT_ZETA`, `LIMIT_COLOR`) each lost one colour-surface write.
Sending the DMA objects, pitch and offsets again after a wait for idle
(`-DXGX_COPY_FIX=5`, the default) held 22 min under 40x copy stress; why the
write is lost is not known. `XGX_COPY_STRESS` and `scenarios/stall` stay
for the next copy stall (`testing.md`).

**EFB copies outlive the idle release (after v51).** Textures unused for
600 frames are released, EFB copies no longer: Pokémon Stadium's screen
binds its copy's destination before the frame's copy, and after ~10 s of
other views got an upload of memory the GPU copy never writes (a garbage
frame) where the GameCube shows the copy still in memory. Copies stay until
evicted, replaced, or idle at a scene change (Stage Clear's freeze frame,
in use, crosses it); an idle copy binds only at its copied size. In LRU
eviction a destination copied to again counts as 3600 frames younger
instead of 60, so re-uploadable textures go first. Dropping copies on a
changed destination hash broke Stage Clear's freeze frame.

**Fighter reflections that draw nothing are skipped.** Fountain of Dreams'
reflection draws every fighter's body even out of its view (~265 of ~1000
draws with four Foxes); the body is skipped when the game's own culling
sphere is outside the reflection's frustum (edit below).

### Platform features

**Dashboard icon and title ID.** `tools/xbox/xbe_title_image.py` (from
OpenCrossing-Xbox) adds a `$$XTIMAGE` section and writes `default.tbn`,
`TitleImage.xbx` and `TitleMeta.xbx` from `xbox/assets/logo.png` (original
art), and sets the title ID to 4D580001 (the `E:\UDATA` save folder):
nxdk's FFFF0002 is shared by every nxdk title, and dashboards showed
another homebrew's cached icon.

**Release builds have no test tools.** A plain build is a release: BACK
screenshots and the frame-rate counter are off unless the settings menu or
`settings.ini` turns them on. Profiler and autopad builds imply
`XHW_TEST_BUILD`, which defaults both on (`testing.md`).

**The settings menu is the platform's, over the title screen.** BACK on the
title opens it (`menu.c`, `platform.md`). A page in Melee's Options menu
would mean new model data and many imported-code edits; the title is one
scene, so one `PORT:` hook freezes it while the menu is up. The text is CPU
writes into the finished frame (`xhw_overlay.c`): no GPU state, the same at
every mode. Video mode and widescreen changes wait for "Save and
restart", which relaunches the XBE. `settings.ini` is written to a
temporary file, read back and renamed over the old one.

**Front LED effects, off by default (after v48).** KOs, a timed match's
last seconds and GAME! can drive the front LED through the SMC
(`xhw_led.c`, `platform.md` "Front LED"). On by default from v43, until a
user's Kronos modchip fought them over the LED and that console needed a
Cerbios recovery; the key is now `led_effects` (an old `led` line is
ignored) and the README warns. A lowest-priority worker does the SMBus
writes, on changes and at most every 80 ms; events come from three small
`PORT:` calls rather than polling game state.

**The AC97 is left idle before a relaunch (after v47).** Save and restart
and quitting go through `XLaunchXBE`. The old shutdown didn't wait for the
pump thread (which could restart the engine) and never reset the bus
masters, and the next boot had the engine stuck ("AC97 stuck: civ 0").
`xhw_audio_shutdown` now waits for the pump, then stops and resets both bus
masters. A crash still leaves the engine running (next entry).

**The audio hardware is brought to idle at boot, and a stuck engine is
given up (roadmap item 9, 2026-10-04).** A console also booted silent after
a power-off and the dashboard, engine stuck on CIV 0 from the first buffer
and each recovery freezing the game ~1 s. Whatever ran before an XBE can
leave the AC97 and the APU (whose DSPs feed the AC97's bus masters) running
or half set up, and a quick reboot resets neither. The boot now logs both
as found (`[AUDIO] found`), then before the cold reset enables the AC97's
PCI memory and bus mastering, halts both bus masters, and stops the APU's
interrupts, setup engine (`SECTL` 0) and DSPs (`GPRST`/`EPRST` 0; AC97 path
only); after it, it powers the codec up if register 0x26 reports a block
off (`[AUDIO] idle`). Which of these sticks is not known yet: the next
silent boot's `found` lines tell. If the engine still finishes no buffer
from the boot on, after one recovery cold reset the driver gives up: one
`[AUDIO] stuck since boot` line, the pump consumes the ring in real time so
the game runs on, and a 10 s notice (`xhw_notice`) says to switch the Xbox
off and on. An engine that played and stops later keeps the old recovery.
Each boot also logs how the one before ended (`lastexit.txt`, written on
every exit that runs code; none means switched off, reset or killed). A
healthy boot's audio is unchanged. `-DXHW_AUDIO_TEST` leaves the engine
running across a relaunch or fakes a stuck one, for xemu (`testing.md`
"Audio at boot").

**Writes to a full or unwritable E: (2026-10-04).** A player reported
settings.ini never made, no BACK screenshots and widescreen doing nothing
(all files in `E:\UDATA\4d580001\`; widescreen at 480 also needs the
dashboard's flag). His E: has over 1 GB free, so the cause is open, but
every write there failed quietly, and a card save rewrote its `.gci` in
place, so a failed write truncated it. Now:
- Boot logs E:'s free space (`[BOOT] E: N MB free`), the dashboard's video
  flags (`[VIDEO] dashboard`) and a `probe.tmp` write test (`[BOOT] save
  folder write test`); a failed settings save logs errno.
- When boot.log can't be created on E: or E: has under 1 MB free, the log
  goes to `D:\boot.log` (next to default.xbe), a 10 s notice says so, and
  the title's hint line replaces "BACK: Melee-X settings" for the run.
- Card saves write `<name>.tmp` and rename it over the `.gci`
  (`xhw_replace_file`): the old save survives, the game gets
  `CARD_RESULT_IOERROR`, a failed create or rename is undone, and the next
  boot keeps a complete `.tmp` whose `.gci` is gone and deletes any other.
- A BACK screenshot that can't be written is deleted. Each failure posts a
  notice (`xhw_notice`, `xhw.h`; one posted while another is up is logged
  as `[NOTICE] dropped`).

**Vanilla gameplay.** melee-pc's UCF, free camera, frozen stadium,
unlock-all, netplay, Slippi and launcher are off or not built.

### Measurement

**Measure before changing** (`fps-plan.md`, `testing.md`). In xemu,
`-icount shift=1,sleep=off` makes guest time count instructions, so
`[PERFX]` and `icount_report.py` give instructions per draw and per tick,
repeatable within 0.6% (the pad-alarm wait is charged to `vsync`). xemu has
no caches, so layout and cache gains are decided by the console's `[PERF]`.
`[SIMH]` hashes each fighter's state and the seed every 60 ticks: toolchain
and simulation changes must leave it unchanged. Lockstep (`env
MX_LOCKSTEP=1`, test builds) makes the SDK clock 1/60 s per frame boundary,
so screenshots of builds of different speed compare at the same tick.
Profiler builds also write each match's whole profile once (`prof.bin`,
`[PROFH]`), since the periodic `[PROF]` report holds only ~60% of samples.

**Tried on the console and removed** (`fps-plan.md`), none changing what is
simulated or drawn: a deferred back end replaying draw records (B4, -6%);
MEM1 on 4 MB pages via the page directory's self-map (B2, +1.7% then
nothing, pages unknown to the kernel); planned prefetch of animation walks
(B3, within noise); cheaper identity pixel state (`XGX_TRIM`, almost no draw
qualifies).

## Edits to imported code

Imported files are kept as they are upstream except for these edits, each
marked `PORT:` (the `TARGET_PC` ones in `lbdvd.c`, `lbfile.c`'s
`discIsDone` and `gmscene.c`'s pad-alarm wait were in melee-pc at import).

Math and libraries:
- `src/pc/libm/pc_libm.h`, `pc_rem_pio2f.c`: accept `FLT_EVAL_METHOD == -1`
  (clang's SSE float with x87 double; the libm stays exact).
- `src/pc/discfont.c`: the DOL is read through the DVD layer
  (`DVDGetDOLLocation`), as in the emscripten build, not `nod`.
- `extern/aurora/lib/dolphin/mtx/mtx.c` (`C_MTXConcat`): four columns at a
  time in SSE, same operations per lane, bit-identical.

Game fixes:
- `src/melee/ft/ft_0D31.c` (`ftCo_DeadUpStar_Anim`): the star KO reads
  `x508`..`x51C_radians` by name, not past a local copy of `x504` (the stack).
- `src/melee/lb/lbfile.c`, `ft/ftdata.c` (two places), `gr/grdisplay.c`:
  "is this ARAM?" is `PC_IS_ARAM_ADDR`, not `< 0x80000000`.
- `src/sysdolphin/baselib/hsd_3A76.c`: the default kerning table is indexed
  by glyph number, not a byte offset.
- `src/melee/ft/ftparts.c` (`ftPartsRemap`): the joint byte is unsigned
  ("no such joint" became `fp->parts[-1]` on cross-character throws).
- `src/melee/ft/kinds/ftPikachu/ftpikachuspeciallw.c`: Thunder's entry
  clears `speciallw.x0` by name (melee-pc moved it out of `specialhi.x0`).
- `src/melee/gr/grpstadium.c` (`grStadium_801D4548`): transformations are
  picked from `{3, 4, 6, 9}` as upstream doldecomp does (`HSD_ASSERT(0xA44)`).
- `src/melee/gr/grpstadium.c` (`grStadium_801D42B8`): the transformation
  file is parsed once, after its load (hung in `HSD_ArchiveLocateExtern`).
- `src/melee/ft/kinds/ftKirby/ftkirby.c` (`ftKb_LoadHatParts`): the copy
  ability's part visibility and texture list (`u.kb.x44`) are set up as
  upstream does (crash in `ftAnim_80070458` on knockback).
- `src/melee/gr/grbigblue.c` (`grBb_YakumonoParam`): `DISC_STRUCT`, with
  `x134_translate` a `DiscVec3` (read byte-swapped, a match stopped at once).
- `src/sysdolphin/baselib/texp.c` (`HSD_TExpSetReg`): `reg[8]` is static,
  starting white; zeroed locals blacked out half-constants (Kirby's Falcon
  helmet) where the GameCube's stack held leftovers.
- `src/melee/gm/gm_1798.c` (`fn_80179990`): the 1st-place branch reads
  `lbl_8046E3AC` itself, not `ResultsDisplayLayout`'s overlay (the winner's
  portrait drew black). `fn_80179854` (deviation): a Debug VS tie sets
  `x0_6` too, so the winners' portraits are copied.
- `src/melee/if/ifall.c` (`ifAll_802F370C`): the wide HUD doesn't move the
  match timer (top centre; `pc_widescreen_hud_timer_x` put it off-centre).
- `src/melee/lb/lbsnap.c` (`lbSnap_8001DC0C`): the snapshot header's `x14`
  is swapped to big-endian like the rest.
- `src/melee/lb/lbcardgame.c`: save data and name tags go to the card
  big-endian from a static copy (`card_image`, `toCardOrder`) and are
  converted after a read (`fromCardOrder`, `card_endian.c`); `lb_803BAB60`
  (`CardIconInfo`) is spelled as the GameCube's bytes (banner and icon).
- `src/melee/gm/gmonlinemode.c` (`gm_Scene_OnlineLobby_OnFrame`): the
  online/LAN lobby returns to the menu (it crashed on the stubs' NULLs).
- `src/melee/ty/toy.c` (`Toy_8030813C`, `_Toy_8030663C`): a trophy missing
  from the model table shows the first trophy instead of panicking; a
  `[TOY] sort:` line logs the sort's rows.
- `src/melee/ft/ftparts.c` (`ftParts_80074D7C`): a visibility group whose
  table or non-empty index list points outside MEM1/ARAM is skipped and
  logged once per kind (`[WARN] ftParts:`).
- `src/melee/lb/lbaudio_ax.c` (`lbAudioAx_80027648`): a needed SSM that
  failed to load with nothing pending reloads bank 2 once, then is dropped,
  instead of waiting forever.
- `src/sysdolphin/baselib/synth.c` (`HSD_SynthSFXHeaderLoadCallback`): an
  SSM that overflows its bank is dropped without its callback (it was
  requeued forever); `lbaudio_ax.c` (`lbAudioAx_80023B24`, the sound test)
  waits for loads in flight before emptying bank 2.
- `synth.c` (v53): `HSD_SynthSFXUnloadBank` and `HSD_SynthSFXGroupDataRemove`
  change the bank lists and the sound-effect hash under the interrupt
  lock, which the load completions (DVD/ARQ worker threads) hold; on the
  GameCube those completions were interrupts and couldn't land mid-change.
  Hash walks (`HSD_Synth_80389334`, `HSD_SynthSFXDataUnlink`) check each
  entry lies inside the audio heap's allocations (`pc_sfx_entry_ok`); a
  broken link skips the sound with `[WARN] sfx N: bucket B chain broken`
  instead of crashing (tester crash, v52: `HSD_SynthSFXPlayWithGroup`
  reading address 3 after 14 VS matches). With it, `OSAlloc` (os.c) takes
  its own lock: the audio heap is allocated from on those workers and freed
  on the game thread.
- `src/melee/gr/granime.c` (`grAnime_801C8318`): the AObj that
  `fn_801C82E8` longjmps back with is a `volatile` local (clang returned
  NULL, so stage "animation ended" checks never fired: Kraid froze).

Speed (same results, bit for bit where a test is named):
- HSD animation and matrices (`test_anim_mtx.py`, `anim_mtx_ref.c`; a NaN
  only has to stay a NaN):
  - `src/sysdolphin/baselib/fobj.c`: `1.0 / fterm` in float, `splGetHelmite`
    inlined (`FObjHermite`), `parseFloat` without a division.
  - `src/pc/libm/pc_sincosf.c` (new), `pc_trig.h`: sinf and cosf of one
    argument, used by `src/sysdolphin/baselib/mtx.c` (`HSD_MtxSRT`).
  - `src/sysdolphin/baselib/mtx.h` (`HSD_MtxConcatScaledAdd`): the envelope
    blend as one SSE step per row, used by `pobj.c` and
    `src/melee/ft/ftparts.c` (`ftPartsSetupEnvelopeMtx`).
  - `aobj.h` (`HSD_AObjIsPlaying`), `jobj.c`, `mobj.c`, `tobj.c`, `robj.c`,
    `pobj.c` (`*Anim`): no call for an object with nothing to animate.
- `src/sysdolphin/baselib/mtx.c` (`HSD_MtxInverseTranspose`): the
  cofactors as three SSE rows read by 16-byte loads (`test_pobj_mtx.py`).
- `src/sysdolphin/baselib/pobj.c` (`SetupEnvelopeModelMtx`), `dobj.c`
  (`HSD_DObjDisp`): within one DObj a repeated envelope reuses its matrices
  (memo cleared per DObj); new ones are computed into the memo, prefetched,
  and their locals are `POBJ_NOINIT` (`test_pobj_mtx.py`, `pobj_mtx_ref.c`).
- `src/sysdolphin/baselib/dobj.c` (`HSD_DObjDisp`): a rigid DObj whose
  cached lists lie outside the view (`gx_dl_culled`, `gx_vtx.c`, rehashed
  after 16 frames) is skipped before material setup (Mute City 1391 -> ~525
  draws in xemu).
- `HSD_PREFETCH` (`xbox_game_prelude.h`), hints for the next nodes in
  `jobj.c`, `dobj.c`, `pobj.c`, `mobj.c`, `tobj.c`, `fobj.c`, `aobj.c` and
  `displayfunc.c`'s list walks.
- `src/melee/mp/mpisland.c` (`mpIsland_8005A728`, `mpIsland_8005B004`):
  the `visited` arrays, `memzero`ed by the code, skip
  `-ftrivial-auto-var-init`.
- `src/melee/lb/lb_00B0.c` (`memzero`): `memset` instead of a byte loop.
- `src/melee/lb/lbarq.c` (`lbArq_80014BD0`): a blocking ARAM load calls
  `ARQFlushQueue` first, so completions run on the game thread at once
  (`fps-plan.md` C4); same callbacks, same order.
- `src/pc/audio.c` (`test_audio_mix.py`, `audio_mix_ref.c`, also as plain
  C with `PC_AUDIO_SCALAR`): `decode_samples` decodes a frame's samples a
  run at a time (ADPCM in s32, no per-sample checks); `mix_voice` steps the
  source positions first (`src_steps`), then mixes four outputs a step in
  SSE1 (`src_interp`, `mix_out`, `mix_out_dry`), silent voices in closed
  form, frames over `SRC_MAX` per sample (`mix_voice_stepped`);
  `clamp_frame` in SSE1 passing NaNs as the scalar code; `axfx_reverb_run`
  runs to the next line wrap without per-sample checks.
- `src/melee/mp/mplib.c`: the stage-collision line loops skip a line (or
  vertex) whose full test could only fail its bounding test (`mpLineBox`,
  `mpEdgeBoxOut`); the full tests were ~10% of the console's simulation.
  Exact: each skip needs a line strictly outside the query's box where
  `mpLineIntersection` (strict compares) and `mpLineIntersectionH`/`V`
  (strict on one axis, 0.0001 on the other) return false at their first
  test and write nothing, and nothing between skip and test has a side
  effect (`mpCheckFloor`'s callback runs before it). The query box is NaN
  (no skip) unless its coordinates are inside +-2^16, so roundings are
  under 2^-6; `fl(z) < f` implies `z < f`, and rounding is monotone:
  - `mpCheckLeftWall`/`RightWall`, and the `Remap` variants on a joint
    without B8-B10 (the query is a-b itself): the vertices as tested,
    margin 1.
  - `mpCheckFloor`: the box of the raw vertices, margin 4, the query's y
    less `y_offset`, only for lines at least 1 long (`mpLib_8004ED5C`'s
    `len2`, finite). With d = sqrtf(len2) >= 1, |dx|/d <= 1 + 2^-22, so the
    start moves at most 1.00001; the end moves (x1 - X0)/d with
    |x1 - X0| <= |x0 - x1| + 2|e0|: at most 3.00001. Past rounding (or a
    vertex beyond 2^20), the lengthened line is still over 0.7 clear.
  - The `Remap` variants on a moving joint: `mpRemap2d` moves a by at most
    |b0 - a0| + |b1 - a1| (t clamped to [0, 1]; NaN only from a NaN input)
    or |b0 - a0| + |b1 - a0| (its other branch); `mpRemapReach` adds the
    three plus 1 and widens the box by that (no skip above 2^17 or for an
    infinite or NaN reach). Clear by over 1.9.
  - `mpLib_800511A4_RightWall`/`800515A0_LeftWall`: per vertex, the segment
    (last position remapped from edge a-b to c-d) to (position) against
    c-d: the position is compared with c-d's box as `mpLineIntersection`
    does, the last position with that box widened by the remap's reach; a
    remapped NaN or infinity fails `SQ(vdx) + SQ(vdy) > 0.001F` or the
    bounding test anyway. Clear by over 0.6.
  `tools/xbox/test_mplib.py` checks returns, outputs, joint flags and
  callback calls bit for bit against `tests/xbox/mplib_ref.c` (x86-64 and
  32-bit x87); on a Fountain-like stage it skips 81-99% of the tests.

Logging, test hooks and Xbox features:
- `src/melee/gr/grcastle.c`: `MELEE_DEBUG_CASTLE_BILL=<frames>` (test
  runs) sets the Bullet Bill timer and logs its states as `[CASTLE]` lines;
  inert without the variable.
- `src/melee/gm/gmclassic.c`: melee-pc's Classic test hooks moved into
  `pc_classic_stage_override`, also called by `MELEE_BOOT_SCENE=classic`.
- `src/melee/gm/gmvsmode.c` (`onEnterDebugVs`): `MELEE_DEBUG_VS_TIME`,
  `_CHARS`, `_ITEMS`, `_INVISIBLE`; `ftkirby.c` (`ftKb_Init_OnDeath`):
  `MELEE_DEBUG_KIRBY_HAT`. `TARGET_PC`, inert without the variables.
- `src/melee/gm/gmboot.c`, `gm_1A3F.c` (`runGameMode`):
  `MELEE_BOOT_SCENE=cutscene` boots the debug cutscene mode at state
  `MELEE_BOOT_CUTSCENE` (`scenarios/cntalk`). `TARGET_PC`, inert without
  the variables.
- `src/melee/gm/gm_1A3F.c` (`gm_801A4014`): scene enters and leaves are
  logged with free memory (`xsdk_scene_log`).
- `src/melee/gm/gmvs.c`: `[GAME]` lines at a match's end and at the
  hand-over to the results.
- `src/melee/ft/fighter.c` (knockback): the first 32 hits a boot not from a
  fighter, or with knockback over 200, are logged (`[WARN] hit:`, `[WARN]
  hit by item kind`).
- `src/melee/gm/gmscene.c` (`gm_801A4D34`): `xsdk_perf_render_begin/end`
  around the render pass (`[PERF]`), `xsdk_sim_tick` after each tick
  (`[SIMH]`).
- Draw census and probes: `src/sysdolphin/baselib/gobj.c` (`render_gobj`,
  `HSD_GObj_80390FC0`), `src/melee/lb/lbshadow.c` (`lbShadow_8000F38C`),
  `src/melee/gr/grizumi.c` (`grIzumi_801CCEA0`) set `xgx_census_tag`
  (`xgx_probe.h`); the last two skip the shadow map or reflection when
  `xhw_ablate` says so (probe builds only, `-DXHW_PMC=1`).
- `src/melee/gm/gmtitle.c` (`gm_Scene_Title_OnFrame`): calls
  `xsdk_menu_title_frame` first; while the settings menu is up it returns
  early and holds the attract timer at 0.
- Front LED: `src/melee/ft/ft_0D31.c` (`ftCo_800D34E0`) calls `xhw_led_ko`;
  `src/melee/gm/gmvs.c` calls `xhw_led_timer` (last 10 seconds) and
  `xhw_led_match_end`.
- `src/melee/ft/ftdrawcommon.c` (`ftDrawCommon_80080C28`, `80081140`,
  `80081118`): during grIzumi's reflection a fighter whose camera-box sphere
  (`bone_pos`, `ext.v.z + 15`, as `Camera_80030CFC`) is outside the frustum
  skips `HSD_JObjDispAll` of its body; the rest runs as before.

Game files are compiled with `-Werror=implicit-function-declaration`. The
prelude renames `acosf`, `atan2f`, `asinf`, `expf` and `powf` after
`<math.h>`, so their prototypes have to be in the prelude: without them
every call read its float result from EAX.

To sync a newer melee-pc:
1. Copy its `src/melee`, `src/sysdolphin`, `src/pc` and the headers again.
2. Re-apply the `PORT:` edits.
3. Update `src/UPSTREAM_COMMIT`.
4. Regenerate `xbox/src/sdk/stubs.c` if the link reports new `pc_*` hooks.
5. Run the lowering tests and the host tests that check edits bit for bit
   (`test_anim_mtx`, `test_pobj_mtx`, `test_audio_mix`, `test_mplib`).

## Known risks

- **Hardware coverage.** VS matches on many stages, menus, the Trophy
  Collection, saves and a 100% save run on the console over 30-minute
  sessions; single-player modes and many stages have seen less play. A
  stage-specific crash has so far meant a disc struct missing
  `DISC_STRUCT` (Big Blue). xemu differs from the console in ways that hid
  real bugs (w-buffer, PFIFO timing, AC97): check rendering on the console.
- **Performance.** The simulation keeps 60 ticks; matches draw 23-60 fps on
  the console (v52 median 46 across Stadium and Fountain, Fountain 4-CPU
  720p ~40; `roadmap.md`).
- **Rendering gaps**: indirect texturing only as BUMPENVMAP does it (checked
  in xemu only, whose BUMPENVMAP reads unsigned textures as two's
  complement), fog per vertex, TEV swap-table permutations; the texture and
  display-list vertex pools run full in long sessions (they evict and
  rebuild).
- **Netplay** is not built; the online/LAN lobby returns to the menu.
- **The demand-commit fault handler** relies on the kernel sending
  kernel-mode access violations on reserved memory to the thread's SEH
  chain. Every boot depends on it (MEM1 and ARAM commit this way) and it has
  held since the first hardware builds; a fault at raised IRQL still can't
  be handled (`testing.md`, Crashes).
