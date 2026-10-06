#!/usr/bin/env python3
"""Console rounds (docs/fps-plan.md): build variants in folders of their own,
chained so one launch runs them all. N is the round (CHAINS below).

  tools/xbox/console_round.py N stage BUILDS   # BUILDS/<build>.xbe, the builds the chain names
  tools/xbox/console_round.py N upload         # FTP every folder (MX_FTP_HOST)
  tools/xbox/console_round.py N watch          # save each run's log (at the dashboard)
  tools/xbox/console_round.py N report         # fps and ms a frame per bucket, per run
  tools/xbox/console_round.py N shots          # the runs' screenshots, the same bytes grouped
  tools/xbox/console_round.py N clean          # take the scripts and the rN folders off

Each folder F:\\Applications\\Melee-X-rN?\\ holds a test build (-DXHW_AUTOPAD=1)
and its autopad.txt; test builds take the disc image from
F:\\Applications\\Melee-X\\. A script ends with env MX_NEXT_XBE=<the next
folder>: 12 s after its match the next build boots. The last run is the
baseline (dev, no chaining) in Melee-X itself. Every boot keeps only the
previous boot's log (boot_prev.log) and the game serves no FTP, so a build
that launches the next one first renames its log to boot_<folder>.log;
`watch` pulls them whenever the console is at the dashboard.
Staged files go to $MX_HW/stage-rN, logs to $MX_HW/logs-rN."""
import collections
import ftplib
import hashlib
import io
import os
import re
import shutil
import sys
import time
from pathlib import Path

HW = Path(os.environ.get('MX_HW', Path.home() / 'xemu' / 'hw'))
STAGE = LOGS = None   # main(): per round
ROUND = 0
UDATA = '/E/UDATA/4d580001'
APPS = '/F/Applications'

FOD = ['env MELEE_BOOT_SCENE=vs', 'env MELEE_DEBUG_VS_STAGE=2', 'env MELEE_DEBUG_VS=cpu4',
       'env MELEE_DEBUG_VS_TIME=120', 'env MELEE_SEED=1']
FODLONG = ['env MELEE_BOOT_SCENE=vs', 'env MELEE_DEBUG_VS_STAGE=2', 'env MELEE_DEBUG_VS=cpu4',
           'env MELEE_DEBUG_VS_TIME=300', 'env MELEE_SEED=1']
FODSHOT = ['env MELEE_BOOT_SCENE=vs', 'env MELEE_DEBUG_VS_STAGE=2', 'env MELEE_DEBUG_VS=cpu4',
           'env MELEE_DEBUG_VS_TIME=30', 'env MELEE_SEED=1', 'env MX_LOCKSTEP=1',
           '200 TSHOT', '500 TSHOT', '800 TSHOT', '1200 TSHOT']
# the title's settings menu, which the CPU draws into the framebuffer
# (xhw_overlay.c): the title with its hint line, then the menu; NEXT leaves
# with the menu open, so settings.ini isn't written
MENU = ['env MELEE_BOOT_SCENE=title', 'env MELEE_NO_ATTRACT=1', 'env MX_LOCKSTEP=1',
        '1200 SHOT', '1230 BACK', '1290 SHOT', '1350 NEXT']
# scenarios/stall, timed: Fountain with each clearing EFB copy repeated
# MX_COPY_STRESS times (the GPU stall after a copy, nv2a.c XGX_COPY_FIX)
STALL = ['env MELEE_BOOT_SCENE=vs', 'env MELEE_DEBUG_VS_STAGE=2', 'env MELEE_DEBUG_VS=cpu4',
         'env MELEE_DEBUG_VS_TIME=240', 'env MELEE_SEED=1']
# round 8: 8 minutes of it (each run's hang goes on to the next build)
STALL8 = ['env MELEE_BOOT_SCENE=vs', 'env MELEE_DEBUG_VS_STAGE=2', 'env MELEE_DEBUG_VS=cpu4',
          'env MELEE_DEBUG_VS_TIME=480', 'env MELEE_SEED=1']
STALL12 = ['env MELEE_BOOT_SCENE=vs', 'env MELEE_DEBUG_VS_STAGE=2', 'env MELEE_DEBUG_VS=cpu4',
           'env MELEE_DEBUG_VS_TIME=720', 'env MELEE_SEED=1']
FD = ['env MELEE_BOOT_SCENE=vs', 'env MELEE_DEBUG_VS_STAGE=32', 'env MELEE_DEBUG_VS_CHARS=2,8',
      'env MELEE_DEBUG_VS_TIME=60', 'env MELEE_SEED=1']
# Pokémon Stadium, 4 CPUs: the frame rate, and lockstep shots of the
# screen's red arrowheads (roadmap item 11, renderer.md "Depth")
PS = ['env MELEE_BOOT_SCENE=vs', 'env MELEE_DEBUG_VS_STAGE=3', 'env MELEE_DEBUG_VS=cpu4',
      'env MELEE_DEBUG_VS_TIME=120', 'env MELEE_SEED=1']
PSSHOT = ['env MELEE_BOOT_SCENE=vs', 'env MELEE_DEBUG_VS_STAGE=3', 'env MELEE_DEBUG_VS=cpu4',
          'env MELEE_DEBUG_VS_TIME=40', 'env MELEE_SEED=1', 'env MX_LOCKSTEP=1',
          '600 TSHOT', '1200 TSHOT', '1800 TSHOT']
# folder, build, scenario, switches: in the order they run. Rounds 2 and 3
# end on dev's baseline in Melee-X itself, which can't chain: it stays on
# its results. A round whose last folder is its own ends at the dashboard
# by itself (env MX_NEXT_XBE=dashboard), ready for `watch`.
# round 2 (2026-10-03): MX_DEFER (B4) and MX_PREFETCH (B3) were removed after it
ALL = {'MX_DEFER': '1', 'MX_MEM1_LARGE': '6', 'MX_PREFETCH': '1'}
CHAINS = {}
CHAINS[2] = [
    ('Melee-X-r2a', 'head', 'fod', {}),
    ('Melee-X-r2b', 'lto', 'fod', {}),
    ('Melee-X-r2c', 'ltopgo', 'fod', {}),
    ('Melee-X-r2d', 'ltopgo', 'fod', {'MX_DEFER': '1'}),
    ('Melee-X-r2e', 'ltopgo', 'fod', {'MX_MEM1_LARGE': '6'}),
    ('Melee-X-r2f', 'ltopgo', 'fod', {'MX_PREFETCH': '1'}),
    ('Melee-X-r2g', 'ltopgo', 'fod', ALL),
    ('Melee-X-r2h', 'head', 'fd', {}),
    ('Melee-X-r2i', 'ltopgo', 'fd', {}),
    ('Melee-X-r2j', 'ltopgo', 'fd', ALL),
    ('Melee-X', 'base', 'fod', {}),
]
# round 3: the final build (LTO + PGO, profile retrained) twice, B2 twice,
# and A2's function order off, with and without LTO + PGO (round 2's r2a
# ran 3.4% below dev's baseline)
B2 = {'MX_MEM1_LARGE': '6'}   # B2, removed after round 3
CHAINS[3] = [
    ('Melee-X-r3a', 'ltopgo', 'fod', {}),
    ('Melee-X-r3b', 'ltopgo', 'fod', B2),
    ('Melee-X-r3c', 'ltopgo-noorder', 'fod', {}),
    ('Melee-X-r3d', 'head', 'fod', {}),
    ('Melee-X-r3e', 'head-noorder', 'fod', {}),
    ('Melee-X-r3f', 'ltopgo', 'fod', {}),
    ('Melee-X-r3g', 'ltopgo', 'fod', B2),
    # the GPU's frame by window (-DXHW_PMC=1, LTO + PGO): none, no shadow
    # maps, no reflection, no fill, no EFB copies, ~10 s each over 5 minutes
    ('Melee-X-r3h', 'probe', 'fodlong', {'MX_ABLATE': '0,2,3,8,9'}),
    ('Melee-X', 'base', 'fod', {}),
]
# round 4: the final build (PGO retrained on the gate's code) and the tile
# regions (docs/renderer.md "Tile regions"): MX_TILE 0 (pbkit's setup), 8
# (no Z compression), 1 (Z16 compression format), 2 / 4 (the colour tile
# enabled as base|1 / base|3), 3 and 5 (both). A warm-up run first (the
# first build of a chain runs ~4% slow); then lockstep shots of 0, 3, 5 and
# 8, which must be byte for byte the same.
CHAINS[4] = [('Melee-X-r4a', 'final', 'fod', {})] + [
    (f'Melee-X-r4{chr(98 + i)}', 'final', 'fod', {'MX_TILE': str(t)}) for i, t in enumerate((0, 8, 1, 2, 4, 3, 5))
] + [
    (f'Melee-X-r4{chr(105 + i)}', 'final', 'fodshot', {'MX_TILE': str(t)}) for i, t in enumerate((0, 3, 5, 8))
]
# round 5: round 4's colour tile (MX_TILE 4: pbkit's base|2 plus the
# enable bit) with the CPU's framebuffer access through the NV2A's aperture
# (nv2a.c fb_cpu). The settings menu and lockstep shots with the tile off
# and on: a shot with the tile on is written twice, through the aperture
# and at pbkit's address (the tiled layout), so the first must match the
# tile-off run's. Then the frame rate with the tile on once more.
CHAINS[5] = [
    ('Melee-X-r5a', 'final', 'menu', {'MX_TILE': '0'}),
    ('Melee-X-r5b', 'final', 'menu', {'MX_TILE': '4'}),
    ('Melee-X-r5c', 'final', 'fodshot', {'MX_TILE': '0'}),
    ('Melee-X-r5d', 'final', 'fodshot', {'MX_TILE': '4'}),
    ('Melee-X-r5e', 'final', 'fod', {'MX_TILE': '4'}),
]
# round 6, the final one: the build releases will be (LTO + PGO, the colour
# tile on) at 720p twice and on Final Destination; the merged clear
# (MX_COPY_FIX 7) and the state trims (MX_TRIM 15, removed after it, 56d5ab1)
# alone and together; 480p with the tile on and off; lockstep shots
# that must match byte for byte (480p tile off against on, 720p defaults
# against both switches); the settings menu; and the merged clear under the
# copy stress (the GPU stall it sits next to, scenarios/stall), last.
SWITCHES = {'MX_COPY_FIX': '7', 'MX_TRIM': '15'}
CHAINS[6] = [
    ('Melee-X-r6a', 'final', 'fod', {}),   # warm-up
    ('Melee-X-r6b', 'final', 'fod', {}),
    ('Melee-X-r6c', 'final', 'fod', {'MX_COPY_FIX': '7'}),
    ('Melee-X-r6d', 'final', 'fod', {'MX_TRIM': '15'}),
    ('Melee-X-r6e', 'final', 'fod', SWITCHES),
    ('Melee-X-r6f', 'final', 'fod', {}),
    ('Melee-X-r6g', 'final', 'fd', {}),
    ('Melee-X-r6h', 'final', 'fod', {'MX_VIDEO': '480'}),
    ('Melee-X-r6i', 'final', 'fod', {'MX_VIDEO': '480', 'MX_TILE': '0'}),
    ('Melee-X-r6j', 'final', 'fodshot', {'MX_VIDEO': '480', 'MX_TILE': '0'}),
    ('Melee-X-r6k', 'final', 'fodshot', {'MX_VIDEO': '480'}),
    ('Melee-X-r6l', 'final', 'fodshot', {}),
    ('Melee-X-r6m', 'final', 'fodshot', SWITCHES),
    ('Melee-X-r6n', 'final', 'menu', {}),
    ('Melee-X-r6o', 'final', 'stall', {'MX_COPY_FIX': '7', 'MX_COPY_STRESS': '40'}),
]
# round 7 (v53): 720p depth, Z24S8 (MX_Z24 1, the new default) against v52's
# Z16 (0), twice each after a warm-up, on Fountain and Pokémon Stadium; Z24S8
# without Z compression (MX_TILE 12 = 4 + 8) prices the compression; FD 1v1
# must hold 60. Then lockstep Stadium shots (the red arrowheads on the
# screen's frame, ticks 600/1200/1800: whole with 1, slivers with 0) and
# Fountain's (against round 6's r6l).
CHAINS[7] = [
    ('Melee-X-r7a', 'final', 'fod', {}),   # warm-up
    ('Melee-X-r7b', 'final', 'fod', {'MX_Z24': '0'}),
    ('Melee-X-r7c', 'final', 'fod', {'MX_Z24': '1'}),
    ('Melee-X-r7d', 'final', 'fod', {'MX_Z24': '0'}),
    ('Melee-X-r7e', 'final', 'fod', {'MX_Z24': '1'}),
    ('Melee-X-r7f', 'final', 'fod', {'MX_Z24': '1', 'MX_TILE': '12'}),
    ('Melee-X-r7g', 'final', 'ps', {'MX_Z24': '0'}),
    ('Melee-X-r7h', 'final', 'ps', {'MX_Z24': '1'}),
    ('Melee-X-r7i', 'final', 'fd', {'MX_Z24': '1'}),
    ('Melee-X-r7j', 'final', 'psshot', {'MX_Z24': '0'}),
    ('Melee-X-r7k', 'final', 'psshot', {'MX_Z24': '1'}),
    ('Melee-X-r7l', 'final', 'fodshot', {'MX_Z24': '1'}),
]
# round 8 (v4.1): the 720p GPU stall after an EFB copy (v54 burn-in, 44 min:
# LIMIT_ZETA on the Z clear after a copy). The copies switched the zeta
# format to Z16 and back at 720p (Z24S8 since v53); MX_COPY_ZFMT 1 keeps
# the screen's (v4.1's default), 0 is v54's. Copy stress 40 for 8 minutes:
# v54's path (expected to stall: the run's hang chains on), v4.1's twice,
# v4.1's with the format re-sent (MX_COPY_FIX 13), 480p as a check; then
# lockstep shots that must match byte for byte (the picture doesn't change),
# and Stadium (its screen is a copy) at v4.1's defaults.
CHAINS[8] = [
    ('Melee-X-r8a', 'final', 'stall8', {'MX_VIDEO': '720', 'MX_COPY_ZFMT': '0', 'MX_COPY_STRESS': '40'}),
    ('Melee-X-r8b', 'final', 'stall8', {'MX_VIDEO': '720', 'MX_COPY_ZFMT': '1', 'MX_COPY_STRESS': '40'}),
    ('Melee-X-r8c', 'final', 'stall8', {'MX_VIDEO': '720', 'MX_COPY_ZFMT': '1', 'MX_COPY_STRESS': '40', 'MX_COPY_FIX': '13'}),
    ('Melee-X-r8d', 'final', 'stall8', {'MX_VIDEO': '720', 'MX_COPY_ZFMT': '1', 'MX_COPY_STRESS': '40'}),
    ('Melee-X-r8e', 'final', 'stall8', {'MX_VIDEO': '480', 'MX_COPY_STRESS': '40'}),
    ('Melee-X-r8f', 'final', 'fodshot', {'MX_VIDEO': '720', 'MX_COPY_ZFMT': '0'}),
    ('Melee-X-r8g', 'final', 'fodshot', {'MX_VIDEO': '720', 'MX_COPY_ZFMT': '1'}),
    ('Melee-X-r8h', 'final', 'ps', {'MX_VIDEO': '720'}),
]
# Round 8's result (2026-10-05): v54's path stalled twice (frames 3314 and
# 4241, ~11 fps under stress 40, so ~5-6 min in; the same LIMIT_ZETA as the
# burn-in). MX_COPY_ZFMT 1 is no fix: SET_SURFACE_FORMAT with R5G6B5 + Z24S8
# swizzled is a DATA_ERROR on the console, every copy, the format dropped
# (removed). r8d came up black in the AC97 init after a chained relaunch.
# round 9: the format switched back to the screen's right after the copy,
# before the retarget's DMA switches (MX_COPY_FIX 16), and sent again after
# the wait for idle (8): 29 = 5 + 8 + 16. Lockstep shots against 5 first
# (must match byte for byte), then 3 x 12 minutes of stress 40 at 720p; a
# stall reboots to the dashboard (launch the next run by hand).
CHAINS[9] = [
    ('Melee-X-r9a', 'final', 'fodshot', {'MX_VIDEO': '720', 'MX_COPY_FIX': '5'}),
    ('Melee-X-r9b', 'final', 'fodshot', {'MX_VIDEO': '720', 'MX_COPY_FIX': '29'}),
    ('Melee-X-r9c', 'final', 'stall12', {'MX_VIDEO': '720', 'MX_COPY_FIX': '29', 'MX_COPY_STRESS': '40'}),
    ('Melee-X-r9d', 'final', 'stall12', {'MX_VIDEO': '720', 'MX_COPY_FIX': '29', 'MX_COPY_STRESS': '40'}),
    ('Melee-X-r9e', 'final', 'stall12', {'MX_VIDEO': '720', 'MX_COPY_FIX': '29', 'MX_COPY_STRESS': '40'}),
]
# round 10 (v4.1 after v57 stalled at 97 min of play): XGX_COPY_FIX 37 (720p
# copies into A8R8G8B8 + Z24S8: no zeta format switch), XGX_ONE_DMA (DMA
# object 3 only, physical offsets) and XGX_CTX_KEEP (a PGRAPH context switch
# to the loaded channel keeps the live state; [NV2A] PGRAPH context switches
# counts them), all on by default. Lockstep shots with ONE_DMA/CTX_KEEP off
# must match; stress 40 at 720p with the new stress modes (2 four targets,
# 4 the CPU waits for idle between copy and clear) and the defaults; 480;
# last v57's settings (29, no ONE_DMA/CTX_KEEP) under mode 6: the
# reproducer attempt (a stall there reboots to the dashboard).
CHAINS[10] = [
    ('Melee-X-r10a', 'final', 'fodshot', {'MX_VIDEO': '720'}),
    ('Melee-X-r10b', 'final', 'fodshot', {'MX_VIDEO': '720', 'MX_ONE_DMA': '0', 'MX_CTX_KEEP': '0'}),
    ('Melee-X-r10c', 'final', 'stall12', {'MX_VIDEO': '720', 'MX_COPY_STRESS': '40', 'MX_COPY_STRESS_MODE': '6'}),
    ('Melee-X-r10d', 'final', 'stall12', {'MX_VIDEO': '720', 'MX_COPY_STRESS': '40'}),
    ('Melee-X-r10e', 'final', 'stall12', {'MX_VIDEO': '480', 'MX_COPY_STRESS': '40', 'MX_COPY_STRESS_MODE': '6'}),
    ('Melee-X-r10f', 'final', 'stall12', {'MX_VIDEO': '720', 'MX_COPY_STRESS': '40', 'MX_COPY_STRESS_MODE': '6',
                                          'MX_COPY_FIX': '29', 'MX_ONE_DMA': '0', 'MX_CTX_KEEP': '0'}),
]
# round 10's result: v57's settings stalled under stress mode 6 at frame 1139
# (~1 min: the reproducer the other stresses weren't); the defaults ran 12
# min of it clean, 480 too; lockstep shots equal; no PGRAPH context switch
# after frame 1's first load on the console (XGX_CTX_KEEP never acts).
# round 11: the defaults under the reproducer 4 x 12 min (at v57's rate
# ~40 stalls expected), then v57's settings once more (must stall).
R11 = {'MX_VIDEO': '720', 'MX_COPY_STRESS': '40', 'MX_COPY_STRESS_MODE': '6'}
CHAINS[11] = [(f'Melee-X-r11{c}', 'final', 'stall12', dict(R11)) for c in 'abcd'] + [
    ('Melee-X-r11e', 'final', 'stall12', dict(R11, MX_COPY_FIX='29', MX_ONE_DMA='0', MX_CTX_KEEP='0')),
]
# r11a ran clean; r11b faulted (LIMIT_COLOR, not the stall's LIMIT_ZETA) at
# frame 7200, right after the test build's periodic PGRAPH read with the GPU
# busy; that read is gone. round 12: round 11 again without it.
CHAINS[12] = [(f'Melee-X-r12{c}', 'final', 'stall12', dict(R11)) for c in 'abcd'] + [
    ('Melee-X-r12e', 'final', 'stall12', dict(R11, MX_COPY_FIX='29', MX_ONE_DMA='0', MX_CTX_KEEP='0')),
]
# r12a, r12b clean; r12c (the defaults) stalled at frame 1073, LIMIT_ZETA on
# the Z clear with the zeta pitch register at 0xa00 (the colour's) although
# the pushbuffer had sent 0x14000a00 twice; between them only pb_fill's
# colour-only clear (0xF0). round 13: colour and Z in one clear
# (XGX_COPY_FIX bit 2): v57's settings + 2 first (they stalled in ~2 min),
# then v58 + 2 x 4, then v57's settings (must stall).
V57 = dict(MX_ONE_DMA='0', MX_CTX_KEEP='0')
CHAINS[13] = [('Melee-X-r13a', 'final', 'stall12', dict(R11, MX_COPY_FIX='31', **V57))] + [
    (f'Melee-X-r13{c}', 'final', 'stall12', dict(R11, MX_COPY_FIX='39')) for c in 'bcde'] + [
    ('Melee-X-r13f', 'final', 'stall12', dict(R11, MX_COPY_FIX='29', **V57)),
]
# round 13 not run. round 14 (after a second opinion): the zeta pitch probe
# (MX_COPY_STRESS_MODE 8: PGRAPH 0x40085c read with the GPU idle before the
# clears, after the colour clear and after the Z clear; a wrong value is
# counted, logged and sent again, so the runs don't stop at the first one).
# a v57's settings, b the defaults, c + 64 (the copy keeps the zeta pitch),
# d + 2 (one clear), e + 2 + 64.
R14 = dict(R11, MX_COPY_STRESS_MODE='14')
CHAINS[14] = [
    ('Melee-X-r14a', 'final', 'stall12', dict(R14, MX_COPY_FIX='29', **V57)),
    ('Melee-X-r14b', 'final', 'stall12', dict(R14)),
    ('Melee-X-r14c', 'final', 'stall12', dict(R14, MX_COPY_FIX='101')),
    ('Melee-X-r14d', 'final', 'stall12', dict(R14, MX_COPY_FIX='39')),
    ('Melee-X-r14e', 'final', 'stall12', dict(R14, MX_COPY_FIX='103')),
]
CHAIN = []   # main(): CHAINS[N]


def script(i):
    folder, build, scen, sw = CHAIN[i]
    lines = [f'# docs/fps-plan.md round {ROUND}, run {i + 1} of {len(CHAIN)}: {build}, {scen}, '
             + (' '.join(f'{k}={v}' for k, v in sw.items()) or 'no switches')]
    lines += {'fod': FOD, 'fodlong': FODLONG, 'fd': FD, 'fodshot': FODSHOT, 'menu': MENU,
              'stall': STALL, 'stall8': STALL8, 'stall12': STALL12, 'ps': PS, 'psshot': PSSHOT}[scen]
    lines += [f'env {k}={v}' for k, v in sw.items()]
    if i + 1 < len(CHAIN):
        lines.append(f'env MX_NEXT_XBE=F:\\Applications\\{CHAIN[i + 1][0]}\\default.xbe')
    elif folder != 'Melee-X':   # a chained build ends the round: back to the dashboard, FTP up
        lines.append('env MX_NEXT_XBE=dashboard')
    return '\n'.join(lines) + '\n'


def connect():
    f = ftplib.FTP(os.environ.get('MX_FTP_HOST', 'xbox'), timeout=30)
    f.login('xbox', 'xbox')
    return f


def stage(builds):
    builds = Path(builds)
    for d in (STAGE, LOGS):   # a new round: the last round's logs would read as this one's
        if d.exists():
            shutil.rmtree(d)
    for i, (folder, build, _, _) in enumerate(CHAIN):
        d = STAGE / folder
        d.mkdir(parents=True)
        shutil.copy2(builds / f'{build}.xbe', d / 'default.xbe')
        (d / 'autopad.txt').write_bytes(script(i).replace('\n', '\r\n').encode())
    print(f'staged {len(CHAIN)} folders in {STAGE}; the first to launch: {CHAIN[0][0]}')


def upload():
    f = connect()
    f.cwd(UDATA)   # the console's FTP server lists the current directory whatever path NLST is given
    for name in f.nlst():   # the logs of earlier boots would read as this round's (pull them first)
        name = name.rsplit('/', 1)[-1]
        if name.startswith('boot') and name.endswith('.log'):
            f.delete(f'{UDATA}/{name}')
    for name in f.nlst():   # the shots of earlier rounds would read as this one's
        name = name.rsplit('/', 1)[-1]
        if name.startswith('shot_') and name.endswith('.bmp'):
            f.delete(f'{UDATA}/{name}')
    for folder, _, _, _ in CHAIN:
        d = f'{APPS}/{folder}'
        try:
            f.mkd(d)
        except ftplib.error_perm:
            pass
        for name in ('default.xbe', 'autopad.txt'):
            data = (STAGE / folder / name).read_bytes()
            f.storbinary(f'STOR {d}/{name}', io.BytesIO(data))
            back = io.BytesIO()
            f.retrbinary(f'RETR {d}/{name}', back.write)
            if back.getvalue() != data:
                sys.exit(f'{d}/{name}: mismatch after upload')
        print(f'{folder}: ok')
    f.quit()


def run_name(text):
    """The chain folder a log belongs to, from its [BOOT] image line."""
    m = re.search(r'\[BOOT\] image \S+ (\S+)', text)
    if not m:
        return 'Melee-X'   # the baseline logs no path
    return m[1].rstrip('\\').split('\\')[-2]


def watch():
    LOGS.mkdir(parents=True, exist_ok=True)
    want = {c[0] for c in CHAIN}
    while True:
        try:
            f = connect()
            f.cwd(UDATA)   # each chained build's log is boot_<folder>.log, the last one boot.log
            names = [n.rsplit('/', 1)[-1] for n in f.nlst()]
            shots = LOGS / 'shots'   # autopad SHOTs: shot_<folder>_NN.bmp
            for name in [n for n in names if n.startswith('shot_') and n.endswith('.bmp')]:
                if not (shots / name).exists():
                    shots.mkdir(exist_ok=True)
                    with open(shots / name, 'wb') as o:
                        f.retrbinary(f'RETR {UDATA}/{name}', o.write)
                    print(f'{time.strftime("%H:%M:%S")} saved shots/{name}', flush=True)
            for name in [n for n in names if n.startswith('hang_') and n.endswith('.log')]:
                buf = io.BytesIO()   # a run that hung (xhw_autopad_after_hang): saved as is
                f.retrbinary(f'RETR {UDATA}/{name}', buf.write)
                out = LOGS / name
                if not out.exists() or out.stat().st_size != len(buf.getvalue()):
                    out.write_bytes(buf.getvalue())
                    print(f'{time.strftime("%H:%M:%S")} saved {name}', flush=True)
            for name in [n for n in names if n.startswith('boot') and n.endswith('.log')]:
                buf = io.BytesIO()
                try:
                    f.retrbinary(f'RETR {UDATA}/{name}', buf.write)
                except ftplib.error_perm:
                    continue
                text = buf.getvalue().decode('utf-8', 'replace')
                if ('[GAME] match ends' not in text and '[AUTOPAD] NEXT' not in text   # NEXT: a run without a match
                        and '[AUTOPAD] hang' not in text):   # a run that hung and chained on
                    continue
                run = run_name(text)
                out = LOGS / f'{run}.log'
                if run in want and (not out.exists() or out.stat().st_size < len(buf.getvalue())):
                    out.write_bytes(buf.getvalue())
                    print(f'{time.strftime("%H:%M:%S")} saved {out.name} ({len(buf.getvalue())} bytes)', flush=True)
            f.quit()
        except (OSError, EOFError, ftplib.Error) as e:
            print(f'{time.strftime("%H:%M:%S")} ftp: {e}', flush=True)
        done = {p.stem for p in LOGS.glob('*.log') if not p.name.startswith('hang_')}
        if want <= done:
            print('all runs saved', flush=True)
            return
        time.sleep(15)


PERF = re.compile(r'\[PERF\] (\d+) frames ([\d.]+) fps \| ms/frame (.*?) \| ([\d.]+) ticks per render \| audio (\d+)% '
                  r'\| (\d+) draws')


GPUW = re.compile(r'\[NV2A\] per 600 frames: GPU still busy at (\d+) frame starts, (\d+) us a frame waiting there '
                  r'\(done (\d+) us after the present on average\), flip (\d+) us a frame')


def gpu_waits(text):
    """C3: the match's [NV2A] wait lines (each covers 600 frames): frame
    starts with the GPU still busy (of 600), ms a frame waiting there, ms a
    frame waiting for the flip."""
    rows, on = [], False
    for line in text.splitlines():
        if line.startswith('[SCENE] enter:') and 'scene 2 ' in line:
            on = True
        elif line.startswith('[GAME] match ends'):
            on = False
        elif on and (m := GPUW.search(line)):
            rows.append((int(m[1]), int(m[2]) / 1000, int(m[4]) / 1000))
    if not rows:
        return None
    n = len(rows)
    return sum(r[0] for r in rows) / n, sum(r[1] for r in rows) / n, sum(r[2] for r in rows) / n


def match_periods(text):
    """[PERF] periods of the match: after the versus scene's entry (its first
    period holds the loading), up to the match's end."""
    rows, on, first = [], False, False
    for line in text.splitlines():
        if line.startswith('[SCENE] enter:') and 'scene 2 ' in line:
            on, first = True, True
        elif line.startswith('[GAME] match ends'):
            on = False
        elif on and (m := PERF.search(line)):
            if first:
                first = False
                continue
            b = dict((k, float(v)) for k, v in re.findall(r'(\w+) ([\d.]+)', m[3]))
            rows.append((int(m[1]), float(m[2]), b, float(m[4]), int(m[6])))
    return rows


def report():
    print(f'{"run":12s} {"build":14s} {"scen":7s} {"switches":28s} {"n":>3s} {"fps":>6s} {"sim":>5s} {"rend":>5s} '
          f'{"dlist":>5s} {"draw":>5s} {"gpu":>5s} {"draws":>6s} {"tick/r":>6s} {"busy":>5s} {"wait":>5s} {"flip":>5s}')
    for folder, build, scen, sw in CHAIN:
        p = LOGS / f'{folder}.log'
        if not p.exists():
            print(f'{folder:12s} (no log)')
            continue
        text = p.read_text('utf-8', 'replace')
        rows = match_periods(text)
        gw = gpu_waits(text)
        if not rows:
            print(f'{folder:12s} (no match periods)')
            continue
        frames = sum(r[0] for r in rows)
        secs = sum(r[0] / r[1] for r in rows if r[1])
        ms = {k: sum(r[0] * r[2].get(k, 0) for r in rows) / frames for k in ('sim', 'render', 'dlist', 'draw', 'gpu')}
        draws = sum(r[0] * r[4] for r in rows) / frames
        ticks = sum(r[0] * r[3] for r in rows) / frames
        swn = ','.join(k[3:].lower() + ('' if v == '1' else f'={v}') for k, v in sw.items()) or '-'
        print(f'{folder:12s} {build:14s} {scen:7s} {swn:28s} {len(rows):3d} {frames / secs:6.2f} {ms["sim"]:5.2f} '
              f'{ms["render"]:5.2f} {ms["dlist"]:5.2f} {ms["draw"]:5.2f} {ms["gpu"]:5.2f} {draws:6.0f} {ticks:6.2f}'
              + (f' {gw[0]:5.0f} {gw[1]:5.2f} {gw[2]:5.2f}' if gw else ''))
    print('busy: frame starts (of 600) with the GPU still on the last frame; wait: ms a frame waiting there; '
          'flip: ms a frame waiting for the flip (C3)')


def shots():
    """The runs' screenshots by index (shot_<folder>_NN.bmp), runs with the
    same bytes grouped: lockstep runs that must show the same picture."""
    by = collections.defaultdict(lambda: collections.defaultdict(list))
    for p in sorted((LOGS / 'shots').glob('shot_*.bmp')):
        m = re.match(r'shot_(.+)_(\d+)\.bmp$', p.name)
        if m:
            by[(scen_of(m[1]), int(m[2]))][hashlib.md5(p.read_bytes()).hexdigest()[:10]].append(m[1][-3:])
    for (scen, n), groups in sorted(by.items()):
        print(f'{scen:8s} shot {n:02d}: ' + ' | '.join(f'{h} {",".join(r)}' for h, r in groups.items()))


def scen_of(folder):
    """A run's scenario and video switch: only those runs' shots compare."""
    for f, _, scen, sw in CHAIN:
        if f == folder:
            return scen + ('-' + sw['MX_VIDEO'] if 'MX_VIDEO' in sw else '')
    return '?'


def clean():
    f = connect()
    for folder, _, _, _ in CHAIN:
        d = f'{APPS}/{folder}'
        for name in ('autopad.txt',) if folder == 'Melee-X' else ('default.xbe', 'autopad.txt'):
            try:
                f.delete(f'{d}/{name}')
            except ftplib.error_perm:
                pass
        if folder != 'Melee-X':
            try:
                f.rmd(d)
            except ftplib.error_perm:
                pass
    f.quit()
    print(f'round {ROUND} scripts and folders removed (Melee-X keeps the baseline XBE)')


def main():
    global ROUND, STAGE, LOGS
    if len(sys.argv) < 3 or not sys.argv[1].isdigit() or int(sys.argv[1]) not in CHAINS:
        sys.exit(__doc__)
    ROUND = int(sys.argv[1])
    CHAIN[:] = CHAINS[ROUND]
    STAGE, LOGS = HW / f'stage-r{ROUND}', HW / f'logs-r{ROUND}'
    cmd = sys.argv[2]
    if cmd == 'stage' and len(sys.argv) == 4:
        stage(sys.argv[3])
    elif cmd in ('upload', 'watch', 'report', 'shots', 'clean'):
        globals()[cmd]()
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
