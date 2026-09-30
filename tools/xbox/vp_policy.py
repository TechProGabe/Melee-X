#!/usr/bin/env python3
"""Replay vertex-program selects through program-memory residency policies.

The NV2A holds 136 vertex-program instructions. The back end (nv2a.c) keeps
programs resident there and loads one through the pushbuffer when a draw
needs a program that isn't (xbox/src/hw/nv2a_vpmem.c decides where). This
tool replays the programs a frame selects, in order, through several
policies and reports loads and instructions loaded per frame.

Input is either a console trace or a synthetic frame:
  vp_policy.py boot.log          [VPT] lines from a -DXGX_DEBUG_VPTRACE build
  vp_policy.py                   a synthetic 4-CPU Fountain of Dreams frame

Program sizes come from the generators themselves, built for the host with
ctypes: the one before the optimizer (tests/xbox/vp_ref.c, "old") and the
current one after vp_canon (xbox/src/hw/nv2a_vp.c, "new"). The "c" policy
is nv2a_vpmem.c, the code the console runs; the others are written here to
compare against it. "belady" knows the future (it evicts the programs needed
last) and is a reference, not something the console can do.

Each traced frame is replayed REPEAT times (default 3) and the last pass is
counted: the program memory as it would be after the same frame, which is
what consecutive frames of a match look like.
"""
import argparse
import bisect
import collections
import ctypes
import os
import random
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CAP = 136
KEY_BYTES = 44        # sizeof(VpKey)
PROG_BYTES = 4 + CAP * 16 + 4

# VpKey fields (nv2a_vp.h)
VPL_DIFFUSE, VPL_SPOT, VPL_SPEC = 1, 2, 3
VPF_LIN = 1


# ---------------------------------------------------------------------------
# generators, through ctypes
# ---------------------------------------------------------------------------
class Programs:
    def __init__(self, tmp):
        lib = os.path.join(tmp, "libvp.so")
        cc = os.environ.get("CC", "cc")
        subprocess.run([cc, "-O2", "-shared", "-fPIC", "-DVP_HOST_TEST", "-I" + os.path.join(ROOT, "xbox/src/hw"),
                        os.path.join(ROOT, "xbox/src/hw/nv2a_vp.c"),
                        os.path.join(ROOT, "xbox/src/hw/nv2a_vpmem.c"),
                        os.path.join(ROOT, "tests/xbox/vp_ref.c"), "-o", lib, "-lm"], check=True)
        self.lib = ctypes.CDLL(lib)
        self.cache = {}

    def canon(self, key):
        buf = ctypes.create_string_buffer(bytes(key), KEY_BYTES)
        self.lib.vp_canon(buf)
        return buf.raw[:KEY_BYTES]

    def size(self, key, old):
        """instructions of key's program: before the optimizer (old) or now"""
        ck = (bytes(key), old)
        if ck not in self.cache:
            prog = ctypes.create_string_buffer(PROG_BYTES)
            if old:
                self.lib.vp_ref_generate(ctypes.c_char_p(bytes(key)), prog)
            else:
                self.lib.vp_generate(ctypes.c_char_p(self.canon(key)), prog)
            self.cache[ck] = int.from_bytes(prog.raw[:4], "little")
        return self.cache[ck]


# ---------------------------------------------------------------------------
# traces: a list of frames, each a list of keys (bytes) in select order
# ---------------------------------------------------------------------------
def parse_log(path):
    """[VPT] lines (nv2a.c, -DXGX_DEBUG_VPTRACE) -> groups of consecutive
    frames, and what the console reported for them:
         [VPT] frame F: S selects, L loads (I instructions), K keys
         [VPT] k ID HASH N KEYHEX       one per program the frame selects
         [VPT] s ID[L] ID[L] ...        the selects in order; L: it loaded
         [VPT] end"""
    groups, console = [], []
    keys, seq, frame, last = {}, [], None, None
    for line in open(path, errors="replace"):
        m = re.search(r"\[VPT\] (.*)", line)
        if not m:
            continue
        f = m.group(1).split()
        if not f:
            continue
        if f[0] == "frame":
            frame = int(f[1].rstrip(":"))
            keys, seq = {}, []
            nums = [int(x) for x in re.findall(r"\d+", m.group(1))]
            console.append((nums[2], nums[3]))
        elif f[0] == "k" and frame is not None:
            keys[int(f[1])] = bytes.fromhex(f[4])
        elif f[0] == "s" and frame is not None:
            seq += [int(t.rstrip("L")) for t in f[1:]]
        elif f[0] == "end" and frame is not None:
            fr = [keys[i] for i in seq]
            if last is not None and frame == last + 1:
                groups[-1].append(fr)
            else:
                groups.append([fr])
            last, frame = frame, None
    return groups, console


def make_key(has_nrm=0, nchans=1, chans=(), tex=(), fog=0, copy=0, ang_one=0, dist_one=0):
    b = bytearray(KEY_BYTES)
    b[0], b[1] = has_nrm, nchans
    for i, c in enumerate(chans):
        b[2 + i * 6:8 + i * 6] = bytes(c)
    b[26] = len(tex)
    for i, t in enumerate(tex):
        b[27 + i * 3:30 + i * 3] = bytes(t)
    b[39], b[40], b[41], b[42] = copy, fog, ang_one, dist_one
    return bytes(b)


def ch(enable=0, amb=0, mat=0, diff=0, attn=VPL_DIFFUSE, mask=0):
    return (enable, amb, mat, diff, attn, mask)


def synthetic(nframes, diffuse=0x03, spec=0x10, seed=1, infinite=True):
    """A 4-CPU match on Fountain of Dreams as HSD draws it: shadow maps, the
    stage, four fighters, their ground shadows, particles and the HUD. The
    mix follows HSD's channel setup (state.c HSD_SetupChannelMode: COLOR0
    lit with spot attenuation over the stage's diffuse lights, ALPHA0 off,
    COLOR1 specular for shiny materials; COLOR1 keeps its last setting when
    only one channel is on), tobj.c's texgens (TEX0, environment maps from
    normals) and the stage fog. With the flush-all policy and the old
    generator it loads ~106 programs of ~68 instructions a frame, near what
    the console logged there (~80 of ~60); it stands in for a real trace."""
    rng = random.Random(seed)
    one = diffuse if infinite else 0    # infinite lights: attenuation (1, 0, 0)
    lit = ch(1, 0, 0, 2, VPL_SPOT, diffuse)
    alpha_off = ch(0, 0, 0, 0, VPL_DIFFUSE)
    spec_ch = ch(1, 0, 0, 2, VPL_SPEC, spec)
    tex0 = (4, 0, 0)
    env = (1, 1, 0)
    unlit_reg = ch(0, 0, 0, 0, VPL_DIFFUSE)
    unlit_vtx = ch(0, 0, 1, 0, VPL_DIFFUSE)
    col1 = [alpha_off, alpha_off]    # COLOR1 / ALPHA1 as last set

    def key(*a, **kw):
        return make_key(*a, ang_one=one, dist_one=one, **kw)

    def fighter(kind):
        if kind == "L":
            return key(1, 1, [lit, alpha_off] + col1, [tex0], VPF_LIN)
        if kind == "LN":
            return key(1, 1, [lit, alpha_off] + col1, [], VPF_LIN)
        if kind == "S":
            col1[:] = [spec_ch, alpha_off]
            return key(1, 2, [lit, alpha_off] + col1, [tex0], VPF_LIN)
        if kind == "E":
            col1[:] = [spec_ch, alpha_off]
            return key(1, 2, [lit, alpha_off] + col1, [env, tex0], VPF_LIN)
        return key(0, 1, [unlit_reg, unlit_reg] + col1, [tex0], VPF_LIN)   # "U": eyes, decals

    # per fighter: its DObjs' materials in draw order (fixed for a character)
    kinds = ["L", "L", "L", "S", "S", "E", "LN", "U"]
    fighters = [[rng.choice(kinds) for _ in range(rng.randint(14, 22))] for _ in range(4)]
    stage = [rng.choice(["SU", "SU", "SL", "SE", "SV"]) for _ in range(40)]

    def stage_key(kind):
        if kind == "SU":
            return key(0, 1, [unlit_reg, unlit_reg] + col1, [tex0], VPF_LIN)
        if kind == "SV":
            return key(0, 1, [unlit_vtx, unlit_vtx] + col1, [tex0], VPF_LIN)
        if kind == "SL":
            return key(1, 1, [lit, alpha_off] + col1, [tex0], VPF_LIN)
        return key(1, 1, [lit, alpha_off] + col1, [env, tex0], VPF_LIN)

    shadow_model = key(0, 1, [unlit_reg, unlit_reg] + col1, [], 0)
    copy = make_key(copy=1)
    ground_shadow = key(0, 1, [unlit_reg, unlit_reg] + col1, [(0, 1, 0)], VPF_LIN)
    particle = [key(0, 1, [unlit_vtx, unlit_vtx] + col1, [tex0], VPF_LIN),
                key(0, 1, [ch(1, 0, 0, 2, VPL_DIFFUSE, 0), unlit_reg] + col1, [tex0], VPF_LIN),
                key(0, 1, [unlit_vtx, unlit_vtx] + col1, [tex0], 0)]
    hud = [key(0, 1, [unlit_vtx, unlit_vtx] + col1, [tex0], 0), key(0, 1, [unlit_vtx, unlit_vtx] + col1, [], 0)]

    frames = []
    for _ in range(nframes):
        seq = []
        for f in fighters:                      # shadow maps: model, then the EFB copy
            seq += [shadow_model, copy]
        for s in stage:
            seq.append(stage_key(s))
        for rep in range(2):                    # opaque pass, then translucent parts
            for f in fighters:
                for m in f:
                    if rep == 0 or m in ("S", "E") or rng.random() < 0.2:
                        seq.append(fighter(m))
        for _ in range(4):
            seq.append(ground_shadow)
        for _ in range(rng.randint(30, 50)):    # effects
            seq.append(rng.choice(particle))
        for _ in range(14):
            seq.append(rng.choice(hud))
        # draws whose key didn't change don't select
        frames.append([k for i, k in enumerate(seq) if i == 0 or k != seq[i - 1]])
    return frames


# ---------------------------------------------------------------------------
# policies: select(pid, n) -> loaded?; frame_end() at each frame boundary
# ---------------------------------------------------------------------------
INF = 1 << 30


class Policy:
    def __init__(self):
        self.res = {}          # pid -> (start, n)
        self.last = {}         # pid -> select counter of its last use
        self.t = 0

    def select(self, pid, n):
        self.t += 1
        hit = pid in self.res
        if not hit:
            self.place(pid, n)
        self.last[pid] = self.t
        return not hit

    def frame_end(self):
        pass

    def gaps(self):
        spans = sorted(self.res.values())
        at, out = 0, []
        for s, n in spans:
            if s > at:
                out.append((at, s - at))
            at = max(at, s + n)
        if at < CAP:
            out.append((at, CAP - at))
        return out

    def windows(self, n):
        """candidate starts for a program of n: 0, CAP - n, and the starts and
        ends of the resident programs; each with the programs it overlaps"""
        starts = {0, CAP - n}
        for s, m in self.res.values():
            starts |= {s, s + m}
        for s in sorted(x for x in starts if x + n <= CAP):
            yield s, [p for p, (ps, pn) in self.res.items() if ps < s + n and s < ps + pn]

    def evict_place(self, pid, n, start, victims):
        for v in victims:
            del self.res[v]
        self.res[pid] = (start, n)


class Flush(Policy):
    """before: pack from slot 0, flush everything when the next doesn't fit"""
    name = "flush"

    def __init__(self):
        super().__init__()
        self.top = 0

    def place(self, pid, n):
        if self.top + n > CAP:
            self.res.clear()
            self.top = 0
        self.res[pid] = (self.top, n)
        self.top += n


class WindowPolicy(Policy):
    """best-fitting free gap, else the contiguous window of lowest cost"""

    def place(self, pid, n):
        fit = [g for g in self.gaps() if g[1] >= n]
        if fit:
            s, _ = min(fit, key=lambda g: (g[1], g[0]))
            self.res[pid] = (s, n)
            return
        s, victims = min(self.windows(n), key=lambda w: self.cost(w[1]))
        self.evict_place(pid, n, s, victims)

    def lru(self, victims):
        return max((self.last[v] for v in victims), default=0)

    def size(self, victims):
        return sum(self.res[v][1] for v in victims)


class Lru(WindowPolicy):
    """the window whose most recently used program is the least recent"""
    name = "lru"

    def cost(self, victims):
        return (self.lru(victims), self.size(victims))


class Prev(WindowPolicy):
    """Belady's rule with the previous frame as the forecast: frames repeat,
    so a program's next use is predicted from where the previous frame
    selected it after this point. The window whose soonest predicted use is
    the furthest goes (LRU among equals)."""
    name = "prev"

    def __init__(self):
        super().__init__()
        self.i = 0
        self.cur, self.prev, self.prev_len = collections.defaultdict(list), {}, 0

    def select(self, pid, n):
        r = super().select(pid, n)
        self.cur[pid].append(self.i)
        self.i += 1
        return r

    def frame_end(self):
        self.prev, self.prev_len = dict(self.cur), self.i
        self.cur, self.i = collections.defaultdict(list), 0

    def next_use(self, p):
        pos = self.prev.get(p)
        if not pos:
            return INF
        j = bisect.bisect_right(pos, self.i)
        return pos[j] - self.i if j < len(pos) else self.prev_len - self.i + pos[0]

    def cost(self, victims):
        return (-min((self.next_use(v) for v in victims), default=INF), self.lru(victims), self.size(victims))


class Belady(WindowPolicy):
    """offline reference: evict the window whose next use is furthest away"""
    name = "belady"

    def __init__(self, future):
        super().__init__()
        self.future = future      # per select index: {pid: next select index}
        self.pos = 0

    def select(self, pid, n):
        r = super().select(pid, n)
        self.pos += 1
        return r

    def cost(self, victims):
        nxt = self.future[self.pos]
        return (-min((nxt.get(v, INF) for v in victims), default=INF), self.size(victims))


class CPolicy(Policy):
    """xbox/src/hw/nv2a_vpmem.c, as the console runs it, behind nv2a.c's cache
    of VPM_PROGS generated programs (vp_select: the least recently used
    entry is reused, and vpm_drop frees its memory)"""
    name = "c"
    ENTRIES = 64

    def __init__(self, lib):
        super().__init__()
        self.lib = lib
        self.mem = ctypes.create_string_buffer(lib.vpm_size())
        lib.vpm_init(self.mem)
        self.entry = {}           # pid -> cache entry
        self.used = {}            # entry -> select counter

    def select(self, pid, n):
        self.t += 1
        e = self.entry.get(pid)
        if e is None:
            free = [i for i in range(self.ENTRIES) if i not in self.used]
            if free:
                e = free[0]
            else:
                e = min(self.used, key=self.used.get)
                self.lib.vpm_drop(self.mem, e)
                self.entry = {p: x for p, x in self.entry.items() if x != e}
            self.entry[pid] = e
        self.used[e] = self.t
        start = ctypes.c_int()
        return bool(self.lib.vpm_select(self.mem, e, n, ctypes.byref(start)))

    def frame_end(self):
        self.lib.vpm_frame(self.mem)


def future_table(seq):
    """for Belady: at each select, when each program is next selected"""
    nxt, table = {}, [None] * len(seq)
    for i in range(len(seq) - 1, -1, -1):
        table[i] = dict(nxt)
        nxt[seq[i]] = i
    return table


def replay(groups, progs, old, make):
    """-> [(loads, instructions, selects)] per counted frame. Each group is a
    run of consecutive frames; its first frame only warms the program
    memory up (a group of one frame is played twice)."""
    out = []
    for group in groups:
        frames = group if len(group) > 1 else group * 2
        ids, seqs, sizes = {}, [], {}
        for fr in frames:
            seq = []
            for k in fr:
                ck = k if old else progs.canon(k)
                p = ids.setdefault(ck, len(ids))
                sizes[p] = progs.size(k, old)
                seq.append(p)
            seqs.append(seq)
        pol = make(future_table([p for s in seqs for p in s]))
        for f, seq in enumerate(seqs):
            loads = ins = 0
            for p in seq:
                if pol.select(p, sizes[p]):
                    loads += 1
                    ins += sizes[p]
            pol.frame_end()
            if f:
                out.append((loads, ins, len(seq)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", help="boot.log or serial.log with [VPT] lines")
    ap.add_argument("--frames", type=int, default=20, help="synthetic frames (default 20)")
    ap.add_argument("--diffuse", type=lambda x: int(x, 0), default=0x03,
                    help="synthetic: the stage's diffuse light mask (default 0x03)")
    ap.add_argument("--spec", type=lambda x: int(x, 0), default=0x10,
                    help="synthetic: the specular light mask (default 0x10)")
    ap.add_argument("--point", action="store_true",
                    help="synthetic: the diffuse lights attenuate (not infinite lights)")
    ap.add_argument("--seed", type=int, default=1, help="synthetic: material and effect mix")
    ap.add_argument("--policies", default="flush,lru,prev,c,belady")
    ap.add_argument("--keys", action="store_true", help="list the programs of the first frame")
    ap.add_argument("--check", action="store_true",
                    help="CI: nv2a_vpmem.c must load exactly what the 'prev' model does, and fewer than flush and lru")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        progs = Programs(tmp)
        lib = progs.lib
        lib.vpm_size.restype = ctypes.c_int
        if args.log:
            groups, console = parse_log(args.log)
            if not groups:
                sys.exit(f"no [VPT] frames in {args.log}")
            nfr = sum(len(g) for g in groups)
            print(f"{nfr} traced frames in {len(groups)} runs from {args.log}; the console loaded "
                  f"{sum(c[0] for c in console) / len(console):.1f} programs "
                  f"({sum(c[1] for c in console) / len(console):.0f} instructions) per traced frame")
            if all(len(g) == 1 for g in groups):
                print("  (single frames: each is played twice, so 'prev' sees the same frame it forecasts from)")
        else:
            groups = [synthetic(args.frames, args.diffuse, args.spec, args.seed, not args.point)]
            print(f"{args.frames} synthetic frames (4-CPU Fountain of Dreams stand-in)")
        frames = [f for g in groups for f in g]
        print(f"{sum(len(f) for f in frames) / len(frames):.0f} selects per frame")
        if args.keys:
            cnt = collections.Counter(frames[0])
            print("  selects  old  new  key")
            for k, c in cnt.most_common():
                print(f"  {c:7d}  {progs.size(k, True):3d}  {progs.size(k, False):3d}  {k.hex()}")
        makers = {
            "flush": lambda fut: Flush(),
            "lru": lambda fut: Lru(),
            "prev": lambda fut: Prev(),
            "c": lambda fut: CPolicy(lib),
            "belady": lambda fut: Belady(fut),
        }
        if args.check:
            res = {n: replay(groups, progs, False, makers[n]) for n in ("flush", "lru", "prev", "c")}
            tot = {n: sum(x[0] for x in r) for n, r in res.items()}
            ok = res["c"] == res["prev"] and tot["c"] < tot["lru"] < tot["flush"]
            print(f"loads over {len(res['c'])} frames: flush {tot['flush']}, lru {tot['lru']}, "
                  f"prev {tot['prev']}, c {tot['c']}: {'ok' if ok else 'FAILED'}")
            sys.exit(0 if ok else 1)
        print(f"{'policy':8s} {'programs':>18s} {'loads/frame':>12s} {'instructions/frame':>19s} {'per load':>9s}")
        for old in (True, False):
            for name in args.policies.split(","):
                r = replay(groups, progs, old, makers[name])
                loads = sum(x[0] for x in r) / len(r)
                ins = sum(x[1] for x in r) / len(r)
                label = "before optimizer" if old else "optimized, canon"
                print(f"{name:8s} {label:>18s} {loads:12.1f} {ins:19.0f} {ins / loads if loads else 0:9.1f}")


if __name__ == "__main__":
    main()
