# Melee-X

Super Smash Bros. Melee running natively on the original Xbox. This is not an
emulator: the decompiled game code is compiled for the Xbox's Pentium III and
draws with its NV2A GPU.

- 4 players on the four controller ports
- 480i / 480p / 720p output, with 720p rendered 16:9 (hor+ widescreen)
- Needs your own Melee disc image: NTSC-U 1.02 (`GALE01`, revision 2). No
  game data is in this repository or in any build.

> **Status: early bring-up.** `default.xbe` builds and links: all 1008 game
> units, the Dolphin SDK on the Xbox, and a first GX → NV2A renderer. It has
> not been run yet (no console or emulator here). See
> [docs/roadmap.md](docs/roadmap.md).

## How it is put together

| layer | from |
|---|---|
| game code (`src/melee`, `src/sysdolphin`) | [doldecomp/melee](https://github.com/doldecomp/melee), as adapted to little-endian hosts by [melee-pc](https://github.com/999sian/melee-pc) |
| Dolphin SDK headers (`extern/aurora/include`) | [encounter/aurora](https://github.com/encounter/aurora) (via melee-pc) |
| Xbox platform layer (`xbox/`) | this repo, with pieces carried over from [OpenCrossing-Xbox](https://github.com/GabeConway/OpenCrossing-Xbox) |
| toolchain | [nxdk](https://github.com/XboxDev/nxdk), LLVM 21 |

The disc data stays big-endian in memory, as on the GameCube. melee-pc
marks the on-disc structs `DISC_STRUCT`, which GCC byte-swaps on access.
nxdk is clang, so the Xbox build lowers those accesses to explicit
big-endian loads and stores first (`tools/lower`, melee-pc's browser path).
Details in [docs/architecture.md](docs/architecture.md) and
[docs/toolchain.md](docs/toolchain.md).

## Building

See [docs/toolchain.md](docs/toolchain.md). In short:

```sh
tools/xbox/setup.sh               # once: LLVM 21, nxdk, disc_lower
xbox/build.sh                     # -> build-xbox/xbe/default.xbe
tools/lower/test_lower.py         # lowering oracle tests
tools/xbox/test_vp_encoder.py     # vertex-program encoder vs nv2a-vsh
```

To play: copy `default.xbe` into a folder on the Xbox HDD (e.g.
`E:\Games\Melee-X\`) together with your own `GALE01` disc image (`.iso`,
`.gcm` or `.ciso`, any name), and launch it from your dashboard.

## Licensing

See [LICENSE.md](LICENSE.md). The decompiled game code carries no license;
the port code is GPL-3.0-or-later because it builds on melee-pc.
