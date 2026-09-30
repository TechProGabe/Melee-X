#!/usr/bin/env python3
"""Fold the [PROF] lines of a log (xhw_prof.c, -DXHW_PROF=1) into functions.

    tools/xbox/prof_report.py boot.log [--map build-xbox/melee_x.map] [--last]

Each [PROF] entry is a 64-byte bucket address and a sample count. Buckets
are attributed to the function containing their start address (a bucket
straddling two functions goes to the first), and the functions are listed
by share of the samples placed in the image. --last uses only the final
report instead of summing all of them."""
import argparse, bisect, collections, pathlib, re, sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import sym  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument('log')
ap.add_argument('--map', default=str(pathlib.Path(__file__).resolve().parents[2] / 'build-xbox/melee_x.map'))
ap.add_argument('--last', action='store_true')
ap.add_argument('-n', type=int, default=40)
a = ap.parse_args()

reports, cur = [], None
for line in pathlib.Path(a.log).read_text(errors='replace').splitlines():
    m = re.search(r'\[PROF\] (\d+) samples: (\d+) in image', line)
    if m:
        cur = {'total': int(m[1]), 'placed': int(m[2]), 'buckets': collections.Counter()}
        reports.append(cur)
        continue
    if cur is not None and '[PROF]' in line:
        for addr, n in re.findall(r'([0-9a-f]{8}):(\d+)', line):
            cur['buckets'][int(addr, 16)] += int(n)
if not reports:
    sys.exit('no [PROF] reports in ' + a.log)
use = reports[-1:] if a.last else reports
total = sum(r['total'] for r in use)
placed = sum(r['placed'] for r in use)
syms = sym.load(a.map)
keys = [s[0] for s in syms]
funcs = collections.Counter()
for r in use:
    for addr, n in r['buckets'].items():
        i = bisect.bisect_right(keys, addr) - 1
        funcs[f'{syms[i][1]} ({syms[i][2]})' if i >= 0 else '?'] += n
listed = sum(funcs.values())
print(f'{len(use)} report(s): {total} samples, {placed} in the image, {listed} in the listed buckets')
for name, n in funcs.most_common(a.n):
    print(f'{100.0 * n / placed:5.1f}%  {n:6d}  {name}')
