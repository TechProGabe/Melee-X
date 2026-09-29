#!/usr/bin/env python3
"""Decode [FBDUMP] frames from an xemu serial log into PNGs.

usage: fbdump_to_png.py serial.log [outprefix]   -> outprefix_000.png, ...
Pixels are X8R8G8B8 little-endian (B,G,R,X in memory) or R5G6B5 at bpp 16.
Pure stdlib (zlib + a minimal PNG writer)."""
import base64, struct, sys, zlib

def png(path, w, h, rows):
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + r for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))

def main():
    log = sys.argv[1]
    pre = sys.argv[2] if len(sys.argv) > 2 else "fbdump"
    n, cur, hdr = 0, None, None
    for line in open(log, errors="replace"):
        line = line.strip()
        if not line.startswith("[FBDUMP] "):
            continue
        body = line[9:]
        if body.startswith("BEGIN"):
            hdr = list(map(int, body.split()[1:5])); cur = []
        elif body == "END" and cur is not None:
            w, h, bpp, pitch = hdr
            raw = base64.b64decode("".join(cur))
            try:
                pix = zlib.decompress(raw)
            except zlib.error as e:
                # a line mangled on the wire (another thread logging mid-dump)
                print("frame corrupt (%s), skipped" % e)
                cur = None
                continue
            if len(pix) < h * pitch:
                print("frame truncated (%d of %d bytes), skipped" % (len(pix), h * pitch))
                cur = None
                continue
            rows = []
            for y in range(h):
                r = pix[y * pitch:(y * pitch) + w * (bpp // 8)]
                if bpp == 32:
                    out = bytearray(w * 3)
                    out[0::3] = r[2::4]; out[1::3] = r[1::4]; out[2::3] = r[0::4]
                else:
                    out = bytearray()
                    for x in range(w):
                        v = r[2 * x] | (r[2 * x + 1] << 8)
                        out += bytes(((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3))
                rows.append(bytes(out))
            path = "%s_%03d.png" % (pre, n); png(path, w, h, rows); print(path); n += 1
            cur = None
        elif cur is not None:
            cur.append(body)
    if n == 0:
        print("no frames", file=sys.stderr); sys.exit(1)

main()
