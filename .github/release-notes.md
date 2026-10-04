Super Smash Bros. Melee running natively on an original Xbox. Not an emulator: the decompiled game is compiled for the Xbox and drawn by its own GPU.

v4 makes the stages come alive, brings back the particle effects, runs faster at 720p and fixes the crashes and freezes people reported on v3.

Thank you to everyone who sent logs, screenshots and crash reports from real consoles. Most of what's fixed here started as one of your reports.

## Stages come alive

A bug in how stage animations reported that they had finished meant many stage events never moved on to their next step. On a lot of stages the timed events simply never happened. They do now:

- **Brinstar Depths:** Kraid rises, slams and turns the stage, instead of freezing after he comes up.
- **Peach's Castle:** Bullet Bills fly across and explode. Before, one could get stuck and keep the screen shaking forever.
- **Onett:** the drugstore sign and the cars run.
- **Adventure mode, Corneria:** the Star Fox team talks over the comm window, with voices, text and moving faces. Before, the window froze on Slippy's first frame with no sound.
- The same fix wakes up timed events on about twenty stages, including Great Bay, Kongo Jungle, Venom, Fourside, Icicle Mountain, Flat Zone and Green Greens.

## Particle effects are back

Particles were never drawn: the Fire Flower's flame, hit sparks and the rest. They are now.

## Faster at 720p

Busy four-player matches at 720p run about 25% faster than v3: Fountain of Dreams went from about 32 to about 40 fps, with the same picture. A two-player match on a simple stage still holds 60.

## Cleaner 720p picture

720p now uses a 24-bit depth buffer with the 16-bit colour. Pokémon Stadium's red lights and the marks on its side platforms no longer break up, and the small flickering seams where surfaces meet on other stages are gone. Classic mode's Team Jigglypuff and Team Kirby cards draw properly at 720p.

## Crashes and freezes fixed

- A crash after many matches in a row: the sound effects' bookkeeping could be torn up when sounds loaded and unloaded at the same time.
- Memory slowly ran out while you played: every stick and button movement was queued up and never thrown away, about 1 MB every half hour. That's what made very long sessions freeze.
- Pokémon Stadium's big screen could show one frame of garbage when the camera switched back to the fight.
- No sound after a crash or power cycle used to make the game freeze every few seconds. Now the sound hardware is reset at boot, and if it's really stuck the game tells you to turn the Xbox off and on, and keeps playing without sound.

## Saves and settings are safer

If `E:` is full or can't be written, Melee-X tells you on screen instead of failing silently. Your last save is kept as it was: a save is written to a new file first and only then swapped in, so a failed write can't wipe it. The title screen says when nothing can be saved, and `boot.log` goes next to `default.xbe` instead.

Pokémon Stadium's big screen shows its text again.

## 128 MB consoles

Upgraded 128 MB consoles always run in their first 64 MB now, the setup everything is tested on. The Use 128 MB RAM option is gone. An old `settings.ini` with `ram128` still loads; the line is ignored.

## Known issues

The ship on Rainbow Cruise still flickers now and then. The trophy view after Classic is lit too dark. None of these stop you from playing. If you run into something else, a `boot.log` (from `E:\UDATA\4d580001\`) and a screenshot help a lot.

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
