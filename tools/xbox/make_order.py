#!/usr/bin/env python3
"""A link order for the code from whole-match profiles (docs/fps-plan.md A2).

  tools/xbox/make_order.py --map build-xbox/melee_x.map gl.log fodperf.log ... -o xbox/order.txt

Reads [PROFH] blocks (or prof.bin files) of -DXHW_PROF=1 runs, folds them into
functions with the map of the same build (prof_report.py's bucket sharing),
and writes lld-link's /order file: the functions the simulation ticks run,
hottest first, down to 99% of their samples; the same for the rest (the
render pass and the back end); every other function that ran, hottest
first; then every other function in the map's current order (so the cold
code keeps today's layout). With -ffunction-sections each function is its
own section and the linker lays .text out in this order. Each log weighs
the same, whatever its length. Names are the map's (decorated) symbols; a
static function name defined in several objects is listed once and places
all of them."""
import argparse
import collections
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import prof_report  # noqa: E402

STRING_FNS = ('_memcpy', '_memmove', '_memset', '_memcmp')   # xhw_string.c (map names)
TEXT = re.compile(r'^\s*0001:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s+(?:f\s+)?(?:i\s+)?(\S+)\s*$')


def text_functions(path):
    """the map's .text symbols in address order: [(va, name)]"""
    out = []
    with open(path, errors='replace') as f:
        for line in f:
            m = TEXT.match(line)
            if m:
                out.append((int(m.group(2), 16), m.group(1)))
    out.sort()
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('profiles', nargs='+', help='logs with [PROFH] lines, or prof.bin files')
    ap.add_argument('--map', required=True, help='the map of the build that took the profiles')
    ap.add_argument('-o', '--out', required=True)
    ap.add_argument('--share', type=float, default=0.99,
                    help='the simulation and render groups end at this share of their samples (default 0.99)')
    a = ap.parse_args()
    funcs = prof_report.Funcs(a.map)
    sim, total = collections.Counter(), collections.Counter()
    for p in a.profiles:
        raw = pathlib.Path(p).read_bytes()[:4]
        matches = prof_report.read_profbin(p) if raw == b'MXPH' else prof_report.read_profh(p)
        if not matches:
            sys.exit(f'{p}: no whole-match profile')
        hist = {'all': collections.Counter(), 'sim': collections.Counter()}
        for m in matches:
            for key in hist:
                for b, c in m[key].items():
                    hist[key][m['base'] + (b << m['shift'])] += c
        n = max(1, sum(hist['all'].values()))
        for key, acc in (('all', total), ('sim', sim)):
            for name, c in funcs.fold(hist[key]).items():
                acc[name.split(' (')[0]] += c / n   # each profile weighs the same
    order, seen = [], set()

    def add(name):
        if name in STRING_FNS:   # xhw_prof.c takes them as one range: keep them together
            for s in STRING_FNS:
                if s not in seen:
                    seen.add(s)
                    order.append(s)
        elif name not in seen and name != '?':
            seen.add(name)
            order.append(name)

    texts = text_functions(a.map)
    size = {name: (texts[i + 1][0] - va if i + 1 < len(texts) else 0) for i, (va, name) in enumerate(texts)}

    def hottest(fns, weight, share):
        """fns by weight, down to `share` of their total"""
        fns = sorted(fns, key=lambda f: -weight[f])
        tot, run, out = sum(weight[f] for f in fns), 0.0, []
        for f in fns:
            if run >= share * tot:
                break
            out.append(f)
            run += weight[f]
        return out

    render = collections.Counter({f: total[f] - sim[f] for f in total})
    groups = [('simulation', hottest([f for f in total if sim[f] >= 0.5 * total[f]], sim, a.share)),
              ('render', hottest([f for f in total if sim[f] < 0.5 * total[f]], render, a.share)),
              ('warm', [f for f, _ in total.most_common()])]
    report = []
    for title, fns in groups:
        before = len(order)
        for f in fns:
            add(f)
        report.append(f'{title} {len(order) - before} ({sum(size.get(f, 0) for f in order[before:]) // 1024} KB)')
    for _, name in texts:
        add(name)
    # C names as written: lld-link puts the i386 underscore back on (the
    # map's names have it). LF only: it would read a \r as part of each name.
    undecorate = lambda n: n[1:] if n.startswith('_') else n
    pathlib.Path(a.out).write_bytes(''.join(undecorate(n) + '\n' for n in order).encode())
    print(f'{a.out}: ' + ', '.join(report) + f', then the rest in map order ({len(a.profiles)} profiles)')


if __name__ == '__main__':
    main()
