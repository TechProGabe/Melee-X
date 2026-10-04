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
| XBE image (game ~5.8 MB + platform) | ~7.5 MB |
| MEM1 (game heaps) | 24 MB reserved as on the GameCube, committed on demand |
| ARAM (preload cache, sound banks) | 16 MB reserved, committed on demand |
| NV2A: 720p R5G6B5 x3 + Z16 | 7.4 MB |
| NV2A: texture pool | 6 MB at 720p, 8 MB at 480 |
| pushbuffer (1 MB) + vertex ring (1.5 MB) | 2.5 MB |
| NV2A: display-list vertex cache | 4 MB (down to 2 MB if short; 720p had 3 MB) |
| network (`xhw_net.c`, only once started: LAN play, test builds' probe) | lwIP and the NIC ~144 KB (64 x 2 KB contiguous receive buffers, rings, kernel pool), 64 KB per open UDP socket; its code (~250 KB) is in the image always |

MEM1 and ARAM are reserved at fixed VAs and committed 64 KB at a time
(`xhw_reserve_lazy`). The first touch of a chunk faults, and the SEH
record every game thread runs under (`xhw_crash_guard`) commits it and
resumes. Memory the kernel writes into (disc image reads) is committed
first with `xhw_commit`, since a fault inside the file system never
reaches that handler. So only the parts of MEM1 and ARAM the game really
uses cost Xbox RAM. `boot.log`'s `[MEM]` lines and `crash.log` report how
much is committed. Melee can touch more than the Xbox has (24 MB of MEM1
and 16 MB of ARAM, with ~35 MB free after the NV2A is up), so ARAM pages
that hold bytes straight from the disc image are left on the disc: their
chunks are decommitted and read back from the image when needed (`ar.c`,
docs/decisions.md). Measured on the console (v36, 480i): ~34 MB free once
the NV2A is up, ~24 MB of MEM1+ARAM committed in a match (another ~11 MB of
ARAM left on the disc), ~7-9 MB free, steady from match to match.

Textures are stored in formats the NV2A samples as is, whenever the size
is a power of two:

| GX format | back-end format |
|---|---|
| CMPR | DXT1: GX's 8x8 tiles are reordered into 4x4 blocks, with the colours byte-swapped and the index bits reversed |
| I4, I8 | AY8 |
| IA4, IA8 | A8Y8 |
| RGB565 | R5G6B5 |
| C4, C8 | I8 palette indices + a 256-entry A8R8G8B8 palette (`SZ_I8_A8R8G8B8`) |

Everything else (RGB5A3, RGBA8, C14X2) and all non-power-of-two images
are decoded to A8R8G8B8.
`tools/xbox/test_tex_convert.py` checks each native format against that
decoder.

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
