/* gx_tex.c - GX texture objects, TLUTs, and the decoded-texture cache.
 *
 * GameCube textures are tiled big-endian blocks in one of eleven formats.
 * They are converted once (to DXT1, AY8, A8Y8 or RGB565 when the NV2A can
 * sample them as is, else decoded to A8R8G8B8) and kept as back-end textures, keyed by
 * (data pointer, size, format, palette, mip count) and revalidated by a
 * sampled hash at most once a frame: HSD reuses archive memory, so a pointer
 * alone can go stale. EFB copies (GXCopyTex) register their destination
 * pointer, and a texture object pointing there binds the copy. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_internal.h"
#include "xhw.h"

typedef struct {
    const uint8_t* data;
    uint16_t w, h;
    uint8_t fmt, wrap_s, wrap_t, mipmap;
    uint8_t min_f, mag_f, is_ci, max_lod;
    uint32_t tlut;
    float lod_bias;
    uint32_t magic;
} TexObj;
_Static_assert(sizeof(TexObj) <= sizeof(GXTexObj), "GXTexObj too small");
#define TEXOBJ_MAGIC 0x54584F42u

typedef struct {
    const uint8_t* data;
    uint32_t fmt, entries;
} TlutObj;
_Static_assert(sizeof(TlutObj) <= sizeof(GXTlutObj), "GXTlutObj too small");

#define TLUT_SLOTS 20
static TlutObj s_tlut[TLUT_SLOTS];

/* ---- cache ---- */
typedef struct {
    const uint8_t* data;
    const uint8_t* tlut_data;
    uint16_t w, h;
    uint8_t fmt, levels, tlut_fmt, efb;
    uint32_t hash, tlut_hash;
    uint32_t tex;
    uint32_t last_used, checked;
    uint32_t stable;            /* revalidations passed in a row (tex_due) */
    int next;                   /* bucket chain by data pointer, -1: end */
} Entry;

#define CACHE_MAX 2048
#define CACHE_BUCKETS 1024
static Entry s_cache[CACHE_MAX];
static int s_count;
static int s_bucket[CACHE_BUCKETS];
static int s_bucket_ready;
static uint32_t s_frame = 1;
static uint32_t* s_scratch;
static uint32_t s_scratch_texels;   /* in 32-bit words */
static uint32_t s_st_uploads, s_st_evicts, s_st_evicts_hot, s_st_drops, s_st_drop_kb, s_st_fast, s_st_new_copies;
static uint32_t s_drop_logged;
static uint32_t s_st_chg_data, s_st_chg_tlut, s_chg_logged;   /* revalidation found new texels / a new palette */

/* The last object bound to each texture map this frame: binding it again
 * (HSD loads the same texture for every pobj of a material) skips the
 * lookup and the dirty bit. Cleared whenever an entry is dropped. */
typedef struct {
    TexObj obj;
    const uint8_t* tlut_data;
    uint32_t tex, frame;
} MapBind;
static MapBind s_bound[XGX_MAX_MAPS];

static uint32_t bucket_of(const uint8_t* data) { return ((uint32_t)(uintptr_t)data >> 5) % CACHE_BUCKETS; }

static void chain_init(void) {
    if (s_bucket_ready) return;
    memset(s_bucket, 0xFF, sizeof s_bucket);
    s_bucket_ready = 1;
}

static void chain_link(int i) {
    int* b = &s_bucket[bucket_of(s_cache[i].data)];
    s_cache[i].next = *b;
    *b = i;
}

static void chain_unlink(int i) {
    int* link = &s_bucket[bucket_of(s_cache[i].data)];
    while (*link >= 0 && *link != i) link = &s_cache[*link].next;
    if (*link == i) *link = s_cache[i].next;
}

/* a new entry at the end of the array, linked */
static Entry* entry_add(const uint8_t* data) {
    Entry* e = &s_cache[s_count];
    memset(e, 0, sizeof *e);
    e->data = data;
    chain_link(s_count++);
    return e;
}

void gx_tex_init(void) { chain_init(); }

/* ---- sizes ---- */
static void block_dims(uint32_t fmt, int* bw, int* bh, int* bpp) {
    switch (fmt) {
        case GX_TF_I4: case GX_TF_C4: case GX_TF_CMPR: *bw = 8; *bh = 8; *bpp = 4; break;
        case GX_TF_I8: case GX_TF_IA4: case GX_TF_C8: *bw = 8; *bh = 4; *bpp = 8; break;
        case GX_TF_RGBA8: *bw = 4; *bh = 4; *bpp = 32; break;
        default: *bw = 4; *bh = 4; *bpp = 16; break;   /* IA8 RGB565 RGB5A3 C14X2 */
    }
}

static uint32_t level_bytes(uint32_t fmt, uint32_t w, uint32_t h) {
    int bw, bh, bpp;
    block_dims(fmt, &bw, &bh, &bpp);
    w = (w + (uint32_t)bw - 1) & ~(uint32_t)(bw - 1);
    h = (h + (uint32_t)bh - 1) & ~(uint32_t)(bh - 1);
    return w * h * (uint32_t)bpp / 8;
}

u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, GXBool mipmap, u8 max_lod) {
    uint32_t total = 0, w = width, h = height, l, levels = mipmap ? max_lod : 1;
    if (!levels) levels = 1;
    for (l = 0; l < levels; l++) {
        total += level_bytes(format, w, h);
        if (w == 1 && h == 1) break;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return total;
}

/* ---- texel conversion ---- */
static inline uint32_t argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return a << 24 | r << 16 | g << 8 | b; }

static uint32_t rgb565(uint16_t v) {
    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return argb(255, r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2);
}

static uint32_t rgb5a3(uint16_t v) {
    if (v & 0x8000) {
        uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        return argb(255, r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2);
    } else {
        uint32_t a = (v >> 12) & 7, r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
        return argb(a << 5 | a << 2 | a >> 1, r * 17, g * 17, b * 17);
    }
}

static uint32_t ia8(uint16_t v) {
    uint32_t a = v >> 8, i = v & 0xFF;
    return argb(a, i, i, i);
}

static uint32_t tlut_color(const TlutObj* t, uint32_t idx) {
    uint16_t v;
    if (!t || !t->data || idx >= (t->entries ? t->entries : 4096)) return 0xFFFF00FFu;
    v = gx_be16(t->data + idx * 2);
    switch (t->fmt) {
        case GX_TL_IA8: return ia8(v);
        case GX_TL_RGB565: return rgb565(v);
        default: return rgb5a3(v);
    }
}

/* decode one level into out (w x h, row-major) */
static void decode_level(const uint8_t* src, uint32_t fmt, uint32_t w, uint32_t h, const TlutObj* tl,
                         uint32_t* out) {
    int bw, bh, bpp;
    uint32_t bx, by, x, y;
    block_dims(fmt, &bw, &bh, &bpp);
    for (by = 0; by < h; by += (uint32_t)bh) {
        for (bx = 0; bx < w; bx += (uint32_t)bw) {
            if (fmt == GX_TF_CMPR) {
                /* 8x8 block = 2x2 DXT1 sub-blocks, big-endian colours */
                int sb;
                for (sb = 0; sb < 4; sb++) {
                    uint32_t ox = bx + (uint32_t)(sb & 1) * 4, oy = by + (uint32_t)(sb >> 1) * 4, pal[4];
                    uint16_t c0 = gx_be16(src), c1 = gx_be16(src + 2);
                    uint32_t bits = gx_be32(src + 4);
                    pal[0] = rgb565(c0);
                    pal[1] = rgb565(c1);
                    if (c0 > c1) {
                        int k;
                        for (k = 0; k < 3; k++) {
                            uint32_t a = (pal[0] >> (k * 8)) & 0xFF, b = (pal[1] >> (k * 8)) & 0xFF;
                            ((uint8_t*)&pal[2])[k] = (uint8_t)((2 * a + b) / 3);
                            ((uint8_t*)&pal[3])[k] = (uint8_t)((a + 2 * b) / 3);
                        }
                        ((uint8_t*)&pal[2])[3] = ((uint8_t*)&pal[3])[3] = 255;
                    } else {
                        int k;
                        for (k = 0; k < 3; k++) {
                            uint32_t a = (pal[0] >> (k * 8)) & 0xFF, b = (pal[1] >> (k * 8)) & 0xFF;
                            ((uint8_t*)&pal[2])[k] = (uint8_t)((a + b) / 2);
                        }
                        ((uint8_t*)&pal[2])[3] = 255;
                        pal[3] = 0;
                    }
                    for (y = 0; y < 4; y++)
                        for (x = 0; x < 4; x++) {
                            uint32_t px = ox + x, py = oy + y;
                            uint32_t i = (bits >> (30 - 2 * (y * 4 + x))) & 3;
                            if (px < w && py < h) out[py * w + px] = pal[i];
                        }
                    src += 8;
                }
                continue;
            }
            if (fmt == GX_TF_RGBA8) {
                /* 4x4: 32 bytes of AR, then 32 bytes of GB */
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 4; x++) {
                        uint32_t i = y * 4 + x, px = bx + x, py = by + y;
                        uint32_t a = src[i * 2], r = src[i * 2 + 1], g = src[32 + i * 2], b = src[32 + i * 2 + 1];
                        if (px < w && py < h) out[py * w + px] = argb(a, r, g, b);
                    }
                src += 64;
                continue;
            }
            for (y = 0; y < (uint32_t)bh; y++) {
                for (x = 0; x < (uint32_t)bw; x++) {
                    uint32_t px = bx + x, py = by + y, c;
                    switch (fmt) {
                        case GX_TF_I4: {
                            uint32_t v = (src[(y * 8 + x) / 2] >> ((x & 1) ? 0 : 4)) & 15;
                            v *= 17;
                            c = argb(v, v, v, v);
                            break;
                        }
                        case GX_TF_I8: { uint32_t v = src[y * 8 + x]; c = argb(v, v, v, v); break; }
                        case GX_TF_IA4: {
                            uint32_t v = src[y * 8 + x], a = (v >> 4) * 17, i = (v & 15) * 17;
                            c = argb(a, i, i, i);
                            break;
                        }
                        case GX_TF_IA8: c = ia8(gx_be16(src + (y * 4 + x) * 2)); break;
                        case GX_TF_RGB565: c = rgb565(gx_be16(src + (y * 4 + x) * 2)); break;
                        case GX_TF_RGB5A3: c = rgb5a3(gx_be16(src + (y * 4 + x) * 2)); break;
                        case GX_TF_C4: c = tlut_color(tl, (src[(y * 8 + x) / 2] >> ((x & 1) ? 0 : 4)) & 15); break;
                        case GX_TF_C8: c = tlut_color(tl, src[y * 8 + x]); break;
                        case GX_TF_C14X2: c = tlut_color(tl, gx_be16(src + (y * 4 + x) * 2) & 0x3FFF); break;
                        default: c = 0xFFFF00FFu; break;
                    }
                    if (px < w && py < h) out[py * w + px] = c;
                }
            }
            src += (uint32_t)(bw * bh * bpp / 8);
        }
    }
}

/* sampled hash: whole small textures, 64 strided words of big ones. Four
 * FNV chains, one per word of every group of four: a single chain is a
 * serial multiply per word (this ran once a frame for every texture drawn). */
static uint32_t hash_bytes(const uint8_t* p, uint32_t n) {
    uint32_t h0 = 2166136261u, h1 = h0 ^ 1, h2 = h0 ^ 2, h3 = h0 ^ 3, i;
    if (!p) return 0;
    if (n <= 4096) {
        const uint32_t* w = (const uint32_t*)p;
        for (i = 0; i + 16 <= n; i += 16, w += 4) {
            h0 = (h0 ^ w[0]) * 16777619u;
            h1 = (h1 ^ w[1]) * 16777619u;
            h2 = (h2 ^ w[2]) * 16777619u;
            h3 = (h3 ^ w[3]) * 16777619u;
        }
        for (; i + 4 <= n; i += 4) h0 = (h0 ^ *(const uint32_t*)(p + i)) * 16777619u;
    } else {
        uint32_t step = (n / 4 / 64) * 4;
        for (i = 0; i < 64; i += 4) {
            h0 = (h0 ^ *(const uint32_t*)(p + i * step)) * 16777619u;
            h1 = (h1 ^ *(const uint32_t*)(p + (i + 1) * step)) * 16777619u;
            h2 = (h2 ^ *(const uint32_t*)(p + (i + 2) * step)) * 16777619u;
            h3 = (h3 ^ *(const uint32_t*)(p + (i + 3) * step)) * 16777619u;
        }
        h0 = (h0 ^ *(const uint32_t*)(p + n - 4)) * 16777619u;
    }
    return ((h0 ^ h1) * 16777619u ^ h2) * 16777619u ^ h3;
}

static Entry* find(const uint8_t* data, uint16_t w, uint16_t h, uint8_t fmt, uint8_t levels, const uint8_t* tlut_data) {
    int i;
    chain_init();
    for (i = s_bucket[bucket_of(data)]; i >= 0; i = s_cache[i].next) {
        Entry* e = &s_cache[i];
        if (e->tex && e->data == data && (e->efb || (e->w == w && e->h == h && e->fmt == fmt && e->levels == levels &&
                                                     e->tlut_data == tlut_data)))
            return e;
    }
    return NULL;
}

static int is_bound(uint32_t tex) {
    int m;
    for (m = 0; m < XGX_MAX_MAPS; m++)
        if (g_xgx.map[m].tex == tex) return 1;
    return 0;
}

/* Removes s_cache[i]; the last entry moves into its slot. */
static void drop_at(int i) {
    Entry* e = &s_cache[i];
    int m, last = s_count - 1;
    if (e->tex) {
        for (m = 0; m < XGX_MAX_MAPS; m++)
            if (g_xgx.map[m].tex == e->tex) {
                g_xgx.map[m].tex = 0;
                g_xgx.dirty |= XGX_DIRTY_MAPS;
            }
        xgx_tex_destroy(e->tex);
    }
    memset(s_bound, 0, sizeof s_bound);
    chain_unlink(i);
    if (i != last) {
        chain_unlink(last);
        s_cache[i] = s_cache[last];
        chain_link(i);
    }
    s_count--;
}

static void drop(Entry* e) { drop_at((int)(e - s_cache)); }

void gx_tex_flush_all(void) {
    int i;
    for (i = s_count - 1; i >= 0; i--)
        if (!s_cache[i].efb) drop_at(i);
}

/* Eviction victim: the least recently used entry no texture map holds, and
 * never one drawn this frame while an older one is left (evicting those
 * only makes the frame upload them again, or drop them). An EFB copy can't
 * be made again from memory, so among the older entries it counts as
 * EFB_GRACE frames younger than it is: textures are re-uploaded first, but
 * a stale copy still goes. -1: nothing to evict. */
#define EFB_GRACE 60
static int lru_victim(void) {
    int i, pick = -1, pick_hot = -1;
    int32_t best = 0;
    for (i = 0; i < s_count; i++) {
        const Entry* e = &s_cache[i];
        if (is_bound(e->tex)) continue;
        if (e->last_used != s_frame) {
            int32_t rank = (int32_t)(s_frame - e->last_used) - (e->efb ? EFB_GRACE : 0);   /* higher: evict first */
            if (pick < 0 || rank > best) {
                pick = i;
                best = rank;
            }
        } else if (pick_hot < 0) {
            pick_hot = i;
        }
    }
    if (pick < 0 && pick_hot >= 0) s_st_evicts_hot++;
    return pick >= 0 ? pick : pick_hot;
}

/* No entry left to evict but ones drawn this frame: the frame's working set
 * is larger than the pool. */
static int only_hot_left(void) {
    int i;
    for (i = 0; i < s_count; i++)
        if (!is_bound(s_cache[i].tex) && s_cache[i].last_used != s_frame) return 0;
    return 1;
}

/* Scene change: textures in the overflow pool go, then the pool itself
 * (xgx_tex_pool_grow). The next scene uploads what it draws. */
void gx_tex_scene_leave(void) {
    int i;
    for (i = s_count - 1; i >= 0; i--)
        if (xgx_tex_in_overflow(s_cache[i].tex)) drop_at(i);
    xgx_tex_pool_shrink();
}

/* Evicts until about `bytes` of the pool is released. Returns how many
 * entries went, 0 when there was nothing to evict or the request can never
 * fit (then evicting would only empty the cache). */
int gx_tex_make_room(uint32_t bytes) {
    uint32_t freed = 0;
    int n = 0, i;
    if (bytes / 1024 > xgx_tex_pool_kb()) return 0;
    while (freed < bytes && (i = lru_victim()) >= 0) {
        freed += xgx_tex_bytes(s_cache[i].tex);
        s_st_evicts++;
        drop_at(i);
        n++;
    }
    return n;
}

static void evict_one(void) {
    int i = lru_victim();
    if (i < 0) i = 0;   /* every entry bound: 2048 entries, 8 maps, can't happen */
    s_st_evicts++;
    drop_at(i);
}

/* ---- native formats ----
 * Power-of-two textures in formats the NV2A samples directly skip the 32-bit
 * decode: CMPR stays DXT1 (a quarter of the memory), intensity formats go to
 * AY8 / A8Y8, RGB565 stays 16-bit, and C4/C8 become 8-bit palette indices
 * with a 256-entry A8R8G8B8 palette (C8 as A8R8G8B8 was a quarter of a
 * Pokémon Stadium match's texture memory). Everything else is A8R8G8B8. */
static uint32_t native_fmt(const TexObj* o) {
    if (o->w & (o->w - 1) || o->h & (o->h - 1)) {
        /* NPOT: resampled to a power of two. Intensity formats are resampled
         * per channel (the same texels as the A8R8G8B8 path, a quarter or
         * half the memory: the Trophy Collection's NPOT I4 textures took 8
         * times their GX size and overflowed the pool). */
        switch (o->fmt) {
            case GX_TF_I4: case GX_TF_I8: return XGX_TEX_AY8;
            case GX_TF_IA4: case GX_TF_IA8: return XGX_TEX_A8Y8;
            default: return XGX_TEX_ARGB8;
        }
    }
    switch (o->fmt) {
        case GX_TF_CMPR: return o->w >= 4 && o->h >= 4 ? XGX_TEX_DXT1 : XGX_TEX_ARGB8;
        case GX_TF_I4: case GX_TF_I8: return XGX_TEX_AY8;
        case GX_TF_IA4: case GX_TF_IA8: return XGX_TEX_A8Y8;
        case GX_TF_RGB565: return XGX_TEX_RGB565;
        case GX_TF_C4: case GX_TF_C8: return XGX_TEX_P8;
        default: return XGX_TEX_ARGB8;
    }
}

static uint32_t native_size(uint32_t fmt, uint32_t w, uint32_t h) {
    switch (fmt) {
        case XGX_TEX_DXT1: return ((w + 3) / 4) * ((h + 3) / 4) * 8;
        case XGX_TEX_AY8: case XGX_TEX_P8: return w * h;
        case XGX_TEX_A8Y8: case XGX_TEX_RGB565: return w * h * 2;
        default: return w * h * 4;
    }
}

/* GX index byte: pixel 0 in bits 7-6; DXT1: pixel 0 in bits 1-0 */
static inline uint8_t rev2(uint8_t v) {
    return (uint8_t)((v >> 6) | ((v >> 2) & 0x0C) | ((v << 2) & 0x30) | (v << 6));
}

/* one level of a native-format texture, rows top to bottom (DXT1: block rows) */
static void convert_level(const uint8_t* src, uint32_t fmt, uint32_t xfmt, uint32_t w, uint32_t h, uint8_t* out) {
    uint32_t bx, by, x, y;
    if (xfmt == XGX_TEX_DXT1) {
        /* GX: 8x8 tiles of four DXT1 blocks (TL TR BL BR), tiles padded to 8x8 */
        uint32_t nbx = (w + 3) / 4, nby = (h + 3) / 4, tiles_x = (w + 7) / 8;
        for (by = 0; by < nby; by++)
            for (bx = 0; bx < nbx; bx++) {
                const uint8_t* b = src + ((by / 2) * tiles_x + bx / 2) * 32 + ((by & 1) * 2 + (bx & 1)) * 8;
                uint8_t* d = out + (by * nbx + bx) * 8;
                d[0] = b[1]; d[1] = b[0]; d[2] = b[3]; d[3] = b[2];
                d[4] = rev2(b[4]); d[5] = rev2(b[5]); d[6] = rev2(b[6]); d[7] = rev2(b[7]);
            }
        return;
    }
    {
        int bw, bh, bpp;
        block_dims(fmt, &bw, &bh, &bpp);
        for (by = 0; by < h; by += (uint32_t)bh)
            for (bx = 0; bx < w; bx += (uint32_t)bw) {
                for (y = 0; y < (uint32_t)bh; y++)
                    for (x = 0; x < (uint32_t)bw; x++) {
                        uint32_t px = bx + x, py = by + y, i = py * w + px;
                        if (px >= w || py >= h) continue;
                        switch (fmt) {
                            case GX_TF_I4: out[i] = (uint8_t)(((src[(y * 8 + x) / 2] >> ((x & 1) ? 0 : 4)) & 15) * 17); break;
                            case GX_TF_I8: case GX_TF_C8: out[i] = src[y * 8 + x]; break;
                            case GX_TF_C4: out[i] = (uint8_t)((src[(y * 8 + x) / 2] >> ((x & 1) ? 0 : 4)) & 15); break;
                            case GX_TF_IA4: {
                                uint8_t v = src[y * 8 + x];
                                out[i * 2] = (uint8_t)((v & 15) * 17);
                                out[i * 2 + 1] = (uint8_t)((v >> 4) * 17);
                                break;
                            }
                            case GX_TF_IA8: {
                                const uint8_t* t = src + (y * 4 + x) * 2;   /* A then I */
                                out[i * 2] = t[1];
                                out[i * 2 + 1] = t[0];
                                break;
                            }
                            default: {   /* RGB565 */
                                uint16_t v = gx_be16(src + (y * 4 + x) * 2);
                                memcpy(out + i * 2, &v, 2);
                                break;
                            }
                        }
                    }
                src += (uint32_t)(bw * bh * bpp / 8);
            }
    }
}

static uint32_t upload_now(const TexObj* o, const TlutObj* tl, uint32_t levels, uint32_t bytes) {
    uint32_t need = 0, w = o->w, h = o->h, l, tex, room, xfmt = native_fmt(o);
    const uint8_t* src = o->data;
    uint8_t* dst;
    for (l = 0; l < levels; l++) {
        need += native_size(xfmt, w, h);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    if (xfmt == XGX_TEX_P8) need += XGX_TEX_PALETTE_BYTES;
    need = (need + 3) / 4;
    if (need > s_scratch_texels) {
        free(s_scratch);
        s_scratch = (uint32_t*)malloc(need * 4);
        s_scratch_texels = s_scratch ? need : 0;
        if (!s_scratch) return 0;
    }
    (void)bytes;
    dst = (uint8_t*)s_scratch;
    w = o->w;
    h = o->h;
    for (l = 0; l < levels; l++) {
        if (xfmt == XGX_TEX_ARGB8) decode_level(src, o->fmt, w, h, tl, (uint32_t*)dst);
        else convert_level(src, o->fmt, xfmt, w, h, dst);
        src += level_bytes(o->fmt, w, h);
        dst += native_size(xfmt, w, h);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    if (xfmt == XGX_TEX_P8) {   /* the palette after the levels, as the ARGB decode would look it up */
        uint32_t i, pal[256];
        for (i = 0; i < 256; i++) pal[i] = tlut_color(tl, i);
        memcpy(dst, pal, sizeof pal);
    }
    s_st_uploads++;
    /* pool full: evict, wait for the GPU once, retry; each round frees
     * twice as much, since the pool fragments */
    tex = xgx_tex_create(o->w, o->h, levels, xfmt, s_scratch);
    if (!tex && only_hot_left() && xgx_tex_pool_grow())
        tex = xgx_tex_create(o->w, o->h, levels, xfmt, s_scratch);
    for (room = need * 4; !tex && gx_tex_make_room(room); room *= 2)
        tex = xgx_tex_create(o->w, o->h, levels, xfmt, s_scratch);
    return tex;
}

static uint32_t upload(const TexObj* o, const TlutObj* tl, uint32_t levels, uint32_t bytes) {
    int pf = xhw_perf_enter(XHW_PERF_TEX);
    uint32_t tex = upload_now(o, tl, levels, bytes);
    xhw_perf_leave(pf);
    return tex;
}

/* -DXGX_DEBUG_MAGENTA: a texture that could not be uploaded draws magenta
 * instead of untextured, so a drop shows on screen */
static uint32_t magenta_tex(void) {
#ifdef XGX_DEBUG_MAGENTA
    static uint32_t s_magenta;
    if (!s_magenta) {
        static uint32_t px[64];
        int i;
        for (i = 0; i < 64; i++) px[i] = 0xFFFF00FFu;
        s_magenta = xgx_tex_create(8, 8, 1, XGX_TEX_ARGB8, px);
    }
    return s_magenta;
#else
    return 0;
#endif
}

/* the same object again, already looked up and validated this frame:
 * nothing changes (s_bound is cleared whenever an entry is dropped) */
static int bind_unchanged(uint32_t map, const TexObj* o) {
    const MapBind* b = &s_bound[map];
    const TlutObj* tl = o->is_ci && o->tlut < TLUT_SLOTS ? &s_tlut[o->tlut] : NULL;
    return b->frame == s_frame && b->tex && g_xgx.map[map].tex == b->tex && b->tlut_data == (tl ? tl->data : NULL) &&
           memcmp(&b->obj, o, sizeof *o) == 0;
}

/* Revalidation (sampled hash of the texels and palette) runs at most once a
 * frame per texture; one that passed TEX_STABLE in a row is checked every
 * fourth frame, staggered by address. The hashes were ~4% of the console's
 * CPU in a match (some 230 textures a frame, scattered reads of MEM1), and
 * no texture changed there; one rewritten in place shows stale for up to
 * three frames. Movie planes change every other frame and never get there. */
#define TEX_STABLE 120
static int tex_due(Entry* e) {
    if (e->checked == s_frame) return 0;
    if (e->stable >= TEX_STABLE && ((s_frame + ((uint32_t)(uintptr_t)e->data >> 5)) & 3)) return 0;
    e->checked = s_frame;
    return 1;
}

void gx_tex_bind(uint32_t map, const GXTexObj* obj) {
    const TexObj* o = (const TexObj*)obj;
    const TlutObj* tl = NULL;
    uint32_t levels = 1, bytes, hash, thash = 0;
    Entry* e;
    XgxMap* m;
    MapBind* b;
    if (map >= XGX_MAX_MAPS) return;
    m = &g_xgx.map[map];
    b = &s_bound[map];
    if (!o || o->magic != TEXOBJ_MAGIC || !o->data || !o->w || !o->h) {
        m->tex = 0;
        b->frame = 0;
        g_xgx.dirty |= XGX_DIRTY_MAPS;
        return;
    }
    if (o->is_ci && o->tlut < TLUT_SLOTS) tl = &s_tlut[o->tlut];
    if (bind_unchanged(map, o)) {
        s_st_fast++;
        return;
    }
    if (o->mipmap) {
        uint32_t w = o->w, h = o->h;
        levels = 1;
        while ((w > 1 || h > 1) && levels < (o->max_lod ? o->max_lod + 1u : 11u)) {
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
            levels++;
        }
    }
#ifdef XGX_DEBUG_NOMIP
    levels = 1;
#endif
    bytes = GXGetTexBufferSize(o->w, o->h, o->fmt, o->mipmap, (u8)levels);
    e = find(o->data, o->w, o->h, o->fmt, (uint8_t)levels, tl ? tl->data : NULL);
    if (e && !e->efb && tex_due(e)) {
        hash = hash_bytes(o->data, bytes);
        if (tl) thash = hash_bytes(tl->data, (tl->entries ? tl->entries : 256) * 2);
        if (hash != e->hash || thash != e->tlut_hash) {
            if (hash != e->hash) s_st_chg_data++;
            else s_st_chg_tlut++;
            if (!s_chg_logged) {
                s_chg_logged = 1;
                xhw_logf("[TEX] changed: %ux%u fmt %u, %u levels, %s at %p", o->w, o->h, o->fmt, levels,
                         hash != e->hash ? "texels" : "palette only", (const void*)o->data);
            }
            drop(e);
            e = NULL;
        } else if (e->stable < TEX_STABLE) {
            e->stable++;
        }
    }
    if (!e) {
        uint32_t tex;
        if (s_count == CACHE_MAX) evict_one();
        tex = upload(o, tl, levels, bytes);
        if (!tex) {
            /* the pool can't take it even after evicting: the surface draws
             * untextured this time (black, usually) and the next bind tries again */
            s_st_drops++;
            s_st_drop_kb += bytes / 1024;
            if (!s_drop_logged) {
                s_drop_logged = 1;
                xhw_logf("[TEX] drop: %ux%u fmt %u, %u levels (%u KB of GX data); pool %u of %u KB free, largest "
                         "block %u KB, %d cached",
                         o->w, o->h, o->fmt, levels, bytes / 1024, xgx_tex_pool_free_kb(), xgx_tex_pool_kb(),
                         xgx_tex_pool_largest_kb(), s_count);
            }
            m->tex = magenta_tex();
            m->w = m->h = 8;
            m->wrap_s = m->wrap_t = GX_REPEAT;
            m->min_filter = m->mag_filter = GX_NEAR;
            m->lod_bias = 0;
            b->frame = 0;
            g_xgx.dirty |= XGX_DIRTY_MAPS;
            return;
        }
        e = entry_add(o->data);
        e->w = o->w;
        e->h = o->h;
        e->fmt = o->fmt;
        e->levels = (uint8_t)levels;
        e->tlut_data = tl ? tl->data : NULL;
        e->hash = hash_bytes(o->data, bytes);
        e->tlut_hash = tl ? hash_bytes(tl->data, (tl->entries ? tl->entries : 256) * 2) : 0;
        e->tex = tex;
        e->checked = s_frame;
    }
    e->last_used = s_frame;
    m->tex = e->tex;
    m->w = e->w;
    m->h = e->h;
    m->wrap_s = o->wrap_s;
    m->wrap_t = o->wrap_t;
    m->min_filter = o->min_f;
    m->mag_filter = o->mag_f;
    m->lod_bias = o->lod_bias;
    g_xgx.dirty |= XGX_DIRTY_MAPS;
    b->obj = *o;
    b->tlut_data = tl ? tl->data : NULL;
    b->tex = e->tex;
    b->frame = s_frame;
}

/* the texture the last EFB copy to `dest` made, for xgx_tex_from_efb to refill */
uint32_t gx_tex_efb_texture(const void* dest) {
    int i;
    chain_init();
    for (i = s_bucket[bucket_of((const uint8_t*)dest)]; i >= 0; i = s_cache[i].next)
        if (s_cache[i].efb && s_cache[i].data == (const uint8_t*)dest) return s_cache[i].tex;
    return 0;
}

/* EFB copy: remember which texture now holds the pixels at `dest` */
void gx_tex_note_efb_copy(const void* dest, uint32_t tex, uint32_t w, uint32_t h, uint32_t fmt) {
    chain_init();
    for (;;) {
        int i, hit = -1;
        for (i = s_bucket[bucket_of((const uint8_t*)dest)]; i >= 0; i = s_cache[i].next)
            if (s_cache[i].data == (const uint8_t*)dest) {
                hit = i;
                break;
            }
        if (hit < 0) break;
        if (tex && s_cache[hit].efb && s_cache[hit].tex == tex) {   /* refilled in place */
            s_cache[hit].w = (uint16_t)w;
            s_cache[hit].h = (uint16_t)h;
            s_cache[hit].fmt = (uint8_t)fmt;
            s_cache[hit].last_used = s_frame;
            return;
        }
        drop_at(hit);
    }
    if (!tex) return;
    if (s_count == CACHE_MAX) evict_one();
    s_st_new_copies++;
    {
        Entry* e = entry_add((const uint8_t*)dest);
        e->w = (uint16_t)w;
        e->h = (uint16_t)h;
        e->fmt = (uint8_t)fmt;
        e->levels = 1;
        e->efb = 1;
        e->tex = tex;
        e->last_used = s_frame;
    }
}

/* textures unused for ~10 s are released */
#ifndef XGX_STATS_EVERY
#define XGX_STATS_EVERY 600
#endif
void gx_tex_frame_end(void) {
    int i;
    if (s_frame % XGX_STATS_EVERY == 0) {
        /* working set of the last frame, by GX format: count / KB as GX data */
        uint32_t n[16] = { 0 }, kb[16] = { 0 }, pkb[16] = { 0 }, live = 0, pool_kb[2] = { 0, 0 }, pool_n[2] = { 0, 0 };
        char line[448];
        int k, len;
        for (i = 0; i < s_count; i++) {   /* what holds the pool: textures, EFB copies */
            pool_kb[s_cache[i].efb ? 1 : 0] += xgx_tex_bytes(s_cache[i].tex) / 1024;
            pool_n[s_cache[i].efb ? 1 : 0]++;
        }
        for (i = 0; i < s_count; i++)
            if (s_cache[i].last_used == s_frame && !s_cache[i].efb) {
                uint32_t f = s_cache[i].fmt & 15;
                n[f]++;
                kb[f] += GXGetTexBufferSize(s_cache[i].w, s_cache[i].h, s_cache[i].fmt, s_cache[i].levels > 1,
                                            s_cache[i].levels) / 1024;
                pkb[f] += xgx_tex_bytes(s_cache[i].tex) / 1024;
                live++;
            }
        len = snprintf(line, sizeof line,
                       "[TEX] %d cached, %u used last frame; per %u: %u uploads, %u evictions (%u of this frame's), "
                       "%u drops (%u KB), %u rebinds skipped, %u changed (%u palette only); fmt n/KB/pool KB:",
                       s_count, live, XGX_STATS_EVERY, s_st_uploads, s_st_evicts, s_st_evicts_hot, s_st_drops,
                       s_st_drop_kb, s_st_fast, s_st_chg_data + s_st_chg_tlut, s_st_chg_tlut);
        for (k = 0; k < 16; k++)
            if (n[k]) len += snprintf(line + len, sizeof line - (size_t)len, " %x:%u/%u/%u", k, n[k], kb[k], pkb[k]);
        xhw_log(line);
        xhw_logf("[TEX] pool holds %u textures (%u KB) and %u EFB copies (%u KB); per %u: %u copies to a new "
                 "destination", pool_n[0], pool_kb[0], pool_n[1], pool_kb[1], XGX_STATS_EVERY, s_st_new_copies);
        s_st_uploads = s_st_evicts = s_st_evicts_hot = s_st_drops = s_st_drop_kb = s_st_fast = s_st_new_copies = 0;
        s_drop_logged = 0;
        s_st_chg_data = s_st_chg_tlut = s_chg_logged = 0;
    }
    s_frame++;
    for (i = 0; i < s_count; i++)
        if (s_frame - s_cache[i].last_used > 600) {
            drop_at(i);
            i--;
        }
}

/* Every texture is revalidated (sampled hash) once a frame anyway. The
 * game invalidates after each EFB copy too (HSD's shadows: four times a
 * frame), which only concerns the copies, never hashed; revalidating every
 * texture again there cost ~2% of a match frame. */
void gx_tex_invalidate_all(void) {}

/* ---- GX API ---- */
void GXInitTexObj(GXTexObj* obj, const void* data, u16 w, u16 h, GXTexFmt fmt, GXTexWrapMode ws, GXTexWrapMode wt,
                  GXBool mipmap) {
    TexObj* o = (TexObj*)obj;
    memset(obj, 0, sizeof *obj);
    o->data = (const uint8_t*)data;
    o->w = w;
    o->h = h;
    o->fmt = (uint8_t)fmt;
    o->wrap_s = (uint8_t)ws;
    o->wrap_t = (uint8_t)wt;
    o->mipmap = mipmap;
    o->min_f = mipmap ? GX_LIN_MIP_LIN : GX_LINEAR;
    o->mag_f = GX_LINEAR;
    o->magic = TEXOBJ_MAGIC;
}

void GXInitTexObjCI(GXTexObj* obj, const void* data, u16 w, u16 h, GXCITexFmt fmt, GXTexWrapMode ws,
                    GXTexWrapMode wt, GXBool mipmap, u32 tlut) {
    TexObj* o = (TexObj*)obj;
    GXInitTexObj(obj, data, w, h, (GXTexFmt)fmt, ws, wt, mipmap);
    o->is_ci = 1;
    o->tlut = tlut;
}

void GXInitTexObjLOD(GXTexObj* obj, GXTexFilter min_f, GXTexFilter mag_f, f32 min_lod, f32 max_lod, f32 lod_bias,
                     GXBool bias_clamp, GXBool edge_lod, GXAnisotropy aniso) {
    TexObj* o = (TexObj*)obj;
    (void)min_lod;
    (void)bias_clamp;
    (void)edge_lod;
    (void)aniso;
    o->min_f = (uint8_t)min_f;
    o->mag_f = (uint8_t)mag_f;
    o->lod_bias = lod_bias;
    o->max_lod = (uint8_t)(max_lod > 0 ? max_lod : 0);
}

void GXInitTexObjData(GXTexObj* obj, const void* data) { ((TexObj*)obj)->data = (const uint8_t*)data; }
void GXInitTexObjWrapMode(GXTexObj* obj, GXTexWrapMode s, GXTexWrapMode t) {
    ((TexObj*)obj)->wrap_s = (uint8_t)s;
    ((TexObj*)obj)->wrap_t = (uint8_t)t;
}
void GXInitTexObjTlut(GXTexObj* obj, u32 tlut) { ((TexObj*)obj)->tlut = tlut; }

u16 GXGetTexObjWidth(const GXTexObj* obj) { return ((const TexObj*)obj)->w; }
u16 GXGetTexObjHeight(const GXTexObj* obj) { return ((const TexObj*)obj)->h; }
GXTexFmt GXGetTexObjFmt(const GXTexObj* obj) { return (GXTexFmt)((const TexObj*)obj)->fmt; }
void* GXGetTexObjData(const GXTexObj* obj) { return (void*)((const TexObj*)obj)->data; }
GXTexWrapMode GXGetTexObjWrapS(const GXTexObj* obj) { return (GXTexWrapMode)((const TexObj*)obj)->wrap_s; }
GXTexWrapMode GXGetTexObjWrapT(const GXTexObj* obj) { return (GXTexWrapMode)((const TexObj*)obj)->wrap_t; }
GXBool GXGetTexObjMipMap(const GXTexObj* obj) { return ((const TexObj*)obj)->mipmap; }

void GXLoadTexObj(GXTexObj* obj, GXTexMapID id) {
    const TexObj* o = (const TexObj*)obj;
    /* unchanged: no flush, so a waiting batch can still be continued */
    if (id < XGX_MAX_MAPS && o && o->magic == TEXOBJ_MAGIC && o->data && o->w && o->h && bind_unchanged(id, o)) {
        s_st_fast++;
        return;
    }
    gx_vtx_flush();
    gx_tex_bind(id, obj);
}

void GXInitTlutObj(GXTlutObj* obj, const void* data, GXTlutFmt fmt, u16 entries) {
    TlutObj* t = (TlutObj*)obj;
    memset(obj, 0, sizeof *obj);
    t->data = (const uint8_t*)data;
    t->fmt = fmt;
    t->entries = entries;
}

void GXLoadTlut(const GXTlutObj* obj, u32 idx) {
    if (idx < TLUT_SLOTS && memcmp(&s_tlut[idx], obj, sizeof s_tlut[idx]) == 0) return;
    gx_vtx_flush();
    if (idx < TLUT_SLOTS) s_tlut[idx] = *(const TlutObj*)obj;
}

void GXInvalidateTexAll(void) {
    gx_vtx_flush();
    gx_tex_invalidate_all();
}

void GXInvalidateTexRegion(const GXTexRegion* r) { (void)r; }
