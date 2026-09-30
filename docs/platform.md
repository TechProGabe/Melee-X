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
- Rumble goes to the pad's motors, scaled by `[input] rumble`.

## `settings.ini`

`E:\UDATA\4d580001\settings.ini` is written with the defaults on first boot:

```ini
[video]
720p = 1            ; use 720p (16:9) when the dashboard allows it
widescreen = 1      ; 16:9 at 480 when the dashboard is set to widescreen
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

## VI (`vi.c`, `xhw_video.c`)

- 720p is used when the dashboard allows it, `720p = 1`, and at least 32 MB
  is free at boot. Otherwise the mode is 480p/480i at 32 bits, 16:9 if the
  dashboard is set to widescreen.
- If 720p can't be set up, `xhw_video_fallback_480` drops to 480.
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
   logging `[AUDIO] AC97 halted/stuck ... restarting` (first eight).
5. Under xemu (detected by CPUID) an MCPX APU voice is used instead.

## CARD (`card.c`)

- Slot A is a folder of `.gci` files at
  `E:\UDATA\4d580001\card_a\01-GALE-<name>.gci`: the 64-byte big-endian
  directory entry followed by the blocks, as Dolphin imports and exports
  them. Saves move between this port, Dolphin and a real card.
- The card is 251 blocks, and slot B is empty.
- Every call completes immediately; callbacks are deferred to the game
  thread.

## Not built

melee-pc's netplay, ranked, LAN, Slippi replays, launcher, updater,
texture packs and custom music are not built. `stubs.c` is generated from
melee-pc's headers and reports each of them as off. `features.c` turns off
UCF, the free camera, frozen stadium and unlock-all, which gives vanilla
gameplay.
