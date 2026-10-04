#!/usr/bin/env python3
"""Where do two runs of a match part? (docs/lan-plan.md phase 0A)

    tools/xbox/simh_diff.py a.log b.log [c.log ...] [--map melee_x.map] [--match N]

Reads each log's [SIMH] lines (every 60 ticks: the fighters' state and the
seed; every tick with env MX_SIMH_VERBOSE=1), and with env MX_RAND_TRACE=1
its [RAND] lines (every tick: the RNG draws, a rolling hash of every draw of
the match so far, and a sum over the tick's callers that leaves their order
out) and the [RANDD] lines of env MX_RAND_DUMP=<a>-<b> (each draw's caller).

Every log is compared with the first, and four firsts are printed:
  count   the first tick with another number of draws (the seed sequence
          depends only on the count, so from here every later draw differs)
  callers the first tick whose callers differ, whatever their order
  order   the first tick whose draws came in another order (the rolling
          hash): the same values went to other consumers. Harmless when the
          reordered consumers draw for themselves only (the particle system
          in xemu); a fighter's draw moving would show in [SIMH] later
  simh    the first [SIMH] line that differs
The runs part at the first of count, callers and simh; at that tick, when
both logs dumped it, the first caller that differs with its neighbours is
printed, symbolized with the build's map (the same build's: the addresses
are its own). Prints `equal through tick N` when all logs agree up to the
last tick they share (an order difference alone is noted, not a parting);
exits 1 when any two part.

A log holds one match by default; --match N picks the N-th (1-based) when a
log has several (the tick count starts over at each match)."""
import argparse
import bisect
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sym  # noqa: E402

SIMH = re.compile(r'\[SIMH\] tick (\d+): ([0-9a-f]{8})')
RAND = re.compile(r'\[RAND\] tick (\d+): (\d+) draws(?: \((\d+) in render, (\d+) off thread\))?, hash ([0-9a-f]{8})'
                  r'(?:, callers ([0-9a-f]{8}))?')
RANDD = re.compile(r'\[RANDD\] tick (\d+):(.*)$')


class Match:
    def __init__(self):
        self.simh = {}    # tick -> hash
        self.rand = {}    # tick -> (draws, in render, off thread, rolling hash, callers sum or None)
        self.dump = {}    # tick -> [(kind, addr)]
        self.more = {}    # tick -> draws past the dump's 1024

    def last(self):
        return max(list(self.simh) + list(self.rand) or [0])


def parse(path):
    """The log's matches, in order: a tick lower than the last one seen
    starts the next match."""
    matches, cur, last = [], None, 0
    with open(path, errors='replace') as f:
        for line in f:
            line = line.rstrip('\r\n')
            m = SIMH.search(line) or RAND.search(line) or RANDD.search(line)
            if not m:
                continue
            tick = int(m[1])
            if cur is None or tick < last:
                cur = Match()
                matches.append(cur)
            last = tick
            if m.re is SIMH:
                cur.simh[tick] = m[2]
            elif m.re is RAND:
                cur.rand[tick] = (int(m[2]), int(m[3] or 0), int(m[4] or 0), m[5], m[6])
            else:
                calls = cur.dump.setdefault(tick, [])
                for tok in m[2].split():
                    if tok.startswith('+'):
                        cur.more[tick] = int(tok[1:])
                    elif ':' in tok:
                        k, a = tok.split(':')
                        calls.append((k, int(a, 16)))
                    else:
                        calls.append(('', int(tok, 16)))
    return matches


def firsts(a, b):
    """The first tick of each kind of difference (None: none), and the last tick both have."""
    out = {'count': None, 'callers': None, 'order': None, 'simh': None}
    for t in sorted(set(a.rand) & set(b.rand)):
        ra, rb = a.rand[t], b.rand[t]
        if out['count'] is None and ra[:3] != rb[:3]:
            out['count'] = t
        if out['callers'] is None and ra[4] is not None and rb[4] is not None and ra[4] != rb[4]:
            out['callers'] = t
        if out['order'] is None and ra[3] != rb[3]:
            out['order'] = t
    for t in sorted(set(a.simh) & set(b.simh)):
        if a.simh[t] != b.simh[t]:
            out['simh'] = t
            break
    return out, min(a.last(), b.last())


def name_of(syms, keys, addr):
    if not syms:
        return ''
    # a return address: the function is the call's, just before it (a noreturn call can end one)
    i = bisect.bisect_right(keys, addr - 1) - 1
    if i < 0:
        return '  ?'
    va, name, obj = syms[i]
    return f'  {name}+0x{addr - va:x} ({obj})'


def show_calls(label, calls, more, syms, keys, start, end):
    for i in range(start, min(end, len(calls))):
        k, a = calls[i]
        print(f'    {label} #{i:<4d} {k or " ":1s} {a:08x}{name_of(syms, keys, a)}')
    if more:
        print(f'    {label} ... {more} draws past the dump')


def explain(names, a, b, tick, syms, keys):
    """The draws of the tick the runs part on, and its first differing caller."""
    na, nb = names
    if tick in a.rand and tick in b.rand:
        ra, rb = a.rand[tick], b.rand[tick]
        print(f'  [RAND] tick {tick}: {na} {ra[0]} draws ({ra[1]} in render, {ra[2]} off thread); '
              f'{nb} {rb[0]} draws ({rb[1]} in render, {rb[2]} off thread)')
    elif not a.rand or not b.rand:
        print('  no [RAND] lines: run with env MX_RAND_TRACE=1')
        return
    da, db = a.dump.get(tick), b.dump.get(tick)
    if da is None or db is None:
        print(f'  no callers for tick {tick} in ' + ' and '.join(n for n, d in ((na, da), (nb, db)) if d is None)
              + f': run with env MX_RAND_DUMP={max(1, tick - 30)}-{tick + 30}')
        return
    i = 0
    while i < len(da) and i < len(db) and da[i] == db[i]:
        i += 1
    if i == len(da) == len(db):
        print(f'  the tick\'s {len(da)} callers are the same, in the same order: the state parted without a '
              'draw (a timing input the RNG does not see); look at the ticks before it')
        return
    print(f'  callers agree for {i} draws; from draw {i}:')
    show_calls(na, da, a.more.get(tick), syms, keys, max(0, i - 3), i + 6)
    show_calls(nb, db, b.more.get(tick), syms, keys, max(0, i - 3), i + 6)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--map', help="the build's melee_x.map, to name the callers")
    ap.add_argument('--match', type=int, default=1, help='which match of each log (1-based)')
    a = ap.parse_args()
    if len(a.logs) < 2:
        sys.exit('two logs or more')
    syms = sym.load(a.map) if a.map else []
    keys = [s[0] for s in syms]
    runs = []
    for path in a.logs:
        ms = parse(path)
        if len(ms) < a.match:
            sys.exit(f'{path}: {len(ms)} match(es) with [SIMH]/[RAND] lines, no match {a.match}')
        runs.append((os.path.basename(path), ms[a.match - 1]))
    ref_name, ref = runs[0]
    parted, through = False, None
    for name, run in runs[1:]:
        f, common = firsts(ref, run)
        if run.last() != ref.last():
            print(f'{name}: ends at tick {run.last()}, {ref_name} at {ref.last()}')
        found = [f[k] for k in ('count', 'callers', 'simh') if f[k] is not None]
        print(f'{name} against {ref_name}: first difference in ' + ', '.join(
            f'{k} {"-" if f[k] is None else f[k]}' for k in ('count', 'callers', 'order', 'simh')))
        if not found:
            note = (f' (draws in another order from tick {f["order"]}: same counts and callers, the same '
                    'seed sequence)') if f['order'] is not None else ''
            print(f'{name}: equal to {ref_name} through tick {common}{note}')
            through = common if through is None else min(through, common)
            continue
        parted = True
        tick = min(found)
        print(f'{name}: parts from {ref_name} at tick {tick}')
        explain((ref_name, name), ref, run, tick, syms, keys)
    if len(runs) > 2:   # the outcomes: logs with the same [SIMH] lines grouped
        groups = {}
        for name, run in runs:
            groups.setdefault(tuple(sorted(run.simh.items())), []).append(name)
        print(f'{len(groups)} outcome(s): ' + ' | '.join(', '.join(g) for g in groups.values()))
    if parted:
        sys.exit(1)
    print(f'equal through tick {through}')


if __name__ == '__main__':
    main()
