Super Smash Bros. Melee running natively on an original Xbox. Not an emulator: the decompiled game is compiled for the Xbox and drawn by its own GPU.

v4 is a big one. It's faster, the particle effects are finally drawn, the stages actually do things again, and a long list of crashes and freezes are gone. Thanks to everyone who sent in logs, screenshots and crash reports from real consoles. Most of what got fixed here started as one of your reports.

## It's faster

Busy four player matches at 720p run about 25% faster than in v3. Four CPUs on Fountain of Dreams went from about 32 fps to about 40, with exactly the same picture. Most of that came from building the game with link-time and profile-guided optimization, a smarter memory layout for the screen, and a lot of small savings in the renderer and the game's own code. Two player matches on simpler stages still sit at 60.

## Particle effects are back

Up to now, particles were never drawn at all. The Fire Flower's flame, hit sparks, smoke and the rest were all invisible. The game was making them just fine; the renderer was throwing them away before they reached the screen. They show up now, and the game looks a lot more like Melee because of it.

## The stages work again

A small bug in how stage animations reported that they'd finished meant many stage events got stuck and never moved on. On a lot of stages, the timed stuff just never happened. All of it runs now:

- On Brinstar Depths, Kraid rises, slams and turns the stage instead of freezing the moment he comes up.
- On Peach's Castle, Bullet Bills fly across and explode. Before, one could get stuck and keep the screen shaking forever.
- In Onett, the drugstore sign and the cars run.
- In Adventure mode's Corneria stage, the Star Fox team talks over the comm window again, with voices, text and moving faces. It used to freeze on Slippy with no sound.
- The same fix wakes up timed events on about twenty stages, including Great Bay, Kongo Jungle, Venom, Fourside, Icicle Mountain, Flat Zone and Green Greens.

The stages feel noticeably more alive.

## Bug fixes and stability

This is where most of the work went:

- Fixed a crash that could happen after a long run of matches. Sound effects loading and unloading at the same moment could tear up the game's list of sounds.
- Fixed memory slowly running out while you play. Every stick and button movement was being queued up and never thrown away, roughly 1 MB every half hour, which is what made very long sessions freeze.
- No more silent boots that freeze every few seconds. After a crash or a power cycle the sound hardware could be left stuck. Melee-X now resets it at startup, and if it's really stuck it tells you to switch the Xbox off and on and keeps playing without sound instead of hitching.
- The 720p picture is cleaner. It now uses a 24-bit depth buffer, so Pokémon Stadium's red lights and the marks on its side platforms no longer break up, and the little flickering seams where surfaces meet on other stages are gone.
- Pokémon Stadium's big screen no longer flashes a frame of garbage when the camera cuts back to the fight, and its text shows up again.
- Classic mode's Team Jigglypuff and Team Kirby intro cards draw properly at 720p.
- If `E:` is full or can't be written, Melee-X tells you on screen instead of failing silently. Saves are written to a new file first and only then swapped in, so a failed write can't wipe your save anymore.
- Upgraded 128 MB consoles now always run in their first 64 MB, the setup everything is tested on. The Use 128 MB RAM option is gone; an old `settings.ini` that still has the line loads fine.

## Known issues

The ship on Rainbow Cruise still flickers now and then, and the trophy view after Classic is lit too dark. Neither stops you from playing. If you run into anything else, a `boot.log` (from `E:\UDATA\4d580001\`) and a screenshot help a lot.

**You need your own disc image** of Super Smash Bros. Melee NTSC-U 1.02 (`GALE01`). Nothing from Nintendo is included.

## Install

1. Unzip `Melee-X-*.zip`.
2. Put your Melee disc image in the `Melee-X` folder next to `default.xbe`.
3. FTP the whole `Melee-X` folder to your Xbox (for example `F:\Applications\`) and launch it from your dashboard.

Updating from v3? Copy the new `default.xbe` and `default.tbn` over the old ones. Your saves and settings stay where they are.

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
