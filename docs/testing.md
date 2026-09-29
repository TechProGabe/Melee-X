# Testing and debugging

## Host tests (no Xbox needed)

```sh
tools/lower/test_lower.py        # disc_lower vs GCC scalar_storage_order (needs GCC 12+)
tools/xbox/test_vp_encoder.py    # vertex-program encoder vs nv2a-vsh (pip install nv2a-vsh)
tools/xbox/test_tex_convert.py   # native texture formats vs the GX decoder
```

CI (`.github/workflows/build.yml`) runs all three after building
`default.xbe`, and uploads the XBE with its link map.

## Running it

You need your own Melee NTSC-U 1.02 image (`GALE01`, revision 2) as
`.iso`, `.gcm` or `.ciso`. No game data belongs in this repository: the
`.gitignore` blocks images, DOLs, BIOS files and saves.

**On an Xbox:** copy `default.xbe` and the image into one folder, for
example `E:\Games\Melee-X\`, and launch the XBE from your dashboard.

**In xemu:** make an XISO of a folder holding `default.xbe` and the image,
using `extract-xiso -c <folder>`, and load it as the DVD. The image is
then found on D:. A retail-compatible BIOS, MCPX ROM and HDD image are
needed, as for any xemu title. xemu is useful for crashes and rendering, but
timing, audio (it uses the APU fallback there) and memory headroom differ
from hardware, so check fixes on a console too.

On macOS the scripts do all of this for you. They use Docker (colima works)
and xemu at `/Applications/Xemu.app`, with its BIOS, MCPX and HDD already
set up in xemu's settings:

```sh
docker build -t melee-x:sdk tools/xbox/docker   # once
tools/xbox/docker/build.sh                      # -> build-xbox/xbe/default.xbe
MX_ISO=~/roms/melee.iso tools/xbox/xemu_run.sh 120 'melee_main'
```

`xemu_run.sh [seconds] [stop-regex]` packs the XISO, boots it, logs COM1 to
`~/xemu/mx-run/serial.log`, and stops after that many seconds or when the
regex matches. See the script header for the `MX_*` variables:

- `MX_STAGE_EXTRA` adds files to the disc, for example `autopad.txt`.
- `MX_GUI=1` leaves xemu running.
- `MX_XEMU_ARGS="-monitor unix:/tmp/mxmon.sock,server,nowait"` adds a QEMU
  monitor. `tools/xbox/xemu_prof.py` then samples where the CPU spends its
  time.
- `XBOX_CFLAGS` (for example `-DXHW_AUTOPAD=1`) passes through to the
  platform build.

Screenshots come out of the serial log as `[FBDUMP]` lines. Decode them
with `tools/xbox/fbdump_to_png.py serial.log shot`.

If the log stops dead, heartbeat included, the guest has bugchecked. In the
monitor, `info registers` then shows `HLT=1` with IF clear, and the
bugcheck code is on the stack (`0x7F, 8` is a double fault). A double fault
arrives through a task gate, so the faulting EIP and ESP are in the TSS
that the current TSS's link field names (read the GDT to find it).

## Logs

Everything is written to `E:\UDATA\4d580001\` (the title ID is `4d580001`):

| file | contents |
|---|---|
| `boot.log` | the log (first 256 KB), every line flushed during the first 600 frames, then at most once a second. Tags: `[BOOT]` `[MEM]` `[OS]` `[DVD]` `[NV2A]` `[PAD]` `[AUDIO]` `[CARD]` |
| `crash.log` | written on a CPU exception: the last log lines, the fault, registers, XBE addresses found on the stack |
| `settings.ini` | options (`docs/platform.md`) |
| `card_a\*.gci` | memory card saves |

The log also goes to COM1 (the serial port), which xemu can redirect to a terminal or file.

Lines worth reading first:

- `[MEM] boot` and `[MEM] after nv2a`: free RAM, plus how much of MEM1 and
  ARAM has been committed so far. If free RAM runs low while the game is
  still loading, that is the 64 MB budget (`docs/architecture.md`).
- `[DVD] GALE01 rev 2, N FST entries`: the image was accepted.
- `[NV2A] frame N: D draws (A approximated), tex pool K KB free`: logged
  every 600 frames. A high `approximated` count means TEV setups the
  combiners only approximate; a tex pool near 0 means texture churn.

## Crashes

The crash guard catches the exception and shows the report on screen. It
also writes `crash.log`, then parks the thread. Symbolize the log with the
link map from the **same** build:

```sh
tools/xbox/sym.py crash.log                          # map: build-xbox/melee_x.map
tools/xbox/sym.py --map path/to/melee_x.map crash.log
tools/xbox/sym.py 0036c4f4 0014b6d0                  # single addresses
```

A fault at raised IRQL (inside a DPC) can't write files. It shows only the
screen report, so photograph it.

A fault address inside 0x10000000-0x11800000 (MEM1) or
0x12000000-0x13000000 (ARAM) that is reported as a crash, and not quietly
committed, means one of two things: the fault happened at raised IRQL, or
the commit failed because RAM ran out. Compare with `free` on the same
report.

## Build switches

| switch | effect |
|---|---|
| `-DXHW_CRASH_GUARD=0` | no SEH guard. Crashes become bugchecks, and demand-committed memory stops working, so debug only |
| `-DXHW_AUDIO_APU=0` | never use the xemu APU fallback |
| `-DXHW_AUTOPAD=1` | scripted input from `D:\autopad.txt`: `<frame> <buttons/SHOT> [for N]` per line (`xhw_autopad.c`) |
| `-DXHW_FBDUMP_EVERY=<n>` | screenshot every n presented frames |
| `-DXGX_STATS_EVERY=<n>` | `[NV2A]` / `[TEX]` stats period, in frames (default 600) |
| `-DXHW_WATCHDOG=0`, `-DXHW_HEARTBEAT_SECS=<n>` | hang dumper off; `[BEAT]` period (0 = off) |
| `-DXHW_NO_SPLASH`, `-DXHW_SPLASH_MS=<n>` | boot title card off; its hold time |
| `XBOX_FORCE=1 tools/xbox/compile_game.py` | rebuild every game unit |
| `XBOX_KEEP_TEMPS=1` | keep the `.i` / `.lowered.c` intermediates |
| `XBOX_CFLAGS`, `XBOX_CMAKE_ARGS`, `XBOX_NINJA_ARGS` | passed through by `xbox/build.sh`; `XBOX_CFLAGS` sets the platform's C flags |

## First-boot checklist

1. Boots past the "no disc image" screen, and `boot.log` shows the
   `[DVD]` line.
2. The title screen draws, and `[NV2A]` lines appear.
3. Audio plays on hardware and in xemu.
4. All four controllers map to players 1-4 by port, and hot-plugging
   doesn't shuffle them.
5. At 720p the picture is 16:9 with the HUD at the screen edges. At 480
   (4:3 dashboard) it matches the GameCube framing.
6. A 4-player VS match on a busy stage holds 60 fps.
7. Saving creates `card_a\01-GALE-*.gci`, and it loads back after a reboot.
