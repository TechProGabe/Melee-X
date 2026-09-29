# Architecture

## Layers

```
user's GALE01 disc image (.iso/.gcm/.ciso next to default.xbe)
        |
src/melee, src/sysdolphin      game code: doldecomp/melee as adapted by melee-pc
                               (upstream commit in src/UPSTREAM_COMMIT); disc
                               data stays big-endian, DISC_STRUCT accesses are
                               lowered to BE loads/stores at build time
        |  Dolphin SDK calls (extern/aurora/include: GX OS DVD PAD VI AX AR CARD MTX)
        v
xbox/src/sdk                   Dolphin SDK implemented for the Xbox, game triple
xbox/src/hw                    kernel, pbkit (NV2A), AC97, USB pads; nxdk triple
        |
nxdk (pdclib, winapi, pbkit, SDL2 gamecontroller, xboxkrnl)
```

melee-pc's `src/pc` is used where it is platform-neutral: `audio.c` (software
AX mixer; only its SDL3 output is swapped for the AC97 driver), `widescreen.c`
(hor+ 16:9 and HUD anchoring), `vtxarray.c`, `discfont.c`, `region.c` and
`libm/`. Its netplay, launcher, updater, SLP replays and texture packs are not
built; the game's calls into them hit stubs that report "inactive".

## Why not aurora

melee-pc runs on aurora, whose GX records the FIFO and compiles TEV into
WebGPU pipelines through Dawn. None of that exists on the Xbox, and a
733 MHz Pentium III cannot afford a second command-processor pass. The Xbox
keeps aurora's public C headers, so the game compiles unchanged, and
implements them directly (the same way OpenCrossing-Xbox implements GX over
its NV2A shim).

## Data model (32-bit)

The Xbox is a 32-bit little-endian x86, so the melee-pc data model is simpler
here than on x86-64:

- `DISC_PTR` slots are 32 bits and every host pointer fits
  (`pc_encode_dp`/`pc_resolve_dp` take their `UINTPTR_MAX <= UINT32_MAX`
  path, as on wasm32). No 0x80000000 mapping is needed; that address is
  kernel space on the Xbox anyway.
- Runtime structs have GameCube-sized pointers again, so the game's heaps
  fit in GameCube-sized arenas (melee-pc needs 96 MB for 64-bit pointers).
- `PC_IS_ARAM_ADDR(a)` treats anything below 16 MB as an ARAM offset. The
  XBE image loads at 0x00010000, so the MEM1 arena must be allocated at a
  fixed VA above 16 MB (`NtAllocateVirtualMemory` with a base), and nothing
  in the XBE image may be handed to the DVD/ARAM paths as a destination.

## Memory (64 MB)

| item | size |
|---|---|
| XBE image (game ~5.8 MB + platform) | ~7 MB |
| MEM1 arena (game heaps) | 20 MB, measure and trim |
| ARAM (preload cache, sound banks) | 16 MB on GameCube: disc-backed pages, as OpenCrossing does |
| NV2A: 720p R5G6B5 x3 + Z16 | 7.4 MB |
| NV2A: texture pool | 5-8 MB |
| pushbuffer + vertex ring | 2 MB |

## Video

- 480i/480p: 640x480x32. 720p: 1280x720, R5G6B5 + Z16 (32-bit colour at
  720p does not fit, OpenCrossing's measurement).
- 16:9 at 720p (and optionally at 480p for widescreen TVs) uses melee-pc's
  hor+ widescreen: the camera projection is widened, HUD elements anchor to
  the screen edges, full-screen overlays opt out with `PC_COBJ_FILL_FRAME`.

## Input

Four Duke/S controllers through nxdk's SDL2 GameController, one per port,
mapped to GameCube pads (`PADStatus`):

| GameCube | Xbox |
|---|---|
| A / B | A / X |
| X / Y | B / Y |
| Z | White (Black also) |
| L / R analog + click | left / right trigger (click past 90%) |
| control stick | left stick |
| C-stick | right stick |
| Start | Start |
| D-pad | D-pad |

Remappable per port in `settings.ini`. Rumble maps to the pad's motors.
