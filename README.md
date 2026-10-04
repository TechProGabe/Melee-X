# Melee-X

Super Smash Bros. Melee running natively on an original Xbox.

This isn't an emulator. The [doldecomp](https://github.com/doldecomp/melee) project turned Melee back into C source, and Melee-X compiles that code for the Xbox's 733 MHz Pentium III. Every frame is drawn by the Xbox's own NV2A GPU through a GameCube graphics layer written for it, and the game data streams from your disc image the same way it would from a GameCube disc.

**Status:** fully playable. Matches run between 30 and 60 fps depending on the stage and how much is going on, menus sit at 60, and the game itself always ticks at full speed. It's still in development, so a crash can happen, but it's unlikely.

- 4 players on the 4 controller ports, with rumble
- 480i, 480p or 720p, 4:3 or 16:9, within what your dashboard's video settings allow
- A settings menu right on the title screen (press **Back** there)
- Saves in the GameCube `.gci` format, so your Dolphin or memory card save works here and the other way around
- Hold **L + R + Back + Black** on any controller to quit to the dashboard

You need your own copy of the game. Nothing from Nintendo ships with this.

The Xbox side grew out of [OpenCrossing-Xbox](https://github.com/GabeConway/OpenCrossing-Xbox), my Animal Crossing port.

## What you need

- A modded original Xbox (softmod or modchip) that runs homebrew. A stock 64 MB console is fine; a 128 MB console runs in its first 64 MB.
- A way to copy files to it, usually FTP from your dashboard (UnleashX, XBMC4Gamers, EvolutionX and friends all have a server built in).
- A disc image of **Super Smash Bros. Melee, NTSC-U, version 1.02** (game ID `GALE01`, revision 2), dumped from your own disc. `.iso`, `.gcm` and `.ciso` all work, and the filename doesn't matter. PAL and Japanese copies won't boot. The 1.00 and 1.01 revisions start, but only 1.02 has been tested.
- A controller. The Duke and the Controller S both work.

## Install

Grab the newest `Melee-X-<version>.zip` from the [Releases](../../releases) page and unzip it. It has `Melee-X/default.xbe` (the game), `Melee-X/default.tbn` (the dashboard icon) and `tools/make-xiso`.

**Hard drive** (easiest, loads fastest):

1. Drop your Melee disc image into the `Melee-X` folder, next to `default.xbe`.
2. Connect to your Xbox with an FTP client like FileZilla. Your dashboard shows the Xbox's IP address; the login on most dashboards is `xbox` / `xbox`.
3. Copy the whole `Melee-X` folder to wherever your dashboard looks for apps or games, for example `F:\Applications\` or `E:\Games\`. The image is about 1.4 GB, so give it a minute.
4. Launch Melee-X from the dashboard (the orange X icon).

**Burned disc:** this packs `default.xbe` and your disc image into one Xbox image of about 1.4 GB, so it needs a **DVD-R**.

1. Get [xdvdfs](https://github.com/antangelo/xdvdfs/releases).
2. Build the ISO, either with `tools/make-xiso Melee-X "Melee.iso" Melee-X.iso` (Git Bash, WSL, Linux or macOS, with `xdvdfs` on your PATH), or by putting your image in the `Melee-X` folder and running `xdvdfs pack Melee-X Melee-X.iso`.
3. Burn `Melee-X.iso` as an image at a low speed (ImgBurn or similar) and start it from the dashboard's disc option.

Not every Xbox DVD drive likes burned discs; Samsung drives are the most forgiving and Thomson drives the least. If yours won't read it, use the hard drive.

**xemu:** load the ISO from above with Machine > Load Disc and set xemu's memory to 64 MB. It runs slower than a real Xbox.

**Updating:** copy the new `default.xbe` and `default.tbn` over the old ones. Your saves and settings stay where they are.

## Saves

Saves always go to the hard drive, even when you play from a disc: `E:\UDATA\4d580001\card_a\`, as normal GameCube `.gci` files. Each save is written to a new file first and only then swapped in, so a failed save (a full `E:`, say) leaves your previous one untouched.

To bring over a save from Dolphin or a real memory card, export it as a `.gci` (Dolphin's memory card manager does this, and GCI tools can pull one off a real card), launch Melee-X once so it makes its folders, and FTP the `.gci` into `E:\UDATA\4d580001\card_a\`. It works the other way around too.

## Video

Your dashboard decides what your TV and cables can handle, and Melee-X never goes past it.

- **480i** works on everything, composite included.
- **480p** needs component cables (or a VGA/HDMI adapter) and 480p turned on in the dashboard. Busy four player matches run smoothest here.
- **720p** needs component cables and 720p turned on in the dashboard. Melee-X uses it whenever the dashboard allows it, and it's the sharpest picture. It's always 16:9.
- **Widescreen** at 480 shows more of the stage instead of stretching it. Set widescreen in the dashboard and leave it on in Melee-X.

**Black screen?** Some TVs don't show 720p even when the dashboard allows it. Hold **Back** on any controller while Melee-X starts (the loading screen says so): that boot comes up in 480i and turns 720p and 480p off in your settings. Turn them back on in the menu whenever you like.

## Settings

Press **Back** on the title screen ("Press Start") and the settings menu opens. Up and down pick a row, left and right (or A) change it, and B saves and closes. Anything marked `*` takes effect after a restart, which **Save and restart** does for you; if you ask for a mode your dashboard doesn't allow, the menu shows what you'll actually get, like `480p -> 480i`.

| option | what it does |
|---|---|
| Video output | 480i, 480p or 720p `*` |
| Widescreen (16:9) | 16:9 at 480, on by default; also needs widescreen set in the dashboard `*` |
| Frame-rate counter | fps in the top left corner, off by default |
| BACK screenshots | Back saves `E:\UDATA\4d580001\shotNN.bmp`, off by default. Handy for bug reports |
| Front LED effects | the front light flashes on KOs, in the last seconds and on GAME!, off by default. Leave it off if something else drives your LED, such as a Kronos modchip |
| Rumble | off, or 25% to 100% |
| Controller | pick a port, then its stick and C-stick dead zones and how far the triggers go in before they count as a full press |
| Save and restart / Save and close | |

Everything the menu changes lives in `E:\UDATA\4d580001\settings.ini`, which you can also edit over FTP. That's the only settings file Melee-X reads (a `settings.ini` next to `default.xbe` is ignored). It's written on the first boot; delete it to go back to the defaults. Button remapping is only in this file for now: under `[port1]` to `[port4]`, lines like `a = A` map an Xbox button (`a b x y white black start back lstick rstick up down left right`) to a GameCube one (`A B X Y Z L R START UP DOWN LEFT RIGHT NONE`).

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

The layout matches where the buttons sit on a GameCube pad.

## If something breaks

Logs go to `E:\UDATA\4d580001\`: `boot.log` always, `boot_prev.log` from the launch before, plus `crash.log` or `hang.log` if it went wrong. Attach those (and a photo if the screen showed an error) when you open an issue. If something looks wrong rather than crashing, turn on BACK screenshots and press Back when you see it.

**Nothing gets saved?** If Melee-X can't write to `E:` (usually because it's full), it says so on screen, the title screen shows `Can't write to E: (full?): nothing will be saved`, and `boot.log` goes next to `default.xbe` instead. Free some space and launch again.

**No sound?** After a crash or a power cycle the Xbox's sound hardware can get stuck. Melee-X then shows "Sound hardware is stuck. Turn the Xbox off and on to get sound back." for 10 seconds and plays on without sound. Switch the console off and on.

## Building it yourself

nxdk and LLVM 21, built in Docker or natively on Windows with MSYS2:

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
- maple72, for testing release candidates on real hardware and reporting what broke
- wadeonxbox, for the early hardware reports and logs that tracked down the v1 boot hang

> AI tools (Claude) were used in developing this port.
