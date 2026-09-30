#!/usr/bin/env python3
"""Give default.xbe a dashboard icon.

  tools/xbox/xbe_title_image.py <default.xbe> <logo.png> [default.tbn]

Adds a $$XTIMAGE section (what the MS dashboard, UnleashX, XBMC and friends
read) holding an XPR0 128x128 DXT1 texture with 1-bit alpha, and optionally
writes default.tbn (a 256x256 PNG; the XBMC-family folder thumbnail).

cxbe can't do this: it copies PE section names, which are 8 chars, and the
name must be "$$XTIMAGE". The XBE header area here ends well before the
first section (0x1000), so a fresh section-header table (old entries + the
new one), its name and its shared-page refcounts are appended there without
moving anything; the image data goes at the end of the file, mapped just
past the last section and not preloaded.
"""
import struct
import sys

from PIL import Image

HDR_LIMIT = 0x1000


def dxt1_block(px):
    """px: 16 (r,g,b,a). Returns 8 bytes. Alpha < 128 -> transparent texel."""
    opaque = [p for p in px if p[3] >= 128]
    if not opaque:
        return struct.pack("<HHI", 0, 0xFFFF, 0xFFFFFFFF)   # c0 <= c1: index 3 = transparent
    # endpoints: extremes along the luminance axis (simple, good enough at 128 px)
    lum = lambda p: p[0] * 299 + p[1] * 587 + p[2] * 114
    lo, hi = min(opaque, key=lum), max(opaque, key=lum)
    enc = lambda p: ((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3)
    dec = lambda c: (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
    c_hi, c_lo = enc(hi), enc(lo)
    transparent = len(opaque) < 16
    if transparent:
        # 3-colour mode (c0 <= c1): 0=c0 1=c1 2=mid 3=transparent
        c0, c1 = min(c_hi, c_lo), max(c_hi, c_lo)
        a, b = dec(c0), dec(c1)
        pal = [a, b, tuple((x + y) // 2 for x, y in zip(a, b))]
    else:
        if c_hi == c_lo:
            c_lo = c_hi - 1 if c_hi else 1
        c0, c1 = max(c_hi, c_lo), min(c_hi, c_lo)
        a, b = dec(c0), dec(c1)
        pal = [a, b, tuple((2 * x + y) // 3 for x, y in zip(a, b)), tuple((x + 2 * y) // 3 for x, y in zip(a, b))]
    bits = 0
    for i, p in enumerate(px):
        if transparent and p[3] < 128:
            idx = 3
        else:
            idx = min(range(len(pal)), key=lambda k: sum((p[j] - pal[k][j]) ** 2 for j in range(3)))
        bits |= idx << (2 * i)
    return struct.pack("<HHI", c0, c1, bits)


def xpr_dxt1(img):
    img = img.convert("RGBA").resize((128, 128), Image.LANCZOS)
    px = img.load()
    data = bytearray()
    for by in range(0, 128, 4):
        for bx in range(0, 128, 4):
            data += dxt1_block([px[bx + x, by + y] for y in range(4) for x in range(4)])
    # D3D texture header: Common (1 ref, type texture), Data, Lock, Format, Size
    fmt = 0x1 | (1 << 3) | (2 << 4) | (0x0C << 8) | (1 << 16) | (7 << 20) | (7 << 24)  # DXT1 128x128, 1 level
    hdr_size = 0x800
    head = struct.pack("<4sII", b"XPR0", hdr_size + len(data), hdr_size)
    head += struct.pack("<IIIII", 0x00040001, 0, 0, fmt, 0) + struct.pack("<I", 0xFFFFFFFF)
    head += b"\xAD" * (hdr_size - len(head))
    return bytes(head) + bytes(data)


def add_section(xbe, name, payload):
    d = bytearray(xbe)
    base, size_hdrs, size_img = struct.unpack_from("<III", d, 0x104)
    nsec, sec_addr = struct.unpack_from("<II", d, 0x11C)
    old = d[sec_addr - base:sec_addr - base + nsec * 0x38]
    for i in range(nsec):
        na = struct.unpack_from("<I", old, i * 0x38 + 20)[0]
        if d[na - base:na - base + len(name) + 1] == name.encode() + b"\0":
            return None   # already added (ninja didn't relink)
    last = max(struct.unpack_from("<II", old, i * 0x38 + 4) for i in range(nsec))
    va = (last[0] + last[1] + 0xFFF) & ~0xFFF
    raw = (len(d) + 0xFFF) & ~0xFFF

    # appended header data: [section headers][refcounts u16 x2][name]
    at = (size_hdrs + 3) & ~3
    new_tab = at
    refs = new_tab + (nsec + 1) * 0x38
    name_at = refs + 4
    end = name_at + len(name) + 1
    if end > HDR_LIMIT:
        sys.exit("no room in the XBE header area")
    tab = bytearray(old)
    tab += struct.pack("<IIIIIIIII", 0x08 | 0x10 | 0x20, va, len(payload), raw, len(payload),
                       base + name_at, 0, base + refs, base + refs + 2) + bytes(20)
    d[new_tab:new_tab + len(tab)] = tab
    d[refs:refs + 4] = bytes(4)
    d[name_at:end] = name.encode() + b"\0"

    struct.pack_into("<I", d, 0x108, end)                       # SizeOfHeaders
    struct.pack_into("<I", d, 0x10C, va + len(payload) - base)  # SizeOfImage
    struct.pack_into("<II", d, 0x11C, nsec + 1, base + new_tab)
    d += bytes(raw - len(d)) + payload
    return bytes(d)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    xbe_path, png = sys.argv[1], sys.argv[2]
    img = Image.open(png)
    xbe = open(xbe_path, "rb").read()
    out = add_section(xbe, "$$XTIMAGE", xpr_dxt1(img))
    if out is None:
        print(f"{xbe_path}: $$XTIMAGE already present")
    else:
        open(xbe_path, "wb").write(out)
        print(f"{xbe_path}: +$$XTIMAGE (128x128 DXT1)")
    if len(sys.argv) > 3:
        img.convert("RGBA").resize((256, 256), Image.LANCZOS).save(sys.argv[3], "PNG")
        print(f"wrote {sys.argv[3]}")


if __name__ == "__main__":
    main()
