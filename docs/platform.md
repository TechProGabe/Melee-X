# Platform layer

How the Dolphin SDK is implemented on the Xbox (`xbox/src/sdk`) and what
sits under it (`xbox/src/hw`).

## Three kinds of code

Game code (`src/melee`, `src/sysdolphin`, parts of `src/pc`) is lowered
and compiled with the game triple; sdk code (`xbox/src/sdk`,
`src/pc/audio.c`, `src/pc/misc.c`, aurora's MTX) shares structs with the
game, so it uses the same triple without lowering; hw code (`xbox/src/hw`:
kernel, pbkit, AC97, USB) uses nxdk's `i386-pc-win32` (`docs/toolchain.md`).
sdk and hw talk only through `xbox/include/xhw.h` and `xgx.h`, whose plain
types (no bit-fields, no 64-bit members) lay out the same on both.

`xbox/include/game/xbox_game_prelude.h` is force-included into game and sdk
code: `__int64` -> `long long`, the `M_*` constants, `atan2f`/`acosf`/
`asinf`/`expf`/`powf` renamed to `melee_*` (the game's own versions), and
the printf family routed to `xsdk_*` (the log) unless
`XSDK_NO_STDIO_RENAME`. nxdk's pdclib printf has no floating point (it skips
`%f` without taking the double, so later arguments are read 4 bytes off):
`log.c`'s `xsdk_vsnprintf` formats float conversions itself and hands the
rest to pdclib one by one. The game's own `sprintf`/`snprintf` still use
pdclib (a `%f` there prints nothing).

## Boot

`xhw_main.c`: mount E:, create `E:\UDATA\4d580001\`, open `boot.log`, log
the previous exit ("Logs"); show the splash (`xhw_splash.c`: "TechProGabe
Presents...", drawn by the CPU, 2 s, any button skips, a load bar and a
"Hold BACK for 480i" line; `-DXHW_NO_SPLASH`, `-DXHW_SPLASH_MS=<n>`,
`-DXHW_SPLASH_DUMP`); find the first `.iso`, `.gcm` or `.ciso` next to the
XBE (D:\). Then `boot.c`: `xsdk_early` reads BACK (480i, see VI) and loads
the settings; `xsdk_boot` runs OSInit, opens the image and checks it is
GALE01 (revision 2 is the target, 0 and 1 load with a warning, later ones
are refused), region, fonts from the DOL, then `xhw_video_boot` (ends the
splash), widescreen mode and `melee_main`. The game thread and every
`xhw_thread_start` thread run inside `xhw_crash_guard`.

## Logs

`boot.log` is in the save folder (the previous one becomes `boot_prev.log`;
past 4 MB it goes on in `boot2.log`/`boot3.log`). When the save folder
can't be written or E: has less than 1 MB free, it goes to D:\ next to
`default.xbe` (`xhw_log_fallback`, `xhw_sys.c`), with `[BOOT] can't write
...`, a notice and the title's hint line. Every way out of the XBE that
runs code writes `lastexit.txt`; the next boot logs it as `[BOOT] previous
exit: ...` and deletes it (no file: switched off, reset or killed).

## OS (`os.c`)

MEM1 is 24 MB at VA 0x10000000, committed on demand
(`docs/architecture.md`), above 16 MB because `PC_IS_ARAM_ADDR` treats
anything lower as an ARAM offset; low memory holds the OSBootInfo fields,
the arena starts at +0x4000. Interrupts follow melee-pc: the game is
single-threaded, `OSDisableInterrupts` is a recursive lock shared with the
worker threads (depth in a TLS slot; the game triple can't use
`__thread`), and alarms and deferred completions (ARQ, CARD) run on the
game thread when it re-enables interrupts and at every frame boundary. The
time base is 40.5 MHz from 2000-01-01, seeded from the Xbox's clock.

## DVD (`dvd.c`)

Reads the FST at boot. `.iso`/`.gcm` are raw; a `.ciso` has a block map
after a 0x8000-byte header, missing blocks read as zeros. Async reads run
on one worker thread, the callback there holding the interrupt lock as the
GameCube's DVD interrupt did. Every read commits its destination first
(`xhw_commit`): the kernel's file system can't take the demand-commit
fault. `DVDGetDOLLocation` feeds `discfont.c`, which reads the fonts out of
the game's own DOL.

## ARAM (`ar.c`)

16 MB at VA 0x12000000, committed on demand; ARAM addresses are offsets
into it, the first 16 KB kept free as the SDK does for the DSP.
`ARAlloc`/`ARFree` are the SDK's bump allocator; ARQ transfers are `memcpy`
at post time, callbacks deferred to the game thread. `src/pc/audio.c` reads
samples straight out of the buffer (`aurora_aram_base()`), which is why the
fault handler commits ARAM, not only the copies. Chunks holding bytes
straight from the disc are decommitted and read back when needed.

## PAD (`pad.c`, `xhw_pad.c`)

Player N is physical port N (`SDL_JoystickGetDevicePlayerIndex`), so
plugging and unplugging doesn't shuffle players. Default layout is
GameCube-like by position (`docs/architecture.md` "Input"). Sticks get a
radial dead zone, then are scaled so full tilt reaches the GameCube's raw
rim (±104); the game clamps to its 80-unit circle. The digital L/R click
fires past `trigger_click`. L and R past 200 with BACK and BLACK, on any
port, quits to the dashboard (L + R + START is Melee's own reset). Rumble
is scaled by `[input] rumble`; 100 drives the motors at 75%.

## `settings.ini`

Only `E:\UDATA\4d580001\settings.ini` is read (test builds with
`-DXSDK_SETTINGS_RESET` first replace it with `D:\settings.ini`). It is
written with the defaults on first boot, and by the settings menu:

```ini
[video]
720p = 1            ; 720p (16:9) where the dashboard allows it
progressive = 1     ; 0: 480i even where the dashboard allows 480p
widescreen = 1      ; 16:9 at 480 when the dashboard is set to widescreen
fps = 0             ; frame-rate counter; default 1 in test builds (XHW_TEST_BUILD)
[system]
ram128 = 0          ; 1: use the RAM above 64 MB on an upgraded console (untested)
screenshots = 0     ; BACK saves shotNN.bmp; default 1 in test builds
led_effects = 0     ; front LED effects ("Front LED")
[input]
rumble = 100        ; percent
[port1]             ; .. [port4]
stick_deadzone = 20 ; percent, radial (0-60); cstick_deadzone = 25
trigger_click = 230 ; 1-255
a = A               ; Xbox button = GameCube button (A B X Y Z L R START UP DOWN LEFT RIGHT NONE);
b = X               ; x = B, y = Y, white/black = Z, back/lstick/rstick = NONE, start, d-pad as is
```

A file missing `fps`, `progressive`, `ram128` or `led_effects` is rewritten
with them added. The old key `led` (v43-v48 wrote 1) is ignored. On a 64 MB
console (`xhw_mem_has_upper`) `ram128 = 1` is ignored at boot and written as
0 by the next save.

Writing (`settings.c`): `settings.tmp`, ending in `; end of settings`, is
flushed, read back, and renamed over `settings.ini` (`xhw_replace_file`)
only if it is whole and parses to the same settings; a failed write leaves
the old file. If the old file was deleted but the rename failed, the next
boot takes a whole `settings.tmp`. Comments and unknown keys are dropped.

### Settings menu (`menu.c`)

BACK on the title screen opens a panel over the title. The title's hint
line reads "BACK: Melee-X settings", or "Can't write to E: (full?): nothing
will be saved" while `xhw_log_fallback()` is set. `gmtitle.c` calls
`xsdk_menu_title_frame` every title frame (`PORT:`); while the panel is up
the title stands still (no START, attract timer held) and `PADRead` hands
the game neutral input until the closing press is let go. The in-game
reset still works.

| row | values | applies |
|---|---|---|
| Video output | 480i, 480p, 720p (`480p -> 480i` etc. when the dashboard doesn't allow it) | restart |
| Widescreen (16:9) | On, Off (`On -> Off` at 480 on a 4:3 dashboard) | restart |
| Frame-rate counter, BACK screenshots | On, Off | at once |
| Front LED effects | On, Off | at once (On: a 1.5 s sweep; Off: LED handed back) |
| Use 128 MB RAM | On, Off; greyed "Off (64 MB console)" on 64 MB | restart |
| Rumble | Off, 25-100% | at once, with a pulse |
| Controller | Port 1-4 for the three rows below | |
| Stick / C-stick dead zone | 0-60%, steps of 5, live readout | on close |
| Trigger click | 5-255, steps of 5, live readout | on close |
| Save and restart | writes the file, relaunches this XBE by its kernel path (`XeImageFileName`, `xhw_reboot_self`) | |
| Save and close | writes the file (B or BACK too) | |

Every controller drives it by raw buttons, so a remapped pad still works.
Restart-only values get a `*` while they differ from the running ones;
closing saves only when something changed. Button mapping is file-only.
The platform draws the panel over the finished frame (`docs/renderer.md`):
Melee's menus are models with prebaked text. It is 18 rows, 91% of the
height at 720p; a new row has to replace one to stay TV-safe.

## Front LED (`xhw_led.c`)

Off by default: on a Kronos-modded console the effects fought the chip and
it needed a Cerbios recovery (`decisions.md`). SMC (SMBus 0x20) register
0x08 takes a four-step sequence the SMC cycles itself (bit 7-n step n's
red, bit 3-n its green, as nxdk's `XSetCustomLED`); 0x07 = 1 switches to
it, 0x07 = 0 hands the LED back. The first effect that applies wins:

| event | pattern | how long |
|---|---|---|
| GAME! / TIME! (`gmvs.c`) | red, orange, green, orange sweep | 3 s, into the next scene |
| KO (`ftCo_800D34E0`) | port colour blinking: P1 red, P2 red/green, P3 orange, P4 green | 1.5 s |
| timer last 10-6 / 5-3 / 2-1 s (`fn_8016CD98`) | orange blip / orange blinking / orange-red | |
| stock match, someone on their last stock | green x3, a red tick | until the match ends |
| anything else | the SMC's | |

A scene change (`xsdk_scene_log`) clears all but a running sweep. The game
thread only posts events into a 16-entry ring (nothing with the setting
off); a lowest-priority worker, started only when it is on, writes a
pattern when it changes, at most every 80 ms, so no SMBus wait holds the
game (`[LED] <steps> <why>` / `[LED] SMC`, up to 300 lines a boot). Leaving
the XBE waits up to 300 ms for the hand-back; a crash or hang report only
signals the worker. A failed SMBus write is logged once and the LED left
alone. xemu doesn't show the LED; the `[LED]` lines are the test.

## VI (`vi.c`, `xhw_video.c`)

720p (1280x720, 16-bit, always 16:9) when `720p = 1`, the dashboard
allows it and at least 32 MB is free at boot (if it can't be set up,
`xhw_video_fallback_480`). Otherwise 640x480x32: 480p when the dashboard
allows it and `progressive = 1`, else 480i (on an HDTV pack set to 480p,
via nxdk's `XVideoInit`; PAL has no progressive modes in nxdk); 16:9 if
the dashboard is set to widescreen. BACK held at boot gives 480i and saves
`720p = 0`, `progressive = 0`, for a TV that doesn't show 720p.
`VIWaitForRetrace` is the frame boundary: it paces to 60.000 Hz, runs due
alarms (the pad poll), then the retrace callbacks; `VISetBlack` is
honoured at the flip.

## Audio (`src/pc/audio.c`, `sdl3_audio.c`, `xhw_audio.c`)

melee-pc's software AX mixer runs unchanged; its SDL3 audio-stream calls go
to a shim (`xbox/include/sdk/SDL3/SDL.h`, renamed `xsdk_SDL_*` to avoid
nxdk's SDL2) that runs the pull callback on its own thread, 32 kHz stereo
into a lock-free ring. A high-priority pump thread resamples to 48 kHz and
feeds the AC97, polled rather than interrupt-driven (nxdk's IRQ path froze
real hardware). Under xemu (CPUID) an MCPX APU voice is used instead.

- **Boot**: logs the AC97 and APU as found (`[AUDIO] found`), then idles
  whatever ran before (our engine after a crash, the dashboard's
  DirectSound): both bus masters (PCM, S/PDIF) halted, the APU's
  interrupts, setup engine and DSPs stopped, a cold reset, the codec
  powered up (`[AUDIO] idle`).
- **Start** (`aci_start`): reset the bus masters, queue seven buffers from
  index 0, set LVI, then the run bit (never on an empty descriptor 0).
- **Recovery**: a missed deadline (~150 ms) halts the bus master; the pump
  restarts a halted or stuck engine (`[AUDIO] AC97 halted/stuck`), and
  after three restarts without a finished buffer cold-resets the AC-link.
- **Giving up**: an engine that has finished no buffer since boot, after
  one recovery cold reset, is stopped (`[AUDIO] stuck since boot`): no more
  cold resets (each froze the game ~1 s), the mixer drained in real time so
  the game runs silent, and a 10 s notice to power-cycle the Xbox.

## CARD (`card.c`)

- Slot A is a folder of `.gci` files,
  `E:\UDATA\4d580001\card_a\01-GALE-<name>.gci`: the 64-byte big-endian
  directory entry followed by the blocks, as Dolphin imports and exports
  them, so saves move between this port, Dolphin and a real card. 251
  blocks; slot B is empty. Every call completes at once, callbacks
  deferred to the game thread.
- **Writing**: `<name>.tmp` is written, flushed and renamed over the `.gci`
  (`xhw_replace_file`), so a full E: or a power cut leaves the previous
  save. On failure a create or rename is undone, the game gets
  `CARD_RESULT_IOERROR`, `[CARD] FAILED to save` is logged and a notice
  goes up.
- **At load** (`recover_tmp`): a whole `.tmp` (header plus all its blocks)
  whose `.gci` is missing becomes the `.gci` (the old file was deleted but
  the rename failed); any other `.tmp` is an unfinished write, deleted.

### Byte order of Melee's files (`card_endian.c`)

Melee's files hold its structs big-endian on a card and in Dolphin, so
`src/melee/lb/lbcardgame.c` converts at the boundary (`PORT:`,
`TARGET_XBOX`, not `TARGET_PC`, whose aurora card backend isn't built):

| file (manifest slot) | struct | conversion |
|---|---|---|
| save data (1) | `GmSaveData`, 0x1790 bytes | field table |
| name tags (2-8) | `struct NameTagDataBank` x7, 0x1F2C bytes each | field table |
| header | comment, banner, icon, `CardIconInfo` | none (bytes) |
| snapshots (`lbsnap.c`) | header `Unk80433380_0` + JPEG | header is a `DISC_STRUCT`; `x14` swapped when filled |

- **Read** (`lb_8001CBBC`): each finished file is converted in place
  (`xsdk_card_from_card`) after HSD has checked the block checksums.
- **Create and write** (`lb_8001C8BC`, `lb_8001CC84`): the manifest points
  at `card_image`, a static big-endian copy (60 KB) taken when the
  operation is queued, so the game's native copy, which it keeps changing
  during the write, is never swapped in place.
- **Field tables** (`tools/xbox/test_card_endian.py`): one row per member
  in order; multi-byte members and arrays byte-reversed, nested structs
  with tables of their own, unnamed padding left as bytes.
  `FighterData.x7C`'s `u16` of bit-fields is repacked field by field (MWCC
  allocates from the top bit, the game triple from the bottom).
- **Older Melee-X saves** were little-endian. A vote decides per file
  (counters vote for the reading that is smaller, `trophy_flags` entries
  for the one with bits 8-13 clear); little-endian needs more votes and is
  then used as is: `[CARD] save data big-endian (votes be N, le M)` or
  `[CARD] converted save data: little-endian (...)`. The next save writes
  big-endian. A close vote logs `looks mixed` (a Dolphin save an older
  build re-saved; reimport the original .gci if it matters).

## Not built

melee-pc's netplay, ranked, LAN, Slippi replays, launcher, updater, texture
packs and custom music are not built. `stubs.c`, generated from melee-pc's
headers, reports each as off. `features.c` turns off UCF, the free camera,
frozen stadium and unlock-all: vanilla gameplay.
