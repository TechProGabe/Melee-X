v4.2 is a bug fix release. Almost everything here came from your reports on GitHub, plus two control options people asked for.

## Fixes

- **720p works again on consoles where it came up scrambled or black** (#18). Some dashboards leave the video chip set for a different picture width, and v4.1 never set it back. That broke 720p over HDMI mods, HDMI adapters and plain component cables alike. Melee-X now checks after switching modes and fixes it.
- **Data > Special and Bonus Records** no longer draw their rows as one flat block (#9, #10).
- **Classic team fights:** the fighters on the team card stay inside the banner again (#14).
- **The trophy room** after Classic, Adventure and All-Star isn't dark anymore (#16).
- **Metal fighters and chrome** are smooth again instead of shaded in flat bands (#17).
- **The Special Movie** no longer drifts out of sync with its sound (#11).
- **No more hitch when the game saves**, like after pressing Start at the end of a match.
- **Rainbow Cruise:** the ship's chrome stopped flashing colours as the camera moves.
- **Brinstar:** the flickering lights in the background are steady, the way they looked on a CRT.
- **Marth's Dancing Blade and Roy's Double-Edge Dance** glow again.
- **Upgraded and overclocked CPUs:** some of their BIOSes patch code inside games, which could crash Melee-X. It now checks itself at boot and undoes that.

## New options

Press Back on the title screen to find these. Both are off by default.

- **Trigger as Z:** the left or right trigger grabs instead of shielding (#12).
- **C-stick in 1P modes:** smash attacks with the C-stick in Classic, Adventure and training, like the Action Replay code (#13).

## Thanks

Big thanks to @DarthPidgey, who reported most of what's fixed here, and to @drewtang713, whose HDMI logs tracked down the 720p bug.

Updating from v4 or v4.1: copy the new `default.xbe` over the old one.
