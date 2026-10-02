# Platform layer

This covers how the Dolphin SDK is implemented on the Xbox (`xbox/src/sdk`)
and what sits under it (`xbox/src/hw`).

## Three kinds of code

| class | where | triple | notes |
|---|---|---|---|
| game | `src/melee`, `src/sysdolphin`, parts of `src/pc` | game triple | built by `tools/xbox/compile_game.py`: preprocess -> CP932 literals -> `disc_lower` -> compile |
| sdk | `xbox/src/sdk`, `src/pc/audio.c`, `src/pc/misc.c`, aurora's MTX | game triple, no lowering | shares structs with the game |
| hw | `xbox/src/hw` | nxdk's `i386-pc-win32` | kernel, pbkit, AC97, USB |

The game triple is `--target=i686-pc-windows-gnu -mno-ms-bitfields
-march=pentium3 -msse -mfpmath=sse`; `docs/toolchain.md` explains why.
sdk and hw talk only through `xbox/include/xhw.h` and `xbox/include/xgx.h`,
which use plain types (no bit-fields, no 64-bit struct members) so that
both triples lay them out the same.

`xbox/include/game/xbox_game_prelude.h` is force-included into game and sdk
code. It:
- maps `__int64` to `long long` and provides the `M_*` constants;
- renames `atan2f`, `acosf`, `asinf`, `expf` and `powf` to `melee_*`, so
  the game's own versions don't clash with pdclib's;
- routes the printf family to `xsdk_*` (the log), unless
  `XSDK_NO_STDIO_RENAME` is defined.

nxdk's pdclib printf has no floating point: it skips `%f`/`%e`/`%g`
without taking the double, and every later argument is read 4 bytes off
(an `OSReport` with `%f` before `%s` crashed the console). `log.c`'s
`xsdk_vsnprintf` formats the float conversions itself and hands each other
conversion to pdclib with its own argument; `OSReport`, `OSPanic`,
`pc_log_line` and the renamed printf family go through it. The game's own
`sprintf`/`snprintf` calls still use pdclib (a `%f` there prints nothing:
the trophy display's debug coordinates).

## Boot

`xhw_main.c` runs these steps:

1. Mount E:, create `E:\UDATA\4d580001\`, and open `boot.log`.
2. `xhw_splash_show` (`xhw_splash.c`, from OpenCrossing-Xbox): the
   "TechProGabe Presents..." card, drawn by the CPU into a 640x480
   framebuffer. It fades in and holds 2 s (any button skips). A load bar
   under it advances through the next steps. `-DXHW_NO_SPLASH` turns it
   off, `-DXHW_SPLASH_MS=<n>` sets the hold, and `-DXHW_SPLASH_DUMP`
   screenshots it.
3. Find the first `.iso`, `.gcm` or `.ciso` in the XBE's folder (D:\).
4. `xsdk_early`: load settings.
5. `xsdk_boot` (`boot.c`): OSInit, open the disc image and check that it
   is GALE01 (revision 2 is the target; 0 and 1 load with a warning), then
   region and fonts. Then `xhw_video_boot` picks the video mode, which ends
   the card, and then widescreen mode and `melee_main`.

The game thread runs inside `xhw_crash_guard`, and so does every thread
started with `xhw_thread_start`.

## OS (`os.c`)

- **MEM1**: 24 MB at VA 0x10000000, reserved and committed on demand
  (`docs/architecture.md`). It has to sit above 16 MB, because
  `PC_IS_ARAM_ADDR` treats anything lower as an ARAM offset. Low memory
  holds the OSBootInfo fields the game reads. The arena starts at +0x4000.
- **Melee's heaps**, for reference:

  | heap | size | where |
  |---|---|---|
  | Stay | 0x4F8800 | MEM1 |
  | AllM | 0x64B400 | MEM1 |
  | Seq | 0x800 | MEM1 |
  | AllA | 0x96C800 | ARAM |

  The ARAM heap spans `ARAlloc(0x20)` to `min(ARGetSize(), 16 MB)`.
- **Interrupts and alarms** follow melee-pc's model:
  - The game is single-threaded.
  - `OSDisableInterrupts` is a recursive lock shared with the worker
    threads (audio mixer, DVD). Each thread keeps its depth in a TLS slot
    (`TlsAlloc`; the game triple can't use `__thread`).
  - Alarms and deferred completions (ARQ, CARD) run on the game thread
    when it re-enables interrupts and at every frame boundary. That is
    where the GameCube's interrupt handlers ran.
- **Time**: the time base ticks at 40.5 MHz (`OS_TIMER_CLOCK`) and counts
  from 2000-01-01, seeded from the Xbox's clock.

## DVD (`dvd.c`)

- Reads the FST from the image at boot.
- Supports `.iso` and `.gcm` (raw) and `.ciso`. A `.ciso` has a block map
  after a 0x8000-byte header; missing blocks read as zeros.
- Async reads run on one worker thread. As with the GameCube's DVD
  interrupt, the callback runs from that thread while holding the
  interrupt lock.
- Every read commits its destination first (`xhw_commit`), because the
  kernel's file system can't take the demand-commit fault.
- `DVDGetDOLLocation` and the DOL loader feed `discfont.c`, which reads
  the fonts out of the game's own DOL.

## ARAM (`ar.c`)

- 16 MB at VA 0x12000000, committed on demand. ARAM addresses are offsets
  into it, and the first 16 KB is kept free, as the SDK does for the DSP.
- `ARAlloc`/`ARFree` are the SDK's bump allocator.
- ARQ transfers are `memcpy` at post time, with callbacks deferred to the
  game thread.
- `src/pc/audio.c` reads samples straight out of the buffer via
  `aurora_aram_base()`. That is why ARAM is committed by the fault handler
  and not only by the AR/ARQ copies.

## PAD (`pad.c`, `xhw_pad.c`)

- Player N is physical port N, taken from SDL2's
  `SDL_JoystickGetDevicePlayerIndex`, so plugging and unplugging controllers
  doesn't shuffle players.
- Default layout is GameCube-like by position:

  | Xbox | GameCube |
  |---|---|
  | A | A |
  | X | B |
  | B | X |
  | Y | Y |
  | White / Black | Z |
  | triggers | analog L/R |
  | left stick | control stick |
  | right stick | C-stick |
  | Start | Start |
  | D-pad | D-pad |

- Stick handling:
  - a radial dead zone;
  - then scaled so full tilt reaches the GameCube's raw rim (±104);
  - the game clamps to its own 80-unit circle.
- The digital L/R click fires past `trigger_click`.
- In-game reset: L and R pressed past 200 with BACK and BLACK, on any
  port, quits to the dashboard (`PADRead`). Not BACK + START: L + R + START
  is Melee's own reset from the pause menu.
- Rumble goes to the pad's motors, scaled by `[input] rumble`; 100 drives the
  motors at 75% (full strength was too strong on the console's controllers).

## `settings.ini`

`E:\UDATA\4d580001\settings.ini` is written with the defaults on first boot,
and by the settings menu below:

```ini
[video]
720p = 1            ; use 720p (16:9) when the dashboard allows it; 0: 480 only
progressive = 1     ; 0: 480i even where the dashboard allows 480p
widescreen = 1      ; 16:9 at 480 when the dashboard is set to widescreen
fps = 0             ; frame-rate counter in the top-left corner (default: 1 in test builds, 0 in a release)
[system]
ram128 = 0          ; 1: use the RAM above 64 MB on an upgraded console (untested)
screenshots = 0     ; 1: BACK saves shotNN.bmp (default: 1 in test builds)
led = 1             ; front LED effects ("Front LED" below)
[input]
rumble = 100        ; percent
[port1]             ; .. [port4]
stick_deadzone = 20 ; percent, radial
cstick_deadzone = 25
trigger_click = 230 ; 0-255
a = A               ; Xbox button = GameCube button (A B X Y Z L R START UP DOWN LEFT RIGHT NONE)
b = X
x = B
y = Y
white = Z
black = Z
start = START
back = NONE
lstick = NONE
rstick = NONE
up = UP
down = DOWN
left = LEFT
right = RIGHT
```

Writing (`settings.c`): the file goes to `settings.tmp` first, is flushed,
read back and parsed, and only a copy that holds the same settings is
renamed over `settings.ini` (`xhw_replace_file`: the kernel's rename with
replace, else delete and rename; then the volume's directory entries are
flushed). The writer ends the file with `; end of settings`, and the
read-back requires it. A failed write leaves the old file and logs
`[SETTINGS] save failed`. If FATX refused the replace and the delete went
through but the rename didn't, `settings.ini` is gone and the complete
`settings.tmp` is kept; a boot that finds `settings.tmp` with no
`settings.ini` takes it, but only with the end line (a copy cut off
mid-write is left and the defaults are written). The writer
regenerates the whole file, so comments and keys it doesn't know are
dropped (as before). `[SETTINGS] in use: ...` and `[SETTINGS] saved: ...`
log the values.

### Settings menu (`menu.c`)

BACK on the title screen ("PRESS START") opens a panel over the title; a
hint line at the top of the title says so. `gmtitle.c` calls
`xsdk_menu_title_frame` every title frame (`PORT:`); while the panel is up
the title stands still (START does nothing, the attract demo's timer is
held) and `PADRead` hands the game connected controllers with nothing
pressed, also until the closing press is let go (one second at most).
The in-game reset (L + R + BACK + BLACK) still works with the menu up.
The console's RAM comes from the kernel's physical page count
(`xhw_mem_has_upper`, `MmQueryStatistics`, as `xhw_mem_hold_upper` uses
it). On a 64 MB console a `ram128 = 1` in the file is ignored at boot
(`[SETTINGS] ram128 = 1 ignored: this console has 64 MB`) and written as 0
by the next save.

| row | values | applies |
|---|---|---|
| Video output | 480i, 480p, 720p; `480p -> 480i` etc. when the dashboard doesn't allow the chosen mode (the ini only allows a mode) | after a restart |
| Widescreen (16:9) | On, Off; `On -> Off` at 480 when the dashboard is 4:3 | after a restart |
| Frame-rate counter | On, Off | at once |
| BACK screenshots | On, Off (`[system] screenshots`; on in test builds): BACK saves `shotNN.bmp` next to `settings.ini` | at once |
| Front LED effects | On, Off (`[system] led`) | at once: On plays a short sweep, Off gives the LED back to the SMC |
| Use 128 MB RAM | On, Off; on a 64 MB console "Off (64 MB console)", greyed, can't be changed | after a restart |
| Rumble | Off, 25-100% | at once, with a short pulse |
| Controller | Port 1-4: the three rows below edit that port | |
| Stick / C-stick dead zone | 0-60% in steps of 5, live stick readout | when the menu closes |
| Trigger click | 5-255 in steps of 5, live trigger readout | when the menu closes |
| Save and restart | writes `settings.ini`, relaunches this XBE by the kernel's path for it (`XeImageFileName`, e.g. `\Device\Harddisk0\Partition6\Applications\Melee-X\default.xbe`; `xhw_reboot_self`, also the game's own reset) | |
| Save and close | writes `settings.ini` (B or BACK does too) | |

Up/Down (D-pad or left stick) select, Left/Right or A change; every
controller drives it, by its raw buttons, so a remapped pad still works.
Values that only take effect after a restart get a `*` while they differ
from the running ones; the video and widescreen rows show what the console
will actually run (`chosen -> used`) when the dashboard doesn't allow the
chosen mode. Closing saves only when something changed. Button mapping
stays in the file. The panel is drawn by the platform over the finished
frame (`docs/renderer.md`), not by the game: Melee's own menus are models
with prebaked text, and a page in its Options menu would mean new menu
data.

The panel is 18 rows (title, blank, 13 settings, the selected row's
hint, a message line, the controls): at 720p that is 91% of the height,
and a 19th row would take 96%, past the TV-safe area. A new row has to
give one up (the LED row took the blank line above the hint).

## Front LED (`xhw_led.c`)

The SMC drives the front LED. SMBus register 0x08 of the SMC (address 0x20)
takes a custom sequence of four steps that the SMC cycles through by itself,
several steps a second; register 0x07 = 1 switches the LED to it and 0x07 =
0 hands it back to the SMC (solid green, or its own blinking). The byte, as
nxdk's `hal/led.c` (`XSetCustomLED`) builds it: bit 7-n is step n's red,
bit 3-n its green (n = 0..3; the high nibble red, the low nibble green, the
first step in the top bit of each); red and green together are orange.
Each change writes 0x08 and then 0x07 = 1.

| event | pattern (4 steps) | how long |
|---|---|---|
| GAME! / TIME! (`gmvs.c`, the match's end) | red, orange, green, orange: a sweep | 3 s, also into the next scene |
| KO (`ft_0D31.c`, `ftCo_800D34E0`) | P1 red/off, P2 red/green, P3 orange/off (yellow), P4 green/off: the port's colour blinking | 1.5 s |
| timer, last 10-6 s (`gmvs.c`, `fn_8016CD98`) | orange blip, off x3 | until the next second |
| timer, last 5-3 s | orange/off blinking | |
| timer, last 2-1 s | orange/red | |
| a stock match with someone on their last stock | green x3, a red tick | until the match ends |
| anything else, the menus, a no contest | the SMC's (register 0x07 = 0) | |

The first row that applies wins. Every scene change (`xsdk_scene_log`)
clears them all but a running sweep, so the LED is the SMC's outside
matches and after the results. Settings menu: turning it on plays the
sweep for 1.5 s. Turning it off hands the LED back (one write, and only if
a pattern was up) and then nothing touches the SMBus: the game's events
return before the ring, and with `led = 0` at boot the worker is never
started.

Cost: the game thread only posts an event into a 16-entry ring (a few
stores and a `SetEvent`; nothing at all with the setting off) at a KO, once
a second in a timed match's last 10 seconds, at the match's end and at a
scene change: never per frame. A worker thread at the lowest priority
(started when the setting is on) drains the ring, works out the pattern and
writes it only when it changes, at most every 80 ms, so it runs while the
game waits for the vertical blank and a write's SMBus wait never holds the
game up. The SMC does the blinking; the worker only times when an effect
ends. Every write is logged as `[LED] <steps> <why> (retrace N)` (`R`,
`G`, `O`, `-` for off) or `[LED] SMC`, up to 300 a boot.

Handing back: `xhw_quit_to_dashboard` and `xhw_reboot_self` (the in-game
reset, the game's own restart, Save and restart, fatal errors) stop the
worker and wait up to 300 ms for its register 0x07 = 0, writing it
themselves if it doesn't answer. A crash (`xhw_crash.c`, not at raised
IRQL) and a hang report on screen (`xhw_watchdog.c`) only set a flag and
signal the worker, raised to the highest priority for it so a spinning game
thread can't starve it: no SMBus wait in those paths. After a crash the
effects stay off; after a hang report the next event starts them again. A
failed SMBus write logs `[LED] SMBus write ... failed` once, tries one
hand-back and leaves the LED alone for the rest of the boot.

While a pattern is up the SMC's own LED signals (tray, errors) are hidden;
patterns are only up for an effect, never while idle. xemu doesn't show the
LED; the `[LED]` lines are the test.

## VI (`vi.c`, `xhw_video.c`)

- 720p is the default: it is used when `720p = 1` (the default), the
  dashboard allows it and at least 32 MB is free at boot. BACK held on any
  controller while Melee-X starts (`boot.c`, read once the splash has
  enumerated the pads) gives 480i and saves `720p = 0`, `progressive = 0`:
  the way out for a TV that doesn't show the dashboard's 720p. The splash
  itself is in the dashboard's mode and says so. Otherwise the
  mode is 640x480 at 32 bits: 480p when the dashboard allows it and
  `progressive = 1`, else 480i; 16:9 if the dashboard is set to widescreen.
  The game sees progressive through `VIGetDTVStatus`.
- 480i on an HDTV pack set to 480p: `XVideoSetMode` always picks 480p there,
  so `set_mode_480` calls nxdk's `XVideoInit` with the 640x480i HDTV mode
  after it. PAL has no progressive modes in nxdk.
- If 720p can't be set up, `xhw_video_fallback_480` drops to 480.
- A `settings.ini` without a `progressive` line was written by v1, which
  defaulted to `720p = 1`: 720p is turned off and the file rewritten.
- `VIWaitForRetrace` is the frame boundary. It paces to 60.000 Hz, runs due
  alarms (the pad-poll alarm reads the controllers there), then the retrace
  callbacks. `VISetBlack` is honoured at the flip.

## Audio (`src/pc/audio.c`, `sdl3_audio.c`, `xhw_audio.c`)

1. melee-pc's software AX mixer runs unchanged. Its SDL3 audio-stream
   calls go to a small shim (`xbox/include/sdk/SDL3/SDL.h`, renamed
   `xsdk_SDL_*` so they don't collide with nxdk's SDL2).
2. The shim runs the mixer's pull callback on its own thread.
3. Output is 32 kHz stereo into a lock-free ring.
4. A high-priority pump thread resamples to 48 kHz and feeds the AC97,
   which is polled rather than interrupt-driven (from OpenCrossing: nxdk's
   IRQ path froze real hardware). Polled, a pump that misses its deadline
   (~150 ms) lets the bus master play to the last valid buffer and halt,
   and moving that index on doesn't restart it on the MCPX: one v13 boot
   was silent throughout (`audio 0%`, the ring never drained). The pump
   clears the sticky status bits and restarts a halted or stuck engine,
   logging `[AUDIO] AC97 halted/stuck ... restarting` (first eight). Every
   start, the first one included, goes through `aci_start`: reset the bus
   masters, queue seven buffers of audio from index 0, set LVI, and only
   then the run bit. Until v31 the run bit could be set on an empty
   descriptor 0: at boot the init raced the pump thread, a restart only
   toggled the run bit, and a cold reset zeroed the descriptors and ran at
   once. Then CIV stayed at 0 with the engine running, silent for the
   whole boot (v27, v31), and every boot logged one `halted` restart. After
   three restarts without a finished buffer the pump also cold-resets the
   AC-link (`[AUDIO] AC97 cold reset (n)` with the global control and
   status registers).
5. Under xemu (detected by CPUID) an MCPX APU voice is used instead.

## CARD (`card.c`)

- Slot A is a folder of `.gci` files at
  `E:\UDATA\4d580001\card_a\01-GALE-<name>.gci`: the 64-byte big-endian
  directory entry followed by the blocks, as Dolphin imports and exports
  them. Saves move between this port, Dolphin and a real card.
- The card is 251 blocks, and slot B is empty.
- Every call completes immediately; callbacks are deferred to the game
  thread.

### Byte order of Melee's files (`card_endian.c`)

`card.c` stores what HSD's card layer hands it, and HSD moves bytes. On a
GameCube card and in Dolphin, Melee's files hold its structs big-endian,
so the port converts at the boundary, in `src/melee/lb/lbcardgame.c`
(`PORT:`, under `TARGET_XBOX`):

| file (manifest slot) | struct | conversion |
|---|---|---|
| save data (1) | `GmSaveData`, 0x1790 bytes | field table |
| name tags (2-8) | `struct NameTagDataBank` x7, 0x1F2C bytes each | field table |
| header | comment (bytes), banner and icon (image bytes from `LbMcGame.dat`), `CardIconInfo` (bytes) | none; `lb_803BAB60` is now the GameCube's bytes, see below |
| snapshots (`lbsnap.c`) | header `Unk80433380_0` + JPEG | the header is a `DISC_STRUCT` (big-endian in memory) except `x14`, now swapped when it is filled; the JPEG is a byte stream |

- **Read** (`lb_8001CBBC`): once `lb_8001BD34` returns, each file whose read
  finished (`lb_8001B6E0` gives 0 or 2) is converted in place with
  `xsdk_card_from_card`. HSD has already checked the block checksums on the
  card's bytes by then.
- **Create and write** (`lb_8001C8BC`, `lb_8001CC84`): the manifest handed
  to lbcardnew points at `card_image`, a static big-endian copy of the save
  data and name tags (60 KB) taken when the operation is queued. The write
  runs over the following frames and verifies against its source, and the
  game keeps using (and changing) its own native copy meanwhile, so the
  live buffer is never swapped in place. HSD computes the block checksums
  from `card_image`, the same bytes a GameCube writes.
- **Field tables**: one row per member, in order, with the compiler's
  padding and the `u8` members as rows of their own; `u16`/`u32`/`s32`/
  `s64`/`u64` members and arrays are byte-reversed, nested structs
  (`GmStats`, `GamePrefs`, `FighterData[25]`, the `gmm_retval_*` structs,
  `NameTagData[19]`) have tables of their own. `FighterData.x7C` opens with
  a `u16` of bit-fields, which MWCC allocates from the top bit and the game
  triple from the bottom, so that unit is repacked field by field.
  `FighterData.x7A` is an `UnkFlagStruct`, a `DISC_STRUCT`, already in
  GameCube bit order. Neither struct has a float. The unnamed ranges
  (`padding_x1A70`, `padding_x1C88`, `padding_trophy_flags`, the `pad_*`
  and `padding*` members) are left as bytes; no code names them.
- **Older Melee-X saves** were written little-endian. Each file read is
  classified by a vote: every counter or record whose two readings differ
  votes for the one that is smaller (a small value read backwards is huge:
  `0x0225` <-> `0x2502`), and every `trophy_flags` entry
  (`0x8000 | 0x4000 | count`) for the reading with bits 8-13 clear; bit
  sets and bytes don't vote. Little-endian wins only with more votes, and
  the file is then left as it is, which is native already. The boot log
  says what was decided per file: `[CARD] save data big-endian (votes be
  N, le M)` or `[CARD] converted save data: little-endian (...)`. The
  game's next save writes it big-endian; nothing is rewritten at load.
- **Mixed files**: a Dolphin save that an older build loaded and saved
  again is big-endian except for the fields the game rewrote. The majority
  wins, and `[CARD] ... looks mixed` is logged when the minority is at
  least 4 votes and an eighth of the total; the rewritten fields stay wrong
  (a counter, the play time); reimport the original .gci if it matters.
- **Icon**: `lb_803BAB60`, the `CardIconInfo` new saves are created with,
  was declared as `u32`s holding the GameCube's bytes. On the Xbox that
  read as "no banner, no icon", so saves created before this fix have a
  comment-only header. They load and save normally (the header layout comes
  from the directory entry); a save created now has the banner and icon.
- Why `TARGET_XBOX`, not `TARGET_PC`: melee-pc's card backend (aurora) is
  not part of this port and its users' saves are native-order files; the
  .gci compatibility with Dolphin and real cards is this port's promise.

## Not built

melee-pc's netplay, ranked, LAN, Slippi replays, launcher, updater,
texture packs and custom music are not built. `stubs.c` is generated from
melee-pc's headers and reports each of them as off. `features.c` turns off
UCF, the free camera, frozen stadium and unlock-all, which gives vanilla
gameplay.
