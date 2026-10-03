#!/usr/bin/env python3
"""Console round 1: the probe build's log, by ablation window and by
performance-counter pair (docs/fps-plan.md step 1).

  tools/xbox/probe_report.py boot.log

Rows are the [PERF] periods of the match (after the first [SIMH], before
`[GAME] match ends`), minus any that hold a screenshot or a profiler report,
each labelled with the [AB] window that ran it (windows switch on period
boundaries) and followed by its [PERFX] and [PMC] lines.

Part 1, by window: fps, ms per frame in each bucket, and the cost per draw
(render + dlist + draw) and per tick (sim), against the baseline windows
(0 and 7). Part 2, the baseline windows' counters per bucket: cycles and
instructions (pair 1) give instructions per cycle, the other pairs are shown
per frame and as a share of the bucket's cycles where they count cycles."""
import argparse
import collections
import re
import statistics

BUCKETS = ['sim', 'render', 'dlist', 'draw', 'tex', 'efb', 'gpu', 'vsync']
PERF = re.compile(r'\[PERF\] (\d+) frames ([\d.]+) fps \| ms/frame (.*?) \| ([\d.]+) ticks per render .*?\| (\d+) draws')
PERFX = re.compile(r'\[PERFX\] (\d+) frames (\d+) draws (\d+) verts (\d+) ticks (\d+) renders \| us (.*?) \| span')
PMC = re.compile(r'\[PMC\] (\d+) (\S+) (\S+) \| k (.*)')
AB = re.compile(r'\[AB\] (\d+) (.*)')
NOISE = ('[FBDUMP]', '[AUTOPAD] SHOT', '[PROF', '[DUMP')
CYCLE_EVENTS = {'IFU_MEM_STALL', 'DCU_MISS_OUTSTANDING', 'RESOURCE_STALLS', 'CYCLES_DIV_BUSY'}


def parse(path):
    rows, window, names, in_match, noisy, cur = [], 0, {}, False, False, None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\r\n')
            m = AB.search(line)
            if m:
                window = int(m.group(1))
                names[window] = m.group(2)
                continue
            if '[SIMH]' in line:
                if not in_match:
                    in_match, noisy = True, True
                continue
            if '[GAME] match ends' in line:
                in_match = False
                continue
            if line.startswith(NOISE):
                noisy = True
                continue
            m = PERF.search(line)
            if m:
                cur = None
                if in_match and not noisy:
                    ms = m.group(3).split()
                    cur = dict(window=window, frames=int(m.group(1)), fps=float(m.group(2)),
                               ms=dict(zip(ms[::2], map(float, ms[1::2]))), ticks_per=float(m.group(4)))
                    rows.append(cur)
                noisy = False
                continue
            m = PERFX.search(line)
            if m and cur is not None:
                f_, draws, verts, ticks, renders = map(int, m.groups()[:5])
                us = m.group(6).split()
                cur.update(draws=draws, ticks=ticks, us=dict(zip(us[::2], map(int, us[1::2]))))
                continue
            m = PMC.search(line)
            if m and cur is not None:
                vals = m.group(4).split()
                per = {vals[i]: (int(vals[i + 1]), int(vals[i + 2])) for i in range(0, len(vals), 3)}
                cur['pmc'] = (int(m.group(1)), m.group(2), m.group(3), per)
    return rows, names


def mean(xs):
    return statistics.fmean(xs) if xs else 0.0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('log')
    args = ap.parse_args()
    rows, names = parse(args.log)
    print(f'{args.log}: {len(rows)} [PERF] periods in the match')
    by_w = collections.defaultdict(list)
    for r in rows:
        by_w[r['window']].append(r)

    def per_draw_us(rs):
        d = sum(r.get('draws', 0) for r in rs)
        return sum(sum(r['us'].get(k, 0) for k in ('render', 'dlist', 'draw')) for r in rs if 'us' in r) / d if d else 0

    def per_tick_ms(rs):
        t = sum(r.get('ticks', 0) for r in rs)
        return sum(r['us'].get('sim', 0) for r in rs if 'us' in r) / 1000 / t if t else 0

    base = by_w.get(0, []) + by_w.get(7, [])
    b_draw, b_tick = per_draw_us(base), per_tick_ms(base)
    print(f"\n{'window':24}{'n':>3}{'fps':>7}" + ''.join(f'{b:>7}' for b in BUCKETS)
          + f"{'us/draw':>9}{'ms/tick':>9}{'d/frame':>9}")
    for w in sorted(by_w):
        rs = by_w[w]
        draws_f = mean([r['draws'] / r['frames'] for r in rs if 'draws' in r])
        line = f"{w} {names.get(w, '?')[:21]:22}{len(rs):3}{mean([r['fps'] for r in rs]):7.1f}"
        line += ''.join(f"{mean([r['ms'].get(b, 0) for r in rs]):7.2f}" for b in BUCKETS)
        pd, pt = per_draw_us(rs), per_tick_ms(rs)
        line += f'{pd:9.2f}{pt:9.2f}{draws_f:9.0f}'
        if w not in (0, 7) and b_draw:
            line += f'   draw {100 * (pd / b_draw - 1):+.1f}%, tick {100 * (pt / b_tick - 1) if b_tick else 0:+.1f}%'
        print(line)

    # counters: baseline windows only (and every row if the windows did not rotate)
    src = base if base else rows
    pairs = collections.defaultdict(list)
    for r in src:
        if 'pmc' in r:
            pairs[r['pmc'][0]].append(r)
    if not pairs:
        print('\nno [PMC] lines (xemu, or not a -DXHW_PMC=1 build)')
        return
    # events per frame per bucket, averaged over the periods that counted them
    ev = {}
    for p, rs in sorted(pairs.items()):
        _, n0, n1, _ = rs[0]['pmc']
        frames = sum(r['frames'] for r in rs)
        for name, i in ((n0, 0), (n1, 1)):
            ev[name] = {b: 1000 * sum(r['pmc'][3].get(b, (0, 0))[i] for r in rs) / frames for b in BUCKETS}
    cyc = ev.get('CPU_CLK_UNHALTED', {})
    ins = ev.get('INST_RETIRED', {})
    print(f"\nper frame, baseline windows ({len(src)} periods); % = share of the bucket's cycles")
    print(f"{'event':26}" + ''.join(f'{b:>12}' for b in BUCKETS))
    for name, per in ev.items():
        line = f'{name[:25]:26}'
        for b in BUCKETS:
            v = per[b]
            if name in CYCLE_EVENTS and cyc.get(b):
                line += f'{v / 1e3:8.0f}k{100 * v / cyc[b]:3.0f}%'
            else:
                line += f'{v / 1e3:11.0f}k'
        print(line)
    if cyc and ins:
        print(f"{'IPC':26}" + ''.join(f'{ins[b] / cyc[b]:12.2f}' if cyc[b] else f'{"-":>12}' for b in BUCKETS))
        if 'ITLB_MISS' in ev:
            print(f"{'ITLB miss / 1k instr':26}"
                  + ''.join(f"{1000 * ev['ITLB_MISS'][b] / ins[b]:12.2f}" if ins[b] else f'{"-":>12}' for b in BUCKETS))
        if 'BR_MISS_PRED_RETIRED' in ev and 'BR_INST_RETIRED' in ev:
            print(f"{'mispredict %':26}" + ''.join(
                f"{100 * ev['BR_MISS_PRED_RETIRED'][b] / ev['BR_INST_RETIRED'][b]:12.1f}"
                if ev['BR_INST_RETIRED'][b] else f'{"-":>12}' for b in BUCKETS))


if __name__ == '__main__':
    main()
