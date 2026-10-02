Super Smash Bros. Melee running natively on an original Xbox. Not an emulator: the decompiled game is compiled for the Xbox and drawn by its own GPU.

Fully playable. Matches run around 30-60 fps depending on the stage, menus at 60, and the game always ticks at full speed. It's still in development, so a crash can happen, but it's unlikely.

**You need your own disc image** of Super Smash Bros. Melee NTSC-U 1.02 (`GALE01`). Nothing from Nintendo is included.

## Install

1. Unzip `Melee-X-*.zip`.
2. Put your Melee disc image in the `Melee-X` folder next to `default.xbe`.
3. FTP the whole `Melee-X` folder to your Xbox (for example `F:\Applications\`) and launch it from your dashboard.

Want a disc instead? `tools/make-xiso` packs everything into a burnable ISO. The full steps, saves, settings and controls are in the [README](../../#readme).

## What's in the zip

```
Melee-X/
  default.xbe      the game
  default.tbn      dashboard icon
tools/
  make-xiso        packs the game and your disc image into a burnable ISO
README.md
LICENSE.md
```
