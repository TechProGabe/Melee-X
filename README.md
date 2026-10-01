# Melee-X

Super Smash Bros. Melee running natively on the original Xbox. This is not an
emulator: the decompiled game code is compiled for the Xbox's Pentium III and
draws with its NV2A GPU.

- 4 players on the four controller ports
- 480i / 480p / 720p output, with 720p rendered 16:9 (hor+ widescreen)
- Needs your own Melee disc image: NTSC-U 1.02 (`GALE01`, revision 2). No
  game data is in this repository or in any build.

> **Status: plays on a real Xbox.** Menus run at 60 fps. 4-player matches
> run the game itself at full speed (60 ticks a second) and draw 25-30 fps
> on most stages (Pokémon Stadium ~28, Fountain of Dreams ~28 since the
> display-list cache took its stage model). Saves, audio, rumble, fog and the
> dashboard icon work, and GameCube/Dolphin saves load (the save data is
> converted between the card's big-endian layout and the Xbox's).
>
> Known problems: fighters take damage during Corneria's countdown and are
> launched on the first landing; textures are not all accurate yet
> (indirect texturing for reflections and water, TEV swap tables); netplay
> is not built. [docs/roadmap.md](docs/roadmap.md) has the details and the
> plan; [docs/testing.md](docs/testing.md) says how to test and which logs
> to send.

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
All documentation is indexed in [docs/README.md](docs/README.md).

## Building

See [docs/toolchain.md](docs/toolchain.md). In short:

```sh
tools/xbox/setup.sh               # once: LLVM 21, nxdk, disc_lower
xbox/build.sh                     # -> build-xbox/xbe/default.xbe
tools/lower/test_lower.py         # lowering oracle tests
tools/xbox/test_vp_encoder.py     # vertex-program encoder vs nv2a-vsh
tools/xbox/test_vp_opt.py         # optimized vertex programs vs the reference generator
tools/xbox/vp_policy.py --check   # vertex-program residency policy
tools/xbox/test_tex_convert.py    # native texture formats vs the GX decoder
tools/xbox/test_fog.py            # GX fog on the NV2A vs GX's fog factor
tools/xbox/test_card_endian.py    # memory-card files: big-endian on the card, native in memory
tools/xbox/test_pool.py           # texture and vertex pool allocator
```

Builds and tests run locally; the GitHub workflow runs only when started by
hand.

On macOS, build in Docker instead and boot the result in xemu
([docs/testing.md](docs/testing.md)):

```sh
docker build -t melee-x:sdk tools/xbox/docker   # once
tools/xbox/docker/build.sh
MX_ISO=/path/to/your/melee.iso tools/xbox/xemu_run.sh 120
```

To play: copy `default.xbe` and `default.tbn` (the dashboard icon) into a
folder on the Xbox HDD (e.g. `F:\Applications\Melee-X\`) together with
your own `GALE01` disc image (`.iso`, `.gcm` or `.ciso`, any name), and
launch it from your dashboard. Saves, settings and logs go to
`E:\UDATA\4d580001\` (`boot.log`, `crash.log`; BACK on a controller writes
a screenshot there). To bring a save from Dolphin or a GameCube card, put
its `.gci` in `E:\UDATA\4d580001\card_a\`.

## Licensing

See [LICENSE.md](LICENSE.md). The decompiled game code carries no license;
the port code is GPL-3.0-or-later because it builds on melee-pc.
