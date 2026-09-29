#!/usr/bin/env python3
"""Sampling profiler for a build running in xemu.

Start xemu with a QEMU monitor socket (tools/xbox/xemu_run.sh does, with
MX_XEMU_ARGS="-monitor unix:/tmp/mxmon.sock,server,nowait"), then

    tools/xbox/xemu_prof.py [--sock /tmp/mxmon.sock] [--samples 400] [--hz 20]

Each sample reads EIP through `info registers` and, to attribute time spent
in library code to its caller, the stack words that point into the XBE
(`x/64wx ESP`). Prints the flat profile (by the function holding EIP) and the
inclusive one (functions found on the stack), both symbolized with
build-xbox/melee_x.map. Kernel addresses (0x80000000 and up) are "kernel",
which includes the idle loop. xemu's timing is not the hardware's: use this
to find what dominates, not for absolute numbers."""
import argparse
import bisect
import collections
import os
import re
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sym  # noqa: E402


def monitor(sock_path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    s.settimeout(2)
    buf = b''
    while b'(qemu)' not in buf:
        buf += s.recv(4096)
    return s


def command(s, cmd):
    s.sendall(cmd.encode() + b'\n')
    buf = b''
    while not buf.rstrip().endswith(b'(qemu)'):
        buf += s.recv(65536)
    return buf.decode(errors='replace')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sock', default='/tmp/mxmon.sock')
    ap.add_argument('--samples', type=int, default=400)
    ap.add_argument('--hz', type=float, default=20)
    ap.add_argument('--map', default=os.path.join(sym.ROOT, 'build-xbox', 'melee_x.map'))
    ap.add_argument('--top', type=int, default=30)
    a = ap.parse_args()
    syms = sym.load(a.map)
    keys = [s[0] for s in syms]
    lo, hi = keys[0], keys[-1] + 0x10000

    def name(addr):
        if addr >= 0x80000000:
            return 'kernel'
        i = bisect.bisect_right(keys, addr) - 1
        return f'{syms[i][1]} ({syms[i][2]})' if i >= 0 else '?'

    s = monitor(a.sock)
    flat, incl = collections.Counter(), collections.Counter()
    n = 0
    for _ in range(a.samples):
        regs = command(s, 'info registers')
        m = re.search(r'EIP=([0-9a-f]{8})', regs)
        e = re.search(r'ESP=([0-9a-f]{8})', regs)
        if not m:
            continue
        eip = int(m.group(1), 16)
        flat[name(eip)] += 1
        seen = {name(eip)}
        if e and eip < 0x80000000:
            words = re.findall(r'0x([0-9a-f]{8})', command(s, f'x/64wx 0x{e.group(1)}'))
            for w in words[1::1]:
                v = int(w, 16)
                if lo <= v < hi:
                    seen.add(name(v))
        for k in seen:
            incl[k] += 1
        n += 1
        time.sleep(1 / a.hz)
    print(f'{n} samples')
    print('\n-- flat (EIP) --')
    for k, c in flat.most_common(a.top):
        print(f'{100 * c / n:5.1f}%  {k}')
    print('\n-- inclusive (EIP or on the stack; stack words are a heuristic) --')
    for k, c in incl.most_common(a.top):
        print(f'{100 * c / n:5.1f}%  {k}')


if __name__ == '__main__':
    main()
