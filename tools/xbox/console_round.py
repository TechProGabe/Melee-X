#!/usr/bin/env python3
"""Console rounds (docs/fps-plan.md): build variants in folders of their own,
chained so one launch runs them all. N is the round (CHAINS below).

  tools/xbox/console_round.py N stage BUILDS   # BUILDS/<build>.xbe, the builds the chain names
  tools/xbox/console_round.py N upload         # FTP every folder (MX_FTP_HOST)
  tools/xbox/console_round.py N watch          # save each run's log (at the dashboard)
  tools/xbox/console_round.py N report         # fps and ms a frame per bucket, per run
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
import ftplib
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
FD = ['env MELEE_BOOT_SCENE=vs', 'env MELEE_DEBUG_VS_STAGE=32', 'env MELEE_DEBUG_VS_CHARS=2,8',
      'env MELEE_DEBUG_VS_TIME=60', 'env MELEE_SEED=1']
# folder, build, scenario, switches: in the order they run; the last one is
# the baseline in Melee-X itself (no chaining)
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
B2 = {'MX_MEM1_LARGE': '6'}
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
CHAIN = []   # main(): CHAINS[N]


def script(i):
    folder, build, scen, sw = CHAIN[i]
    lines = [f'# docs/fps-plan.md round {ROUND}, run {i + 1} of {len(CHAIN)}: {build}, {scen}, '
             + (' '.join(f'{k}={v}' for k, v in sw.items()) or 'no switches')]
    lines += {'fod': FOD, 'fodlong': FODLONG, 'fd': FD}[scen]
    lines += [f'env {k}={v}' for k, v in sw.items()]
    if i + 1 < len(CHAIN):
        lines.append(f'env MX_NEXT_XBE=F:\\Applications\\{CHAIN[i + 1][0]}\\default.xbe')
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
            for name in [n for n in names if n.startswith('boot') and n.endswith('.log')]:
                buf = io.BytesIO()
                try:
                    f.retrbinary(f'RETR {UDATA}/{name}', buf.write)
                except ftplib.error_perm:
                    continue
                text = buf.getvalue().decode('utf-8', 'replace')
                if '[GAME] end banner done' not in text:
                    continue
                run = run_name(text)
                out = LOGS / f'{run}.log'
                if run in want and (not out.exists() or out.stat().st_size < len(buf.getvalue())):
                    out.write_bytes(buf.getvalue())
                    print(f'{time.strftime("%H:%M:%S")} saved {out.name} ({len(buf.getvalue())} bytes)', flush=True)
            f.quit()
        except (OSError, EOFError, ftplib.Error) as e:
            print(f'{time.strftime("%H:%M:%S")} ftp: {e}', flush=True)
        done = {p.stem for p in LOGS.glob('*.log')}
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
        swn = ','.join(k[3:].lower() for k in sw) or '-'
        print(f'{folder:12s} {build:14s} {scen:7s} {swn:28s} {len(rows):3d} {frames / secs:6.2f} {ms["sim"]:5.2f} '
              f'{ms["render"]:5.2f} {ms["dlist"]:5.2f} {ms["draw"]:5.2f} {ms["gpu"]:5.2f} {draws:6.0f} {ticks:6.2f}'
              + (f' {gw[0]:5.0f} {gw[1]:5.2f} {gw[2]:5.2f}' if gw else ''))
    print('busy: frame starts (of 600) with the GPU still on the last frame; wait: ms a frame waiting there; '
          'flip: ms a frame waiting for the flip (C3)')


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
    elif cmd in ('upload', 'watch', 'report', 'clean'):
        globals()[cmd]()
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
