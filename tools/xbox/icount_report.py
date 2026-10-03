#!/usr/bin/env python3
"""Instructions per draw and per tick from a test build's serial log, run in
xemu with -icount (docs/fps-plan.md step 0.1).

  tools/xbox/icount_report.py run.log                 # one run
  tools/xbox/icount_report.py base.log new.log        # before/after, and [SIMH] compared
  tools/xbox/icount_report.py --scale 250 run.log     # instructions per guest us (shift=2: 250)

Under `-icount shift=N,sleep=off` every guest instruction takes 2^N ns of
guest time, so a microsecond of [PERFX] bucket time is 1000 / 2^N
instructions (500 at shift=1; [CAL] at boot measures it). Rows are the
[PERFX] periods of the match: after the first [SIMH] line (fighters exist)
and before `[GAME] match ends`, minus any period holding a screenshot or a
profiler report (those stall the game thread). The audio mixer thread's
time is taken out of the buckets it preempted ([PERFX] lists it per bucket;
--with-audio keeps it). Per row: render, dlist and
draw instructions per draw, sim instructions per tick. The table shows the
median of the rows and the aggregate (the bucket's total over the total
count), which is the steadier of the two for comparing runs.

[SIMH] lines (simulation hashes every 60 ticks) of two logs are compared
tick by tick up to the match's end: equal hashes mean the same simulation.
After TIME!/GAME! the fighters' states depend on frame timing (two runs of
one build part there), so those ticks are left out."""
import argparse
import re
import statistics
import sys

PERFX = re.compile(r'\[PERFX\] (\d+) frames (\d+) draws (\d+) verts (\d+) ticks (\d+) renders \| us (.*?) \| span (\d+) us'
                   r'(?: audio \d+ us in((?: \d+)+))?')
BUCKETS = ['sim', 'render', 'dlist', 'draw', 'tex', 'efb', 'gpu', 'vsync']
SIMH = re.compile(r'\[SIMH\] tick (\d+): ([0-9a-f]{8})')
CAL = re.compile(r'\[CAL\] (\d+) instructions: (\d+) us')
NOISE = ('[FBDUMP]', '[AUTOPAD] SHOT', '[PROF', '[DUMP')


def parse(path, with_audio=False):
    rows, simh, cal, noisy, in_match, matches, ended = [], [], None, False, False, 0, False
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\r\n')
            m = CAL.search(line)
            if m:
                cal = int(m.group(1)) / max(1, int(m.group(2)))
            m = SIMH.search(line)
            if m:
                if not in_match and not ended:
                    in_match, noisy = True, True   # the period that holds the match's start
                    matches += 1
                if in_match:   # after TIME!/GAME! the ticks depend on frame timing
                    simh.append((matches, int(m.group(1)), m.group(2)))
                continue
            if '[GAME] match ends' in line:
                in_match, ended = False, True
                continue
            if '[SCENE] enter' in line:
                ended = False
                continue
            if line.startswith(NOISE):
                noisy = True
                continue
            m = PERFX.search(line)
            if not m:
                continue
            frames, draws, verts, ticks, renders = map(int, m.groups()[:5])
            us = dict(zip(m.group(6).split()[::2], map(int, m.group(6).split()[1::2])))
            if m.group(8) and not with_audio:   # the mixer's time inside each bucket, taken out
                for k, a in zip(BUCKETS, map(int, m.group(8).split())):
                    us[k] -= a
            if in_match and not noisy and draws and ticks:
                rows.append(dict(frames=frames, draws=draws, verts=verts, ticks=ticks, renders=renders, **us))
            noisy = False
    return rows, simh, cal


def table(rows, scale):
    """{metric: (median, aggregate)} in instructions"""
    out = {}
    for key, per in (('render', 'draws'), ('dlist', 'draws'), ('draw', 'draws'), ('sim', 'ticks')):
        vals = [r[key] * scale / r[per] for r in rows if r[per]]
        agg = sum(r[key] for r in rows) * scale / max(1, sum(r[per] for r in rows))
        out[f'{key}/{per[:-1]}'] = (statistics.median(vals) if vals else 0, agg)
    vals = [(r['render'] + r['dlist'] + r['draw']) * scale / r['draws'] for r in rows]
    agg = sum(r['render'] + r['dlist'] + r['draw'] for r in rows) * scale / max(1, sum(r['draws'] for r in rows))
    out['cpu/draw'] = (statistics.median(vals) if vals else 0, agg)
    out['draws/frame'] = (statistics.median([r['draws'] / r['frames'] for r in rows]) if rows else 0,
                          sum(r['draws'] for r in rows) / max(1, sum(r['frames'] for r in rows)))
    out['ticks/render'] = (statistics.median([r['ticks'] / r['renders'] for r in rows]) if rows else 0,
                           sum(r['ticks'] for r in rows) / max(1, sum(r['renders'] for r in rows)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--scale', type=float, default=500.0, help='instructions per guest microsecond (shift=1: 500)')
    ap.add_argument('--with-audio', action='store_true', help="keep the audio mixer's time in the buckets it preempted")
    args = ap.parse_args()
    runs = [(p, *parse(p, args.with_audio)) for p in args.logs]
    tabs = []
    for path, rows, simh, cal in runs:
        print(f'{path}: {len(rows)} rows in the match, {len(simh)} [SIMH]'
              + (f', [CAL] {cal:.1f} instructions/us' if cal else ''))
        if not rows:
            print('  no [PERFX] rows inside a match (a test build? -icount?)', file=sys.stderr)
        tabs.append(table(rows, args.scale))
    keys = list(tabs[0])
    head = f"{'':14}" + ''.join(f'{"median":>10}{"aggregate":>11}' for _ in tabs)
    if len(tabs) == 2:
        head += f'{"agg diff":>10}'
    print(head)
    for k in keys:
        line = f'{k:14}' + ''.join(f'{t[k][0]:10.0f}{t[k][1]:11.0f}' if t[k][1] > 50 else f'{t[k][0]:10.2f}{t[k][1]:11.2f}'
                                   for t in tabs)
        if len(tabs) == 2 and tabs[0][k][1]:
            line += f'{(tabs[1][k][1] / tabs[0][k][1] - 1) * 100:+9.1f}%'
        print(line)
    if len(runs) == 2:
        a = {(m, t): h for m, t, h in runs[0][2]}
        b = {(m, t): h for m, t, h in runs[1][2]}
        common = sorted(set(a) & set(b))
        diff = [k for k in common if a[k] != b[k]]
        print(f'[SIMH] {len(common)} ticks in both, {len(diff)} differ'
              + (f' (first: match {diff[0][0]} tick {diff[0][1]})' if diff else ''))
        if not common or diff:
            sys.exit(1)


if __name__ == '__main__':
    main()
