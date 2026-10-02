# Melee-X

Super Smash Bros. Melee running natively on an original Xbox.

This isn't an emulator. The [doldecomp](https://github.com/doldecomp/melee) project turned Melee back into C source, and Melee-X compiles that code for the Xbox's 733 MHz Pentium III. Every frame is drawn by the Xbox's own NV2A GPU through a GameCube graphics layer written for it, and the game data streams from your disc image the same way it would from a GameCube disc.

**Status:** fully playable. Matches usually run somewhere between 30 and 60 fps depending on the stage and how much is going on, menus sit at 60, and the game itself always ticks at full speed. It's still in development, so a crash can happen, but it's unlikely.

- 4 players on the 4 controller ports, with rumble
- 480i or 480p, 4:3 or 16:9, picked from your dashboard's video settings. Tested on composite and on component cables to an HDTV
- 720p is experimental and stays off unless you turn it on in `settings.ini` (matches run slowly there and some graphics are wrong)
- Hold L + R + Back + Black to quit back to the dashboard
- Saves use the GameCube `.gci` format, so your Dolphin or memory card save works here and the other way around

You need your own copy of the game. Nothing from Nintendo ships with this.

The Xbox side grew out of [OpenCrossing-Xbox](https://github.com/GabeConway/OpenCrossing-Xbox), my Animal Crossing port.

## What you need

- A modded original Xbox (softmod or modchip) that runs homebrew. A stock 64 MB console is fine. On a console upgraded to 128 MB, Melee-X uses only 64 MB unless you turn on `ram128` (see Settings).
- A way to copy files to it. Usually that's FTP from your dashboard (UnleashX, XBMC4Gamers, EvolutionX and friends all have a server built in).
- A disc image of **Super Smash Bros. Melee, NTSC-U, version 1.02** (game ID `GALE01`, revision 2), dumped from your own disc. `.iso`, `.gcm` and `.ciso` all work, and the filename doesn't matter. PAL and Japanese copies won't boot. The 1.00 and 1.01 revisions start, but only 1.02 has been tested.
- A controller. The Duke and the Controller S both work.

## Download

Grab the newest `Melee-X-<version>.zip` from the [Releases](../../releases) page and unzip it:

```
Melee-X/
  default.xbe      the game
  default.tbn      dashboard icon
tools/
  make-xiso        packs the game and your disc image into a burnable ISO
```

Then pick how you want to play.

## Option 1: install to the hard drive

Easiest, and it loads the fastest.

1. Drop your Melee disc image into the `Melee-X` folder, right next to `default.xbe`.
2. Connect to your Xbox with an FTP client like FileZilla. Your dashboard shows the Xbox's IP address, and the login on most dashboards is `xbox` / `xbox`.
3. Copy the whole `Melee-X` folder to wherever your dashboard looks for apps or games, for example `F:\Applications\` or `E:\Games\`.
4. Launch Melee-X from the dashboard. You should see the orange X icon.

When you're done it looks like this:

```
F:\Applications\Melee-X\
  default.xbe
  default.tbn
  Melee.iso        (yours, any name)
```

The image is about 1.4 GB, so give the copy a minute.

## Option 2: burn a disc

This packs `default.xbe` and your disc image into one Xbox disc image. Melee's files fill most of the GameCube disc, so the result is around 1.4 GB and needs a **DVD-R** (a CD-R is too small).

1. Get [xdvdfs](https://github.com/antangelo/xdvdfs/releases). On Windows just download the `.exe` from its releases page.
2. Build the ISO. Either way works:
   - With the script, from Git Bash, WSL, Linux or macOS (needs `xdvdfs` on your PATH):

     ```sh
     tools/make-xiso Melee-X "Melee.iso" Melee-X.iso
     ```

   - By hand: put your disc image in the `Melee-X` folder like in Option 1, then run `xdvdfs pack Melee-X Melee-X.iso`.
3. Burn `Melee-X.iso` to a DVD-R at a low speed with something like ImgBurn. Burn it as an image, don't let the burner convert it.
4. Put the disc in your modded Xbox and start it from the dashboard's disc option.

Not every Xbox DVD drive likes burned discs. Samsung drives are usually the most forgiving and Thomson drives the least. If yours won't read it, go with Option 1.

## Option 3: xemu

Build the ISO from Option 2, then load it in [xemu](https://xemu.app) with Machine > Load Disc. Set xemu's memory to 64 MB. It runs, but slower than a real Xbox, since xemu has to emulate the GPU.

## Saves

Saves always go to the hard drive, even when you play from a disc. They live in `E:\UDATA\4d580001\card_a\` as normal GameCube `.gci` files.

To bring over a save from Dolphin or a real memory card:

1. Export it as a `.gci`. Dolphin's memory card manager does this, and GCI tools can pull one off a real card.
2. Launch Melee-X once so it makes its folders.
3. FTP the `.gci` into `E:\UDATA\4d580001\card_a\`.

Going the other way works too: copy the `.gci` off the Xbox and import it into Dolphin.

## Settings

Settings are in `E:\UDATA\4d580001\settings.ini`. Melee-X writes it the first time it boots, and you can edit it over FTP. A file written by v1 is updated on the first boot of v2: 720p is switched off there, since v1 turned it on by default.

| section | setting | what it does |
|---|---|---|
| `[video]` | `720p` | experimental: 1 uses 720p when your dashboard has it turned on (needs component cables). Off by default; a dashboard set to 720p still gets 480p. Matches run slowly at 720p for now |
| | `progressive` | 0 forces 480i even when your dashboard allows 480p |
| | `widescreen` | 1 draws 16:9 at 480i/480p when the dashboard is set to widescreen |
| | `fps` | 1 shows a frame counter in the top left corner (off by default) |
| `[system]` | `ram128` | 1 lets a console upgraded to 128 MB use all of it. Off by default: Melee-X then runs in the first 64 MB, the setup it was tested on |
| `[input]` | `rumble` | rumble strength in percent, 0 turns it off |
| `[port1]` to `[port4]` | `stick_deadzone`, `cstick_deadzone` | stick dead zones in percent |
| | `trigger_click` | how far (0-255) a trigger goes in before it counts as a full L/R press |
| | `a`, `b`, `x`, `y`, `white`, `black`, ... | button mapping, written as `xbox button = GameCube button` |

## Controls

| Xbox | GameCube |
|---|---|
| A | A |
| X | B |
| B | X |
| Y | Y |
| White or Black | Z |
| Left / right trigger | L / R (analog) |
| Left stick | control stick |
| Right stick | C-stick |
| D-pad | D-pad |
| Start | Start |

The layout matches where the buttons sit on a GameCube pad. Every port can be remapped in `settings.ini`.

Hold **L + R + Back + Black** on any controller to quit to the dashboard.

## If something breaks

Logs go to `E:\UDATA\4d580001\`: `boot.log` always, plus `crash.log` or `hang.log` if it went wrong. Attach those (and a photo if the screen showed an error) when you open an issue.

## Building it yourself

The short version: nxdk and LLVM 21, built either in Docker or natively on Windows with MSYS2.

```sh
tools/xbox/msys/build.sh          # Windows, MSYS2
tools/xbox/docker/build.sh        # Docker (Linux, macOS, Windows)
```

Both write `build-xbox/xbe/default.xbe`. The setup, the tests and how the port works are in [docs/](docs/README.md), starting with [docs/toolchain.md](docs/toolchain.md) and [docs/architecture.md](docs/architecture.md).

## Legal

This repo has no game assets, no disc data and no Nintendo binaries. It's the decompiled C source plus port code. You need your own legally dumped copy of Melee, so please don't open issues asking for ISOs.

Not affiliated with or endorsed by Nintendo or Microsoft. Super Smash Bros. Melee is a trademark of Nintendo, and Xbox is a trademark of Microsoft. Licensing details are in [LICENSE.md](LICENSE.md).

## Credits

- [doldecomp/melee](https://github.com/doldecomp/melee), the decompilation this is all built on
- [melee-pc](https://github.com/999sian/melee-pc), the PC port that made the decomp run on little-endian machines
- [encounter/aurora](https://github.com/encounter/aurora) for the Dolphin SDK headers
- [nxdk](https://github.com/XboxDev/nxdk), [xemu](https://xemu.app) and [xdvdfs](https://github.com/antangelo/xdvdfs), the open Xbox toolchain, emulator and ISO packer

> AI tools (Claude) were used in developing this port.
