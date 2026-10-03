#!/usr/bin/env python3
"""A .profraw from an instrumented build's [PGOC] dump (docs/fps-plan.md A4).

  tools/xbox/pgo_raw.py --exe build-xbox/melee_x.exe --map build-xbox/melee_x.map run.log -o run.profraw
  llvm-profdata merge -o melee.profdata *.profraw

An XBOX_PGO=gen build has no compiler-rt: xbox/src/hw/xhw_pgo.c dumps only
the counter section (.lprfc) to the log, at every scene's exit and the
match's end. Everything else a raw profile holds is constant and is read
here from the linked image (melee_x.exe, the PE that cxbe turns into
default.xbe; the XBE keeps its addresses): the function records (.lprfd),
the compressed names (.lprfn), the MC/DC bitmaps (.lprfb, zero), and the
version word (__llvm_profile_raw_version). The last complete dump of the
log is used; its counters are cumulative since boot. Layout: LLVM's
InstrProfData.inc, raw version 10, 32-bit (llvm 21)."""
import argparse
import re
import struct
import sys

MAGIC_32 = (255 << 56 | ord('l') << 48 | ord('p') << 40 | ord('r') << 32 | ord('o') << 24 | ord('f') << 16
            | ord('R') << 8 | 129)
IPVK_LAST = 2   # IndirectCallTarget, MemOPSize, VTableTarget
DATA_SIZE = 48  # NameRef, FuncHash (8 each), CounterPtr, BitmapPtr, FunctionPointer, Values, NumCounters
#                 (4 each), NumValueSites[3] (u16), NumBitmapBytes (4), padded to 8


def pe_sections(path):
    data = open(path, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    nsec, = struct.unpack_from('<H', data, pe + 6)
    optsize, = struct.unpack_from('<H', data, pe + 20)
    base, = struct.unpack_from('<I', data, pe + 24 + 28)
    secs = {}
    for i in range(nsec):
        at = pe + 24 + optsize + 40 * i
        name = data[at:at + 8].rstrip(b'\0').decode()
        vsize, va, rsize, raw = struct.unpack_from('<IIII', data, at + 8)
        secs[name] = (base + va, vsize, data[raw:raw + min(vsize, rsize)].ljust(vsize, b'\0'))
    return secs


def read_va(secs, va, n):
    for start, size, blob in secs.values():
        if start <= va and va + n <= start + size:
            return blob[va - start:va - start + n]
    sys.exit(f'{va:08x}: not in the image')


def last_dump(log):
    dumps, cur = [], None
    for line in open(log, encoding='utf-8', errors='replace'):
        k = line.find('[PGOC] ')
        if k < 0:
            continue
        body = line[k + 7:].strip()
        m = re.match(r'begin (\d+) (.*): counters ([0-9a-f]+), (\d+)$', body)
        if m:
            cur = {'n': int(m[4]), 'addr': int(m[3], 16), 'why': m[2], 'c': {}}
            continue
        m = re.match(r'end (\d+): (\d+) nonzero', body)
        if m and cur:
            if len(cur['c']) == int(m[2]):
                dumps.append(cur)
            else:
                print(f'warning: dump {m[1]} has {len(cur["c"])} of {m[2]} counters (lines lost)', file=sys.stderr)
            cur = None
            continue
        if cur is None:
            continue
        toks = body.split()
        i = int(toks[0], 16)
        for t in toks[1:]:
            if t.startswith('+'):
                i += int(t[1:], 16)
            else:
                cur['c'][i] = int(t, 16)
                i += 1
    if not dumps:
        sys.exit(f'{log}: no complete [PGOC] dump')
    return dumps[-1]


def sym_va(mapfile, name):
    for line in open(mapfile, errors='replace'):
        p = line.split()
        if len(p) >= 3 and p[1] == name:
            return int(p[2], 16)
    sys.exit(f'{name} not in {mapfile}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('log')
    ap.add_argument('--exe', required=True, help='the linked PE of the instrumented build (build-xbox/melee_x.exe)')
    ap.add_argument('--map', required=True, help='its map')
    ap.add_argument('-o', '--out', required=True)
    a = ap.parse_args()
    secs = pe_sections(a.exe)
    for s in ('.lprfd', '.lprfn', '.lprfc'):
        if s not in secs:
            sys.exit(f'{a.exe}: no {s} section (not an XBOX_PGO=gen build?)')
    d_va, d_size, data = secs['.lprfd']
    c_va, c_size, _ = secs['.lprfc']
    n_va, n_size, names = secs['.lprfn']
    b_va, b_size, bitmap = secs.get('.lprfb', (c_va + c_size, 0, b''))
    dump = last_dump(a.log)
    # the XBE's section size is the PE's rounded up to a page: zeros past it
    if dump['addr'] != c_va or dump['n'] < c_size // 8 or any(i >= c_size // 8 for i in dump['c']):
        sys.exit(f'the dump ({dump["n"]} counters at {dump["addr"]:08x}) is not this image\'s '
                 f'({c_size // 8} at {c_va:08x})')
    counters = bytearray(c_size)
    for i, v in dump['c'].items():
        struct.pack_into('<Q', counters, 8 * i, v)
    version = struct.unpack('<Q', read_va(secs, sym_va(a.map, '___llvm_profile_raw_version'), 8))[0]
    if d_size % DATA_SIZE:
        sys.exit(f'.lprfd is {d_size} bytes, not a multiple of {DATA_SIZE}')
    pad = lambda n: (-n) % 8
    header = struct.pack('<16Q', MAGIC_32, version, 0, d_size // DATA_SIZE, 0, c_size // 8, pad(c_size),
                         b_size, pad(b_size), n_size, (c_va - d_va) & 0xFFFFFFFF, (b_va - d_va) & 0xFFFFFFFF, n_va, 0, 0, IPVK_LAST)
    out = (header + data + counters + b'\0' * pad(c_size) + bitmap + b'\0' * pad(b_size) + names
           + b'\0' * pad(n_size))
    open(a.out, 'wb').write(out)
    print(f'{a.out}: {d_size // DATA_SIZE} functions, {len(dump["c"])} of {c_size // 8} counters nonzero '
          f'(dump: {dump["why"]})')


if __name__ == '__main__':
    main()
