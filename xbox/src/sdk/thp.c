/* thp.c - Melee's movie frames (lbmthp.c, lb_01F8.c): one baseline JPEG
 * straight into the GX I8-tiled Y/U/V planes the game draws.
 *
 * The frames are THP-style JPEGs: ordinary DQT/SOF0/DHT/SOS headers, but
 * the entropy-coded data has no 0x00 stuffing after 0xFF bytes and the frame
 * length isn't passed in. So, like the GameCube decoder, this parses the
 * headers and then decodes exactly width x height worth of MCUs from a raw
 * bit stream, never looking for markers. Output is the decoder's own planes
 * (Y full size, Cb/Cr half size, 4:2:0), which the game's TEV setup turns
 * into RGB, so there is no colour conversion here. The IDCT is stb_image's.
 * Restart intervals are not supported (Melee's movies don't use them). */
#include <stdint.h>
#include <string.h>

#include "pc/pc.h"
#include "xhw.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#define STBI_ASSERT(x) ((void)0)
#include "pc/stb_image.h"

#define FAST_BITS 9

typedef struct {
    uint8_t fast_len[1 << FAST_BITS];   /* 0: code longer than FAST_BITS */
    uint8_t fast_val[1 << FAST_BITS];
    int32_t maxcode[18];                /* per length, left-aligned to 16 bits */
    int32_t delta[17];                  /* value index = code + delta[len] */
    uint8_t vals[256];
    int ok;
} Huff;

typedef struct {
    int id, h, v, tq, td, ta, dc;
    uint8_t* plane;
    int pw, ph;   /* plane size in pixels */
} Comp;

typedef struct {
    const uint8_t* p;
    uint32_t buf;
    int cnt;
} Bits;

static uint16_t s_q[4][64];
static Huff s_huff[2][4];   /* [0 = DC, 1 = AC][table] */
static int s_logged;

static void build_huff(Huff* t, const uint8_t* counts, const uint8_t* vals) {
    int len, i, k = 0, code = 0;
    memset(t, 0, sizeof *t);
    for (len = 1; len <= 16; len++) {
        t->delta[len] = k - code;
        for (i = 0; i < counts[len - 1]; i++, k++) {
            t->vals[k] = vals[k];
            if (len <= FAST_BITS) {
                int shift = FAST_BITS - len, j;
                for (j = 0; j < (1 << shift); j++) {
                    t->fast_len[(code << shift) | j] = (uint8_t)len;
                    t->fast_val[(code << shift) | j] = vals[k];
                }
            }
            code++;
        }
        t->maxcode[len] = code << (16 - len);   /* exclusive bound */
        code <<= 1;
    }
    t->maxcode[17] = 0x7fffffff;
    t->ok = 1;
}

static inline void fill(Bits* b) {
    while (b->cnt <= 24) {
        b->buf |= (uint32_t)*b->p++ << (24 - b->cnt);
        b->cnt += 8;
    }
}

static inline int get_bits(Bits* b, int n) {
    int v;
    if (!n) return 0;
    fill(b);
    v = (int)(b->buf >> (32 - n));
    b->buf <<= n;
    b->cnt -= n;
    return v;
}

static inline int decode_sym(Bits* b, const Huff* t) {
    int len, c;
    fill(b);
    c = (int)(b->buf >> (32 - FAST_BITS));
    len = t->fast_len[c];
    if (len) {
        b->buf <<= len;
        b->cnt -= len;
        return t->fast_val[c];
    }
    c = (int)(b->buf >> 16);
    for (len = FAST_BITS + 1; len <= 16; len++)
        if (c < t->maxcode[len]) break;
    if (len > 16) return -1;
    b->buf <<= len;
    b->cnt -= len;
    return t->vals[(c >> (16 - len)) + t->delta[len]];
}

static inline int extend(int v, int s) { return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v; }

static int decode_block(Bits* b, Comp* c, short blk[64]) {
    const uint16_t* q = s_q[c->tq];
    int s, k, r;
    memset(blk, 0, 64 * sizeof *blk);
    s = decode_sym(b, &s_huff[0][c->td]);
    if (s < 0) return 0;
    c->dc += s ? extend(get_bits(b, s), s) : 0;
    blk[0] = (short)(c->dc * q[0]);
    for (k = 1; k < 64;) {
        int rs = decode_sym(b, &s_huff[1][c->ta]);
        if (rs < 0) return 0;
        r = rs >> 4;
        s = rs & 15;
        if (!s) {
            if (r != 15) break;   /* EOB */
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) return 0;
        blk[stbi__jpeg_dezigzag[k]] = (short)(extend(get_bits(b, s), s) * q[k]);
        k++;
    }
    return 1;
}

/* An 8x8 block into an I8 plane of 8x4 tiles: two tiles, one per 4 rows. */
static void store_block(const Comp* c, int x, int y, const uint8_t* px) {
    int row;
    int tiles_w = (c->pw + 7) >> 3;
    if (x >= c->pw || y >= c->ph) return;
    for (row = 0; row < 8 && y + row < c->ph; row++) {
        int yy = y + row;
        uint8_t* dst = c->plane + (((yy >> 2) * tiles_w + (x >> 3)) << 5) + ((yy & 3) << 3);
        memcpy(dst, px + row * 8, 8);
    }
}

static int be16(const uint8_t* p) { return (p[0] << 8) | p[1]; }

void pc_thp_decode_frame(const void* jpeg, void* tile_y, void* tile_u, void* tile_v) {
    const uint8_t* p = (const uint8_t*)jpeg;
    uint8_t* planes[3] = { (uint8_t*)tile_y, (uint8_t*)tile_u, (uint8_t*)tile_v };
    Comp comp[3];
    int ncomp = 0, width = 0, height = 0, hmax = 1, vmax = 1, i;
    int guard;

    if (!p || p[0] != 0xFF || p[1] != 0xD8) goto fail;
    p += 2;
    memset(comp, 0, sizeof comp);
    for (guard = 0; guard < 64; guard++) {
        int m, len;
        while (*p != 0xFF) p++;   /* tolerate fill bytes */
        while (*p == 0xFF) p++;
        m = *p++;
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) continue;
        len = be16(p);
        if (m == 0xDB) {   /* DQT */
            const uint8_t* q = p + 2;
            while (q < p + len) {
                int pq = q[0] >> 4, tq = q[0] & 3, k;
                q++;
                for (k = 0; k < 64; k++) s_q[tq][k] = pq ? (uint16_t)be16(q + 2 * k) : q[k];
                q += pq ? 128 : 64;
            }
        } else if (m == 0xC0 || m == 0xC1) {   /* SOF0/1 */
            height = be16(p + 3);
            width = be16(p + 5);
            ncomp = p[7];
            if (ncomp != 3) goto fail;
            for (i = 0; i < 3; i++) {
                comp[i].id = p[8 + 3 * i];
                comp[i].h = p[9 + 3 * i] >> 4;
                comp[i].v = p[9 + 3 * i] & 15;
                comp[i].tq = p[10 + 3 * i] & 3;
                if (comp[i].h > hmax) hmax = comp[i].h;
                if (comp[i].v > vmax) vmax = comp[i].v;
            }
        } else if (m == 0xC4) {   /* DHT */
            const uint8_t* h = p + 2;
            while (h < p + len) {
                int tc = h[0] >> 4, th = h[0] & 3, n = 0, k;
                for (k = 0; k < 16; k++) n += h[1 + k];
                if (tc > 1 || n > 256) goto fail;
                build_huff(&s_huff[tc][th], h + 1, h + 17);
                h += 17 + n;
            }
        } else if (m == 0xDD) {   /* DRI */
            if (be16(p + 2) && !s_logged) {
                s_logged = 1;
                xhw_logf("[THP] restart intervals are not supported");
            }
        } else if (m == 0xDA) {   /* SOS: decode */
            int ns = p[2], mx, my, mcux, mcuy;
            Bits b;
            short blk[64];
            uint8_t px[64];
            if (!width || ns != 3) goto fail;
            for (i = 0; i < ns; i++) {
                int id = p[3 + 2 * i], t = p[4 + 2 * i], c;
                for (c = 0; c < 3; c++)
                    if (comp[c].id == id) {
                        comp[c].td = t >> 4;
                        comp[c].ta = t & 3;
                    }
            }
            for (i = 0; i < 3; i++) {
                comp[i].plane = planes[i];
                comp[i].pw = (width * comp[i].h + hmax - 1) / hmax;
                comp[i].ph = (height * comp[i].v + vmax - 1) / vmax;
                comp[i].dc = 0;
                if (!s_huff[0][comp[i].td].ok || !s_huff[1][comp[i].ta].ok) goto fail;
            }
            b.p = p + len;
            b.buf = 0;
            b.cnt = 0;
            mcux = (width + 8 * hmax - 1) / (8 * hmax);
            mcuy = (height + 8 * vmax - 1) / (8 * vmax);
            for (my = 0; my < mcuy; my++)
                for (mx = 0; mx < mcux; mx++)
                    for (i = 0; i < 3; i++) {
                        Comp* c = &comp[i];
                        int bx, by;
                        for (by = 0; by < c->v; by++)
                            for (bx = 0; bx < c->h; bx++) {
                                if (!decode_block(&b, c, blk)) goto fail;
                                stbi__idct_block(px, 8, blk);
                                store_block(c, (mx * c->h + bx) * 8, (my * c->v + by) * 8, px);
                            }
                    }
            return;
        } else if (m == 0xD9) {
            break;
        }
        p += len;
    }

fail:
    /* black rather than whatever the buffers held (the TEV recipe takes
     * full-range Y) */
    if (width && height) {
        memset(planes[0], 0, (size_t)width * height);
        memset(planes[1], 128, (size_t)(width / 2) * (height / 2));
        memset(planes[2], 128, (size_t)(width / 2) * (height / 2));
    }
    if (!s_logged) {
        s_logged = 1;
        xhw_logf("[THP] could not decode a movie frame");
    }
}
