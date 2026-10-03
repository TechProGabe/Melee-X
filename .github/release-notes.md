Super Smash Bros. Melee running natively on an original Xbox. Not an emulator: the decompiled game is compiled for the Xbox and drawn by its own GPU.

v3 is a big one. More than fifty changes went in since v2, and most of them started as bug reports from people playing on real consoles.

First, a huge thank you to **maple72**. They tested release candidate after release candidate on real hardware, sent logs and screenshots every time something broke, and found a big chunk of what got fixed here. This release would look very different without them. Thanks also to **wadeonxbox**, whose early reports and logs got us through the v1 boot hang.

## The freeze is fixed

The worst bug in v2 was a hard freeze. It could hit after a while in any match, and 100-Man Melee hit it almost every time, leaving you to power cycle the console. That's fixed, so long sessions and 100-Man Melee keep going instead of locking up. v3 has run overnight on a real console without a hitch.

Two other mid-match freezes that only showed up on 128 MB consoles are fixed too.

## 720p is on by default

In v2, 720p was opt-in and matches crawled at about 7 fps. That's a lot better now. A two player match on a simple stage plays at 60 fps at 720p, and only busy four player matches drop to around 20 to 30. The item boxes and Fountain of Dreams' grass no longer flicker with the wrong textures. If your dashboard allows 720p, Melee-X uses it.

If you want those big four player matches smoother, 480p runs them at 30 to 60 fps. Pick it in the new settings menu. And if your TV can't show 720p at all, hold **Back** while Melee-X starts: you get 480i, and that choice is saved.

**Upgrading from v2?** Your `settings.ini` still says `720p = 0` from back when it was opt-in, so you'll stay at 480 until you turn 720p on in the menu (or delete `settings.ini` to get the new defaults).

## A settings menu

Press **Back** on the title screen ("Press Start") and a settings panel opens. You don't have to edit `settings.ini` over FTP anymore. In there you'll find:

Video output (480i, 480p or 720p) and widescreen, both showing the mode your console will really use, since the dashboard has the final say. A frame-rate counter. BACK screenshots, which save a `shotNN.bmp` next to your settings when you press Back, handy for bug reports. Front LED effects. Use 128 MB RAM for upgraded consoles. Rumble strength. Stick dead zones and trigger click per controller.

The counter and rumble change right away, dead zones when you close the menu, and anything marked with a `*` needs a restart, which **Save and restart** does for you.

## Front LED effects

New and off by default: the front light can flash on KOs, count down the last seconds of a match and sweep on GAME!. Turn it on in the settings menu. If something else controls your front LED, such as a Kronos modchip, it's best to leave this off.

## Everything else that got fixed

Classic mode's team intro cards (Team DK and friends) no longer have their right half blacked out, at 480 or 720p.

The match timer sits in the middle of the screen again at 16:9.

The winner's portrait on the results screen was an empty black box. It shows now, and so do the portraits when everyone ties for first.

The sepia freeze frame behind the Stage Clear and Game Clear bonus list was missing or the wrong colour. It's back.

A fighter knocked off the top of the screen sometimes vanished instead of flying off into the background with the star KO. Fixed.

The Cloaking Device and Invisible Melee now draw the proper see-through, refracting look.

Four players on Fountain of Dreams runs faster: the reflection doesn't draw fighters it can't see anymore.

Consoles upgraded to 128 MB now run in their first 64 MB by default, the setup everything was tested on. Turn on Use 128 MB RAM if you want all of it.

A handful of speedups in the sound mixer and the renderer make each frame a bit cheaper.

The previous launch's `boot.log` is kept as `boot_prev.log`, so a freeze followed by a relaunch doesn't lose the log anymore.

## Known issues

Particle effects aren't drawn, so things like the Fire Flower's flame are invisible. The ship on Rainbow Cruise flickers now and then. Pokémon Stadium's big screen can show garbage when the camera switches. On Peach's Castle the Bullet Bill can get stuck and keep the screen shaking. On Classic's Team Kirby card some of the Kirbys can look corrupted. In Adventure mode the Corneria Arwing cutscene has no voices and Falco's face freezes. The trophy view after Classic is lit too dark. And on very long sessions the whole console has frozen twice; that one's still being chased.

None of these stop you from playing. If you run into something else, a `boot.log` (from `E:\UDATA\4d580001\`) and a screenshot help a lot.

**You need your own disc image** of Super Smash Bros. Melee NTSC-U 1.02 (`GALE01`). Nothing from Nintendo is included.

## Install

1. Unzip `Melee-X-*.zip`.
2. Put your Melee disc image in the `Melee-X` folder next to `default.xbe`.
3. FTP the whole `Melee-X` folder to your Xbox (for example `F:\Applications\`) and launch it from your dashboard.

Updating from v2? Copy the new `default.xbe` and `default.tbn` over the old ones. Your saves and settings stay where they are.

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
