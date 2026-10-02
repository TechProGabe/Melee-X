Super Smash Bros. Melee running natively on an original Xbox. Not an emulator: the decompiled game is compiled for the Xbox and drawn by its own GPU.

Fully playable. Matches run around 30-60 fps depending on the stage, menus at 60, and the game always ticks at full speed. It's still in development, so a crash can happen, but it's unlikely.

## What's new in v2

- **Fixed: black screen or hang on the intro movie.** v1 could hang on the first frame of the intro on many consoles. That's fixed, and it works on composite and on component cables to an HDTV, at 480i or 480p, 4:3 or 16:9.
- **720p is now experimental and opt-in.** v1 switched to 720p whenever the dashboard allowed it. Now a dashboard set to 720p still gets 480p; turn it on with `720p = 1` in `settings.ini` if you want to try it (matches run slowly there for now). A v1 `settings.ini` is updated automatically.
- **New `progressive` setting:** `progressive = 0` forces 480i even when your dashboard allows 480p.
- **Quit to the dashboard:** hold L + R + Back + Black on any controller.
- Better crash logs in `boot.log` when something does go wrong on the GPU.

Known issues: Classic mode's team intro cards (Team DK and friends) show the right half black, and the match timer isn't centred at 16:9. Both are visual only.

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
