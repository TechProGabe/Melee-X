/* gx_tex.c - GX texture objects, TLUTs, and the decoded-texture cache.
 *
 * GameCube textures are tiled big-endian blocks in one of eleven formats.
 * They are converted once (to DXT1, AY8, A8Y8 or RGB565 when the NV2A can
 * sample them as is, else decoded to A8R8G8B8) and kept as back-end textures, keyed by
 * (data pointer, size, format, palette, mip count) and revalidated by a
 * sampled hash at most once a frame: HSD reuses archive memory, so a pointer
 * alone can go stale. EFB copies (GXCopyTex) register their destination
 * pointer, and a texture object pointing there binds the copy. */
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
} Entry;

#define CACHE_MAX 2048
static Entry s_cache[CACHE_MAX];
static int s_count;
static uint32_t s_frame = 1;
static uint32_t* s_scratch;
static uint32_t s_scratch_texels;   /* in 32-bit words */

void gx_tex_init(void) {}

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

/* sampled hash: whole small textures, 64 strided words of big ones */
static uint32_t hash_bytes(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u, i;
    if (!p) return 0;
    if (n <= 4096) {
        for (i = 0; i + 4 <= n; i += 4) h = (h ^ *(const uint32_t*)(p + i)) * 16777619u;
        return h;
    }
    {
        uint32_t step = (n / 4 / 64) * 4;
        for (i = 0; i < 64; i++) h = (h ^ *(const uint32_t*)(p + i * step)) * 16777619u;
        h = (h ^ *(const uint32_t*)(p + n - 4)) * 16777619u;
    }
    return h;
}

static Entry* find(const uint8_t* data, uint16_t w, uint16_t h, uint8_t fmt, uint8_t levels, const uint8_t* tlut_data) {
    int i;
    for (i = 0; i < s_count; i++) {
        Entry* e = &s_cache[i];
        if (e->tex && e->data == data && (e->efb || (e->w == w && e->h == h && e->fmt == fmt && e->levels == levels &&
                                                     e->tlut_data == tlut_data)))
            return e;
    }
    return NULL;
}

static void drop(Entry* e) {
    if (e->tex) xgx_tex_destroy(e->tex);
    *e = s_cache[--s_count];
}

static void evict_oldest(void) {
    int i, pick = -1;
    for (i = 0; i < s_count; i++)
        if (!s_cache[i].efb && (pick < 0 || s_cache[i].last_used < s_cache[pick].last_used)) pick = i;
    if (pick >= 0) drop(&s_cache[pick]);
}

/* ---- native formats ----
 * Power-of-two textures in formats the NV2A samples directly skip the 32-bit
 * decode: CMPR stays DXT1 (a quarter of the memory), intensity formats go to
 * AY8 / A8Y8 and RGB565 stays 16-bit. Everything else is A8R8G8B8. */
static uint32_t native_fmt(const TexObj* o) {
    if (o->w & (o->w - 1) || o->h & (o->h - 1)) return XGX_TEX_ARGB8;
    switch (o->fmt) {
        case GX_TF_CMPR: return o->w >= 4 && o->h >= 4 ? XGX_TEX_DXT1 : XGX_TEX_ARGB8;
        case GX_TF_I4: case GX_TF_I8: return XGX_TEX_AY8;
        case GX_TF_IA4: case GX_TF_IA8: return XGX_TEX_A8Y8;
        case GX_TF_RGB565: return XGX_TEX_RGB565;
        default: return XGX_TEX_ARGB8;
    }
}

static uint32_t native_size(uint32_t fmt, uint32_t w, uint32_t h) {
    switch (fmt) {
        case XGX_TEX_DXT1: return ((w + 3) / 4) * ((h + 3) / 4) * 8;
        case XGX_TEX_AY8: return w * h;
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
                            case GX_TF_I8: out[i] = src[y * 8 + x]; break;
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

static uint32_t upload(const TexObj* o, const TlutObj* tl, uint32_t levels, uint32_t bytes) {
    uint32_t need = 0, w = o->w, h = o->h, l, tex, xfmt = native_fmt(o);
    const uint8_t* src = o->data;
    uint8_t* dst;
    for (l = 0; l < levels; l++) {
        need += native_size(xfmt, w, h);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
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
    tex = xgx_tex_create(o->w, o->h, levels, xfmt, s_scratch);
    while (!tex && s_count > 0) {
        evict_oldest();
        tex = xgx_tex_create(o->w, o->h, levels, xfmt, s_scratch);
    }
    return tex;
}

void gx_tex_bind(uint32_t map, const GXTexObj* obj) {
    const TexObj* o = (const TexObj*)obj;
    const TlutObj* tl = NULL;
    uint32_t levels = 1, bytes, hash, thash = 0;
    Entry* e;
    XgxMap* m = &g_xgx.map[map];
    if (map >= XGX_MAX_MAPS) return;
    if (!o || o->magic != TEXOBJ_MAGIC || !o->data || !o->w || !o->h) {
        m->tex = 0;
        g_xgx.dirty |= XGX_DIRTY_MAPS;
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
    if (o->is_ci && o->tlut < TLUT_SLOTS) tl = &s_tlut[o->tlut];
    bytes = GXGetTexBufferSize(o->w, o->h, o->fmt, o->mipmap, (u8)levels);
    e = find(o->data, o->w, o->h, o->fmt, (uint8_t)levels, tl ? tl->data : NULL);
    if (e && !e->efb && e->checked != s_frame) {
        e->checked = s_frame;
        hash = hash_bytes(o->data, bytes);
        if (tl) thash = hash_bytes(tl->data, (tl->entries ? tl->entries : 256) * 2);
        if (hash != e->hash || thash != e->tlut_hash) {
            drop(e);
            e = NULL;
        }
    }
    if (!e) {
        uint32_t tex;
        if (s_count == CACHE_MAX) evict_oldest();
        tex = upload(o, tl, levels, bytes);
        if (!tex) {
            m->tex = 0;
            g_xgx.dirty |= XGX_DIRTY_MAPS;
            return;
        }
        e = &s_cache[s_count++];
        memset(e, 0, sizeof *e);
        e->data = o->data;
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
}

/* EFB copy: remember which texture now holds the pixels at `dest` */
void gx_tex_note_efb_copy(const void* dest, uint32_t tex, uint32_t w, uint32_t h, uint32_t fmt) {
    int i;
    for (i = 0; i < s_count; i++)
        if (s_cache[i].data == (const uint8_t*)dest) {
            drop(&s_cache[i]);
            i--;
        }
    if (!tex) return;
    if (s_count == CACHE_MAX) evict_oldest();
    {
        Entry* e = &s_cache[s_count++];
        memset(e, 0, sizeof *e);
        e->data = (const uint8_t*)dest;
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
void gx_tex_frame_end(void) {
    int i;
    s_frame++;
    for (i = 0; i < s_count; i++)
        if (s_frame - s_cache[i].last_used > 600) {
            drop(&s_cache[i]);
            i--;
        }
}

void gx_tex_invalidate_all(void) {
    int i;
    for (i = 0; i < s_count; i++) s_cache[i].checked = 0;
}

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
    gx_vtx_flush();
    if (idx < TLUT_SLOTS) s_tlut[idx] = *(const TlutObj*)obj;
}

void GXInvalidateTexAll(void) {
    gx_vtx_flush();
    gx_tex_invalidate_all();
}

void GXInvalidateTexRegion(const GXTexRegion* r) { (void)r; }
