#!/usr/bin/env python3
"""Fold the [PROF] lines of a log (xhw_prof.c, -DXHW_PROF=1) into functions.

    tools/xbox/prof_report.py boot.log [--map build-xbox/melee_x.map] [--last]
    tools/xbox/prof_report.py --full serial.log|prof.bin [--map ...] [--csv out.csv]

Each [PROF] entry is a 64-byte bucket address and a sample count. Most
functions don't start on a bucket boundary (three in four) and half are
shorter than a bucket, so a bucket is shared out among the functions it
overlaps: by the bytes each has in it, weighted by each function's sample
density over all buckets (estimated iteratively). Crediting the whole bucket
to the function containing its start put pc_atanf's samples on the end of
pc_load_disc_fonts and memcpy's on xhw_splash_release; --first does that
again. The functions are listed by share of the samples placed in the
image. --last uses only the final report instead of summing all of them.

Newer builds also log callers (return addresses, one frame up): [PROFL] for
samples inside memcpy/memset/memcmp/memmove, [PROFC] for every sample. Those
are folded into the calling functions and listed after the functions.
[PROFS] buckets (v33 on) count only the samples taken during the simulation
ticks; they are listed last, as the simulation's own profile.

--full reads the whole-match profile instead: the [PROFH] lines an autopad
build logs at a match's end, or the prof.bin a profiler build writes to
E:\\UDATA\\4d580001 (console.py pull fetches it). Every bucket of the match,
all samples and the simulation's: the whole table with cumulative shares,
summed over the matches in the log (--last: the last one), and --csv writes
it as a spreadsheet."""
import argparse, bisect, collections, csv, pathlib, re, struct, sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import sym  # noqa: E402

BUCKET = 64


class Funcs:
    """The map's symbols as address ranges: each runs to the next symbol."""

    def __init__(self, path):
        syms, seen = [], set()
        for va, name, obj in sym.load(path):
            if va not in seen:   # aliases: the first name
                seen.add(va)
                syms.append((va, name, obj))
        self.syms = syms
        self.keys = [s[0] for s in syms]

    def name(self, i):
        return f'{self.syms[i][1]} ({self.syms[i][2]})' if i >= 0 else '?'

    def at(self, addr):
        return bisect.bisect_right(self.keys, addr) - 1

    def end(self, i):
        return self.keys[i + 1] if i + 1 < len(self.keys) else self.keys[i] + BUCKET

    def fold(self, buckets, first=False):
        """{bucket address: samples} -> Counter of function names"""
        out = collections.Counter()
        shared = []   # (samples, [(function, bytes in the bucket)])
        for addr, n in buckets.items():
            i = self.at(addr)
            parts = []
            while i < len(self.keys) and (i < 0 or self.keys[i] < addr + BUCKET):
                lo = max(addr, self.keys[i]) if i >= 0 else addr
                hi = min(addr + BUCKET, self.end(i)) if i >= 0 else min(addr + BUCKET, self.keys[0])
                if hi > lo:
                    parts.append((i, hi - lo))
                i += 1
            if first or len(parts) <= 1:
                out[parts[0][0] if parts else -1] += n
            else:
                shared.append((n, parts))
        if shared:
            # densities (samples per byte) from the whole profile; a few rounds
            # of: share each bucket by bytes x density, recompute the densities
            direct = collections.Counter(out)
            dens = collections.defaultdict(lambda: 1.0)
            for _ in range(40):
                got = collections.Counter(direct)
                for n, parts in shared:
                    w = [dens[f] * b for f, b in parts]
                    tot = sum(w) or 0
                    for (f, b), x in zip(parts, w):
                        got[f] += n * (x / tot if tot else b / BUCKET)
                dens = collections.defaultdict(float)
                for f, n in got.items():
                    if f >= 0:
                        dens[f] = n / max(1, self.end(f) - self.keys[f])
            out = got
        named = collections.Counter()
        for f, n in out.items():
            named[self.name(f)] += n
        return named

    def fold_exact(self, addrs, back=1):
        """{return address: samples} -> Counter of function names (the call
        instruction ends at the address: look up the byte before it)"""
        out = collections.Counter()
        for addr, n in addrs.items():
            out[self.name(self.at(addr - back))] += n
        return out


def table(counts, denom, n, cumulative=False):
    run = 0
    for name, c in counts.most_common(n):
        run += c
        cum = f'  {100.0 * run / denom:5.1f}%' if cumulative else ''
        print(f'{100.0 * c / denom:5.1f}%{cum}  {round(c):6d}  {name}')


# ---- whole match: [PROFH] lines or prof.bin ----

def read_profh(path):
    """the matches in a log's [PROFH] lines: [{'base', 'shift', 'all', 'sim', counters}]"""
    matches, cur = [], None
    for line in pathlib.Path(path).read_text(errors='replace').splitlines():
        k = line.find('[PROFH] ')
        if k < 0:
            continue
        body = line[k + 8:].strip()
        m = re.match(r'begin match (\d+): base ([0-9a-f]+) shift (\d+) buckets ([0-9a-f]+), (\d+) ms, (\d+) samples: '
                     r'(\d+) in image, (\d+) outside, (\d+) while waiting, (\d+) unreadable, (\d+) in the simulation',
                     body)
        if m:
            cur = {'match': int(m[1]), 'base': int(m[2], 16), 'shift': int(m[3]), 'nbuckets': int(m[4], 16),
                   'ms': int(m[5]), 'total': int(m[6]), 'placed': int(m[7]), 'outside': int(m[8]),
                   'waiting': int(m[9]), 'noframe': int(m[10]), 'sim_placed': int(m[11]),
                   'all': {}, 'sim': {}, 'complete': False}
            continue
        if cur is None:
            continue
        m = re.match(r'end match (\d+): all (\d+) sim (\d+)', body)
        if m:
            sums = (sum(cur['all'].values()), sum(cur['sim'].values()))
            if sums != (int(m[2]), int(m[3])):
                print(f'warning: match {m[1]}: [PROFH] lines sum to all {sums[0]} sim {sums[1]}, '
                      f'the end line says {m[2]} {m[3]} (lines lost?)', file=sys.stderr)
            cur['complete'] = True
            matches.append(cur)
            cur = None
            continue
        m = re.match(r'(all|sim) ([0-9a-f]+) (\S+)$', body)
        if not m:
            continue
        hist, b = cur[m[1]], int(m[2], 16)
        for tok in m[3].split(','):
            if tok.startswith('+'):
                b += int(tok[1:], 16)
            else:
                hist[b] = hist.get(b, 0) + int(tok, 16)
                b += 1
    if cur is not None:
        print(f'warning: match {cur["match"]} has no [PROFH] end line (log cut short?)', file=sys.stderr)
        matches.append(cur)
    return matches


PROFBIN = struct.Struct('<12I')   # xhw_prof.c ProfBinHeader


def read_profbin(path):
    data = pathlib.Path(path).read_bytes()
    h = PROFBIN.unpack_from(data)
    magic, version, base, shift, nb, match, ms, placed, outside, waiting, noframe, sim_placed = h
    if magic != 0x4850584D or version != 1:
        sys.exit(f'{path}: not a prof.bin (magic {magic:08x} version {version})')
    if len(data) < PROFBIN.size + nb * 8:
        sys.exit(f'{path}: {len(data)} bytes, short of {nb} buckets')
    arrs = struct.unpack_from(f'<{2 * nb}I', data, PROFBIN.size)
    return [{'match': match, 'base': base, 'shift': shift, 'nbuckets': nb, 'ms': ms, 'total': placed + outside,
             'placed': placed, 'outside': outside, 'waiting': waiting, 'noframe': noframe,
             'sim_placed': sim_placed, 'complete': True,
             'all': {i: c for i, c in enumerate(arrs[:nb]) if c},
             'sim': {i: c for i, c in enumerate(arrs[nb:]) if c}}]


def full(a, funcs):
    raw = pathlib.Path(a.log).read_bytes()[:4]
    matches = read_profbin(a.log) if raw == b'MXPH' else read_profh(a.log)
    if not matches:
        sys.exit('no [PROFH] profile in ' + a.log)
    use = matches[-1:] if a.last else matches
    hists = {'all': collections.Counter(), 'sim': collections.Counter()}
    for m in use:
        if m['shift'] != 6:
            print(f'warning: bucket shift {m["shift"]}, not 6', file=sys.stderr)
        for key in hists:
            for b, c in m[key].items():
                hists[key][m['base'] + (b << m['shift'])] += c
    placed = sum(m['placed'] for m in use)
    sim_placed = sum(m['sim_placed'] for m in use)
    ms = sum(m['ms'] for m in use)
    print(f'{len(use)} match(es), {ms / 1000:.1f} s: {sum(m["total"] for m in use)} samples, {placed} in the image, '
          f'{sum(m["outside"] for m in use)} outside, {sum(m["waiting"] for m in use)} while waiting, '
          f'{sum(m["noframe"] for m in use)} unreadable; {sim_placed} in the simulation')
    by = {key: funcs.fold(h, a.first) for key, h in hists.items()}
    print(f'\nall samples, by function ({len(by["all"])} functions)')
    print('  self    cum  samples  function')
    table(by['all'], max(1, placed), a.n, True)
    print(f'\nsimulation ticks only, by function ({len(by["sim"])} functions; % of the simulation\'s samples)')
    print('  self    cum  samples  function')
    table(by['sim'], max(1, sim_placed), a.n, True)
    if a.csv:
        with open(a.csv, 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['function', 'object', 'samples', 'percent', 'sim samples', 'sim percent'])
            for name, c in by['all'].most_common():
                fn, obj = name.rsplit(' (', 1) if ' (' in name else (name, ')')
                s = by['sim'].get(name, 0)
                w.writerow([fn, obj[:-1], f'{c:.1f}', f'{100.0 * c / max(1, placed):.3f}', f'{s:.1f}',
                            f'{100.0 * s / max(1, sim_placed):.3f}'])
        print(f'\nwrote {a.csv}')


# ---- the periodic [PROF] reports ----

def periodic(a, funcs):
    reports, cur = [], None
    for line in pathlib.Path(a.log).read_text(errors='replace').splitlines():
        m = re.search(r'\[PROF\] (\d+) samples: (\d+) in image', line)
        if m:
            cur = {'total': int(m[1]), 'placed': int(m[2]), 'buckets': collections.Counter(),
                   'libc': collections.Counter(), 'callers': collections.Counter(), 'libc_n': 0, 'callers_n': 0,
                   'sim': collections.Counter(), 'sim_n': 0}
            reports.append(cur)
            continue
        if cur is None:
            continue
        m = re.search(r'\[PROF([LCS])\] (\d+) samples', line)
        if m:
            cur[{'L': 'libc_n', 'C': 'callers_n', 'S': 'sim_n'}[m[1]]] += int(m[2])
            continue
        for tag, key in (('[PROFL]', 'libc'), ('[PROFC]', 'callers'), ('[PROFS]', 'sim'), ('[PROF]', 'buckets')):
            if tag in line:
                for addr, n in re.findall(r'([0-9a-f]{8}):(\d+)', line):
                    cur[key][int(addr, 16)] += int(n)
                break
    if not reports:
        sys.exit('no [PROF] reports in ' + a.log)
    use = reports[-1:] if a.last else reports
    total = sum(r['total'] for r in use)
    placed = sum(r['placed'] for r in use)
    buckets = collections.Counter()
    for r in use:
        buckets.update(r['buckets'])
    funcs_n = funcs.fold(buckets, a.first)
    listed = sum(funcs_n.values())
    print(f'{len(use)} report(s): {total} samples, {placed} in the image, {round(listed)} in the listed buckets')
    table(funcs_n, placed, a.n)

    def callers(key, title):
        n_all = sum(r[key + '_n'] for r in use)
        if not n_all:
            return
        hist = collections.Counter()
        for r in use:
            hist.update(r[key])
        by = funcs.fold(hist, a.first) if key == 'sim' else funcs.fold_exact(hist)
        print(f'\n{title}: {n_all} samples ({100.0 * n_all / placed:.1f}% of the image\'s)')
        table(by, n_all, a.n)

    callers('libc', 'memcpy/memset/memcmp/memmove, by calling function')
    callers('sim', 'simulation ticks only, by function')
    callers('callers', 'all samples, by calling function (one frame up)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log')
    ap.add_argument('--map', default=str(pathlib.Path(__file__).resolve().parents[2] / 'build-xbox/melee_x.map'))
    ap.add_argument('--last', action='store_true')
    ap.add_argument('--full', action='store_true', help='the whole-match profile ([PROFH] lines or prof.bin)')
    ap.add_argument('--csv', help='--full: also write the table to this CSV file')
    ap.add_argument('--first', action='store_true', help='credit a whole bucket to the function at its start')
    ap.add_argument('-n', type=int, help='functions to list (default 40; --full: all)')
    a = ap.parse_args()
    if a.n is None and not a.full:
        a.n = 40
    funcs = Funcs(a.map)
    full(a, funcs) if a.full else periodic(a, funcs)


if __name__ == '__main__':
    main()
