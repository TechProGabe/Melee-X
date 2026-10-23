v4.2 fixes the issues players reported on GitHub after v4.1, and adds two control options.

## Fixes

- **Fixed Data > Special and Melee Records > Bonus Records** drawing the filled rows as one flat block (#9, #10).
- **Fixed the Classic team cards** (Team Kirby, Team C. Falcon and the others): the fighters stay inside the banner, behind the black bars and the names (#14).
- **Fixed the trophy room after Classic, Adventure and All-Star** being dark: the spotlight lights the trophy again, so Kirby and the Ice Climbers no longer stay hidden until the room lights up (#16).
- **Fixed metal fighters and chrome** shading in flat bands (metal Zelda's dress, the metal battle in Adventure); they're smooth again (#17).
- **Fixed the Special Movie's sound drifting** away from the picture (#11). Melee-X now plays sound at the GameCube's real rate (a hair faster than before, as on the GameCube).
- **720p with an internal HDMI mod:** Melee-X now checks the picture format after setting 720p and corrects it if the HDMI board's setup left it wrong (#18). If 720p still looks scrambled on your HDMI console, please send your `boot.log`.
- **Upgraded and overclocked CPUs:** some BIOSes for upgraded CPUs rewrite a timing instruction inside games, which can crash them. Melee-X now checks its code at every boot and puts such bytes back.

## New options (settings menu, press Back on the title screen)

- **Trigger as Z** (per controller): the left or right trigger grabs (Z) instead of shielding (#12).
- **C-stick in 1P modes:** the C-stick does smash attacks in Classic, Adventure, training and the other single-player modes instead of moving the camera, like the Action Replay code (#13).

Both are off by default.

Updating from v4 or v4.1: copy the new `default.xbe` over the old one.
