#!/usr/bin/env python3
"""Draw census and display-list census from a -DXGX_CENSUS=1 build's log
(docs/fps-plan.md steps 0.3 and 0.4).

  tools/xbox/census_report.py run.log            # every block summed
  tools/xbox/census_report.py --block 2 run.log  # one 600-frame block only

[CENSUS] blocks (nv2a.c, every 600 presents): draws, vertices and the state
groups that changed before them, per pass (main, fighter shadow maps,
Fountain's reflection) and per owner (the p_link class of the GObj whose
render callback issued the draw). [DLCC] blocks (gx_vtx.c): cached
display-list bytes by owner, lists cached under more than one key, and the
lists rebuilt most."""
import argparse
import collections
import re

PASSES = {0: 'main', 1: 'shadow', 2: 'reflect', 3: 'pass3+'}
# p_link classes seen in a match (GObj_Create's second argument)
OWNERS = {1: 'scene', 2: 'scene2', 3: 'light/cam', 4: 'stage misc', 5: 'stage', 6: 'map', 7: 'item spawn',
          8: 'fighter', 9: 'item', 11: 'effect', 12: 'effect', 13: 'gm', 14: 'hud', 15: 'hud', 255: 'none'}
DIRTY = ['proj', 'view', 'posmtx', 'texmtx', 'lights', 'chans', 'texgen', 'tev', 'tevreg', 'pixel', 'fog', 'maps',
         'scissor']
HEAD = re.compile(r'\[CENSUS\] per (\d+) frames')
ROW = re.compile(r'\[CENSUS\] (\d+) (\d+) (\d+) (\d+) \| (\d+) (\d+) \|((?: \d+){13})')
DLCC = re.compile(r'\[DLCC\] (.*)')


def owner_name(o):
    return OWNERS.get(o, f'plink {o}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('log')
    ap.add_argument('--block', type=int, help='only this [CENSUS] block (0 = first)')
    args = ap.parse_args()
    slots = collections.defaultdict(lambda: [0] * 17)
    frames, block, dlcc = 0, -1, []
    with open(args.log, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\r\n')
            m = HEAD.search(line)
            if m:
                block += 1
                if args.block is None or block == args.block:
                    frames += int(m.group(1))
                continue
            m = ROW.search(line)
            if m and (args.block is None or block == args.block):
                p, o = int(m.group(1)), int(m.group(2))
                vals = [int(m.group(i)) for i in (3, 4, 5, 6)] + list(map(int, m.group(7).split()))
                acc = slots[(p, o)]
                for i, v in enumerate(vals):
                    acc[i] += v
                continue
            m = DLCC.search(line)
            if m and (args.block is None or block == args.block):
                dlcc.append(m.group(1))
    if not frames:
        print('no [CENSUS] blocks (a -DXGX_CENSUS=1 build?)')
        return
    total = sum(v[0] for v in slots.values())
    print(f'{block + 1} blocks, {frames} frames, {total / frames:.0f} draws a frame')
    print(f"{'pass':8}{'owner':12}{'draws/f':>9}{'%':>6}{'verts/f':>9}{'none%':>7}{'mtx%':>6}"
          + ''.join(f'{d + "%":>8}' for d in ('posmtx', 'maps', 'tev', 'chans', 'texgen', 'pixel')))
    by_pass = collections.Counter()
    for (p, o), v in sorted(slots.items(), key=lambda kv: (kv[0][0], -kv[1][0])):
        by_pass[p] += v[0]
        d = v[0] or 1
        pct = lambda name: 100 * v[4 + DIRTY.index(name)] / d
        print(f'{PASSES.get(p, p):8}{owner_name(o):12}{v[0] / frames:9.1f}{100 * v[0] / total:6.1f}{v[1] / frames:9.0f}'
              f'{100 * v[2] / d:7.0f}{100 * v[3] / d:6.0f}'
              + ''.join(f'{pct(n):8.0f}' for n in ('posmtx', 'maps', 'tev', 'chans', 'texgen', 'pixel')))
    print('by pass: ' + ', '.join(f'{PASSES.get(p, p)} {n / frames:.0f} draws/f ({100 * n / total:.0f}%)'
                                  for p, n in sorted(by_pass.items())))
    if dlcc:
        print()
        for line in dlcc:
            print(line)


if __name__ == '__main__':
    main()
