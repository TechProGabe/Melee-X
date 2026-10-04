/* test_tex_cache.c - host check of gx_tex.c's texture cache and binds
 * (GXInitTexObj/GXLoadTexObj, the lookup, revalidation, uploads, evictions,
 * EFB copies, frame ends) against tests/xbox/gx_tex_ref.c, the file before
 * the cache entries were split into a hot and a cold array and the bind path
 * lost its second bind_unchanged(), its memcmp calls and its eager
 * GXGetTexBufferSize. Built twice by tools/xbox/test_tex_cache.py, around
 * either file (TEX_REF for the reference); each run drives the same seeded
 * HSD-like sequence and folds everything the rest of the port can see into a
 * trace: back-end texture creates (with their texels) and destroys, flushes,
 * log lines, the eight texture maps and the dirty bits after every call, and
 * at every frame end the cache itself (each entry's key, handle, frames and
 * hashes, the bucket chains, the per-map bind memo, the counters). The
 * script compares the two traces' checkpoints. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

XgxState g_xgx;

/* ---- trace ---- */
static uint32_t s_digest = 2166136261u;
static uint64_t s_events;
static void fold(const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    while (n--) s_digest = (s_digest ^ *b++) * 16777619u;
}
static void fold32(uint32_t v) { fold(&v, 4); }
static void tr(const char* fmt, ...) {
    char line[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > 0) fold(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
    s_events++;
}

/* the textures' memory: 32 KB aligned, so the cache's buckets (address bits
 * 5-14) and tex_due's stagger (bits 5-6) don't depend on where malloc put it */
#define HEAP_BYTES (12u << 20)
static uint8_t* s_heap;
static uint32_t s_heap_used;
static uint32_t off(const void* p) {
    if (!p) return 0xFFFFFFFFu;
    if ((const uint8_t*)p < s_heap || (const uint8_t*)p >= s_heap + HEAP_BYTES) return 0xFFFFFFFEu;
    return (uint32_t)((const uint8_t*)p - s_heap);
}

/* ---- the back end: a texture pool that fills up ---- */
#define MAX_HANDLES 65536
static uint32_t s_tex_bytes[MAX_HANDLES];
static uint8_t s_tex_live[MAX_HANDLES], s_tex_over[MAX_HANDLES];
static uint32_t s_next_handle = 1, s_pool_used, s_pool_total = 1536u << 10, s_grown;
static uint64_t s_creates, s_create_fails, s_destroys;
static uint32_t native_bytes(uint32_t w, uint32_t h, uint32_t levels, uint32_t fmt) {
    uint32_t n = 0, l, lw = w, lh = h;
    for (l = 0; l < levels; l++) {
        n += fmt == XGX_TEX_DXT1   ? ((lw + 3) / 4) * ((lh + 3) / 4) * 8
             : fmt == XGX_TEX_DXT3 ? ((lw + 3) / 4) * ((lh + 3) / 4) * 16
                                 : lw * lh * (fmt == XGX_TEX_AY8 || fmt == XGX_TEX_P8 ? 1 : fmt == XGX_TEX_ARGB8 ? 4 : 2);
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }
    if (fmt == XGX_TEX_P8) n += XGX_TEX_PALETTE_BYTES;
    return n;
}
static uint32_t pool_alloc(uint32_t bytes) {
    uint32_t t;
    if (s_pool_used + bytes > s_pool_total + s_grown || s_next_handle >= MAX_HANDLES) return 0;
    t = s_next_handle++;
    s_tex_bytes[t] = bytes;
    s_tex_live[t] = 1;
    s_tex_over[t] = s_grown != 0;
    s_pool_used += bytes;
    return t;
}
uint32_t xgx_tex_create(uint32_t w, uint32_t h, uint32_t levels, uint32_t fmt, const void* data) {
    uint32_t n = native_bytes(w, h, levels, fmt), t = pool_alloc(n), dh = 2166136261u, i;
    const uint8_t* d = (const uint8_t*)data;
    for (i = 0; d && i < n; i++) dh = (dh ^ d[i]) * 16777619u;
    tr("create %u %u %u %u %08x -> %u", w, h, levels, fmt, dh, t);
    if (t) s_creates++;
    else s_create_fails++;
    return t;
}
void xgx_tex_destroy(uint32_t tex) {
    tr("destroy %u", tex);
    if (tex < MAX_HANDLES && s_tex_live[tex]) {
        s_tex_live[tex] = 0;
        s_pool_used -= s_tex_bytes[tex];
        s_destroys++;
    }
}
uint32_t xgx_tex_bytes(uint32_t tex) { return tex < MAX_HANDLES && s_tex_live[tex] ? s_tex_bytes[tex] : 0; }
uint32_t xgx_tex_pool_kb(void) { return (s_pool_total + s_grown) / 1024; }
uint32_t xgx_tex_pool_free_kb(void) { return (s_pool_total + s_grown - s_pool_used) / 1024; }
uint32_t xgx_tex_pool_largest_kb(void) { return xgx_tex_pool_free_kb(); }
int xgx_tex_pool_grow(void) {
    tr("grow %u", s_grown);
    if (s_grown) return 0;
    s_grown = 512u << 10;
    return 1;
}
int xgx_tex_in_overflow(uint32_t tex) { return tex < MAX_HANDLES && s_tex_live[tex] && s_tex_over[tex]; }
void xgx_tex_pool_shrink(void) {
    tr("shrink");
    s_grown = 0;
}
int xhw_perf_enter(int bucket) { return bucket; }
void xhw_perf_leave(int prev) { (void)prev; }
uint32_t xgx_tex_from_efb(const int32_t src[4], uint32_t w, uint32_t h, int i, uint32_t reuse) { return 0; }
void xgx_read_efb(const int32_t src[4], uint32_t w, uint32_t h, uint8_t* rgba) {}
/* the format only: "[TEX] changed" and "[TEX] drop" print host pointers */
void xhw_logf(const char* fmt, ...) { tr("logf %s", fmt); }
void xhw_log(const char* s) { tr("log %s", s); }   /* the frame stats: counts only */
static uint64_t s_flushes;
void gx_vtx_flush(void) {
    tr("flush");
    s_flushes++;
}

/* ---- the cache, logically (the reference keeps one array) ---- */
#ifdef TEX_REF
#define COLD(i) (&s_cache[i])
#else
#define COLD(i) (&s_cold[i])
#endif

static void fold_maps(void) {
    fold(g_xgx.map, sizeof g_xgx.map);
    fold32(g_xgx.dirty);
    g_xgx.dirty = 0;
}

static void fold_cache(void) {
    int i, m;
    fold32((uint32_t)s_count);
    fold32(s_frame);
    for (i = 0; i < s_count; i++) {
        const Entry* e = &s_cache[i];
        fold32(off(e->data));
        fold32(off(e->tlut_data));
        fold32(e->w | (uint32_t)e->h << 16);
        fold32(e->fmt | (uint32_t)e->levels << 8 | (uint32_t)e->tlut_fmt << 16 | (uint32_t)e->efb << 24);
        fold32(e->tex);
        fold32((uint32_t)e->next);
        fold32(e->checked);
        fold32(e->last_used);
        fold32(COLD(i)->hash);
        fold32(COLD(i)->tlut_hash);
        fold32(COLD(i)->qhash);
        fold32(COLD(i)->stable);
    }
    fold(s_bucket, sizeof s_bucket);
    for (m = 0; m < XGX_MAX_MAPS; m++) {
        const MapBind* b = &s_bound[m];
        fold32(off(b->obj.data));
        fold((const uint8_t*)&b->obj + sizeof b->obj.data, sizeof b->obj - sizeof b->obj.data);
        fold32(off(b->tlut_data));
        fold32(b->tex);
        fold32(b->frame);
    }
    fold32(s_st_uploads);
    fold32(s_st_evicts);
    fold32(s_st_evicts_hot);
    fold32(s_st_drops);
    fold32(s_st_drop_kb);
    fold32(s_st_fast);
    fold32(s_st_new_copies);
    fold32(s_st_chg_data);
    fold32(s_st_chg_tlut);
}

/* ---- the scene ---- */
static uint32_t s_rng = 12345;
static uint32_t rnd(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
static uint32_t below(uint32_t n) { return n ? rnd() % n : 0; }

typedef struct {
    uint32_t data;              /* heap offset */
    uint16_t w, h;
    uint8_t fmt, mipmap, ws, wt, min_f, mag_f, tlut;
    float max_lod, bias;
    int lod;                    /* GXInitTexObjLOD called */
} Tex;

#define NTEX 4096
#define NTLUT 20
#define NEFB 8
static Tex s_tex[NTEX];
static int s_ntex;
static uint32_t s_tlut_off[NTLUT], s_tlut_n[NTLUT];
static uint32_t s_efb_off[NEFB], s_efb_tex[NEFB];

static uint32_t heap_take(uint32_t bytes, uint32_t align) {
    uint32_t o = (s_heap_used + align - 1) & ~(align - 1), i;
    if (o + bytes > HEAP_BYTES) o = below((HEAP_BYTES - bytes) / 32) * 32;   /* full: overlap an old one */
    for (i = 0; i < bytes; i++) s_heap[o + i] = (uint8_t)rnd();
    if (o + bytes > s_heap_used) s_heap_used = o + bytes;
    return o;
}

static const uint8_t k_fmts[] = { GX_TF_I4, GX_TF_I8, GX_TF_IA4, GX_TF_IA8, GX_TF_RGB565, GX_TF_RGB5A3,
                                  GX_TF_RGBA8, GX_TF_CMPR, GX_TF_C4, GX_TF_C8, GX_TF_C14X2 };
static const uint16_t k_dims[] = { 1, 2, 4, 8, 16, 32, 64, 128, 256 };

static void make_tex(Tex* t, int tiny) {
    static const float k_bias[] = { 0.0f, -0.0f, 0.5f, -1.5f, 3.0f };
    uint32_t r = below(100), levels, bytes;
    memset(t, 0, sizeof *t);
    t->fmt = k_fmts[below(sizeof k_fmts)];
    if (tiny) {
        t->w = t->h = 8;
        t->fmt = below(2) ? GX_TF_I4 : GX_TF_CMPR;
    } else if (r < 12) {   /* not a power of two */
        t->w = (uint16_t)(1 + below(80));
        t->h = (uint16_t)(1 + below(80));
    } else {
        t->w = k_dims[2 + below(r < 80 ? 5 : 7)];
        t->h = k_dims[2 + below(r < 80 ? 5 : 7)];
    }
    t->mipmap = below(3) == 0;
    t->ws = (uint8_t)below(3);
    t->wt = (uint8_t)below(3);
    t->lod = below(4) != 0;
    t->min_f = (uint8_t)below(6);
    t->mag_f = (uint8_t)below(2);
    t->max_lod = (float)below(11);
    t->bias = k_bias[below(5)];
    t->tlut = (uint8_t)below(NTLUT);
    levels = 11;
    bytes = GXGetTexBufferSize(t->w, t->h, t->fmt, t->mipmap, (u8)levels);
    r = below(100);
    if (r < 12 && s_ntex > 0) {   /* the data of another texture, under another key */
        t->data = s_tex[below((uint32_t)s_ntex)].data;
        if (t->data + bytes > s_heap_used) t->data = heap_take(bytes, 32);
    } else if (r < 20) {          /* a 32 KB multiple: the same bucket as others */
        uint32_t o = (s_heap_used + 32767) & ~32767u, i;
        if (o + bytes > HEAP_BYTES) o = 32768u * below(HEAP_BYTES / 32768 - 8);
        for (i = 0; i < bytes; i++) s_heap[o + i] = (uint8_t)rnd();
        if (o + bytes > s_heap_used) s_heap_used = o + bytes;
        t->data = o;
    } else {
        t->data = heap_take(bytes, 32);
    }
}

static void load_tluts(void) {
    int i;
    for (i = 0; i < NTLUT; i++) {
        GXTlutObj tl;
        GXInitTlutObj(&tl, s_heap + s_tlut_off[i], (GXTlutFmt)(i % 3), (u16)s_tlut_n[i]);
        GXLoadTlut(&tl, (u32)i);
        fold_maps();
    }
}

static void bind(const Tex* t, uint32_t map) {
    GXTexObj obj;
    memset(&obj, 0xA5, sizeof obj);
    if (t->fmt == GX_TF_C4 || t->fmt == GX_TF_C8 || t->fmt == GX_TF_C14X2)
        GXInitTexObjCI(&obj, s_heap + t->data, t->w, t->h, (GXCITexFmt)t->fmt, (GXTexWrapMode)t->ws,
                       (GXTexWrapMode)t->wt, t->mipmap, t->tlut);
    else
        GXInitTexObj(&obj, s_heap + t->data, t->w, t->h, (GXTexFmt)t->fmt, (GXTexWrapMode)t->ws,
                     (GXTexWrapMode)t->wt, t->mipmap);
    if (t->lod)
        GXInitTexObjLOD(&obj, (GXTexFilter)t->min_f, (GXTexFilter)t->mag_f, 0.0f, t->max_lod, t->bias, GX_FALSE,
                        GX_FALSE, GX_ANISO_1);
    GXLoadTexObj(&obj, (GXTexMapID)map);
    fold_maps();
}

/* objects the binds refuse: never set up, no data, no size, a map past 7 */
static void bind_bad(void) {
    GXTexObj obj;
    const Tex* t = &s_tex[below((uint32_t)s_ntex)];
    uint32_t k = below(5), map = below(8);
    memset(&obj, (int)below(256), sizeof obj);
    if (k >= 1) GXInitTexObj(&obj, s_heap + t->data, t->w, t->h, (GXTexFmt)t->fmt, GX_CLAMP, GX_CLAMP, GX_FALSE);
    if (k == 1) GXInitTexObjData(&obj, NULL);
    if (k == 2) ((uint16_t*)&obj)[sizeof(void*) / 2 + below(2)] = 0;   /* w or h */
    if (k == 3) map = 8 + below(4);
    if (k == 4) {
        tr("bind direct");
        gx_tex_bind(map, below(2) ? &obj : NULL);
        fold_maps();
        return;
    }
    GXLoadTexObj(&obj, (GXTexMapID)map);
    fold_maps();
}

/* GXCopyTex: the back end makes (or refills) a texture at an EFB copy's
 * destination, which materials then bind like any texture */
static void efb_copy(int k) {
    uint32_t dest = s_efb_off[k], reuse, tex, w = k_dims[3 + below(5)], h = k_dims[3 + below(5)];
    uint32_t fmt = below(2) ? GX_TF_RGBA8 : GX_TF_I8;
    reuse = gx_tex_efb_texture(s_heap + dest);
    tr("efb %u reuse %u", dest, reuse);
    tex = reuse && below(4) ? reuse : below(10) ? pool_alloc(w * h * 4) : 0;
    tr("efb tex %u", tex);
    gx_tex_note_efb_copy(s_heap + dest, tex, w, h, fmt);
    fold_maps();
}

static void change_texels(const Tex* t) {
    uint32_t bytes = GXGetTexBufferSize(t->w, t->h, t->fmt, t->mipmap, 11), at;
    /* a sampled word (the quick hash's), the last word, or anywhere */
    switch (below(3)) {
        case 0: at = 0; break;
        case 1: at = bytes - 4; break;
        default: at = below(bytes); break;
    }
    s_heap[t->data + at] ^= (uint8_t)(1 + below(255));
}

int main(void) {
    static Tex* scene[600];
    int nscene = 0, i, k, frame, mats_per_frame;
    uint64_t binds = 0, fast = 0, chg_data = 0, chg_tlut = 0, drops = 0, evicts = 0, copies = 0;
    int max_count = 0, max_stable = 0;
    uint8_t* raw = (uint8_t*)malloc(HEAP_BYTES + 32768);
    s_heap = (uint8_t*)(((uintptr_t)raw + 32767) & ~(uintptr_t)32767);
    gx_tex_init();
    for (i = 0; i < NTLUT; i++) {
        s_tlut_n[i] = i < 10 ? 16 : i < 18 ? 256 : 0;   /* 0: GX's 256 */
        s_tlut_off[i] = heap_take((s_tlut_n[i] ? s_tlut_n[i] : 256) * 2, 32);
    }
    for (i = 0; i < NEFB; i++) s_efb_off[i] = heap_take(256 * 256 * 4, 32);
    for (s_ntex = 0; s_ntex < 1200; s_ntex++) make_tex(&s_tex[s_ntex], 0);
    for (i = 0; i < 6; i++) {   /* textures on EFB copies (shadows, the reflection) */
        Tex* t = &s_tex[s_ntex++];
        make_tex(t, 0);
        t->data = s_efb_off[i];
    }
    load_tluts();
    for (frame = 0; frame < 4000; frame++) {
        int phase = frame % 1000;
        /* a while on a small fixed set: textures pass TEX_STABLE and go to
         * the staggered quick revalidation */
        int quiet = phase >= 300 && phase < 560, pick = quiet && nscene > 60 ? 60 : nscene;
        /* the working set drifts; now and then a new scene, or a burst of
         * small textures past the cache's 2048 entries */
        if (frame % 250 == 0 || nscene == 0) {
            if (frame) {
                tr("scene leave");
                gx_tex_scene_leave();
                fold_maps();
            }
            nscene = 120 + (int)below(300);
            for (i = 0; i < nscene; i++) scene[i] = &s_tex[below((uint32_t)s_ntex)];
        } else if (!quiet) {
            for (i = 0; i < 3; i++) scene[below((uint32_t)nscene)] = &s_tex[below((uint32_t)s_ntex)];
        }
        if (frame % 500 == 77) {   /* larger than the whole pool: dropped, drawn untextured */
            static Tex huge;
            if (!huge.w) {
                make_tex(&huge, 0);
                huge.w = huge.h = 1024;
                huge.fmt = GX_TF_RGBA8;
                huge.mipmap = 0;
                huge.data = heap_take(1024 * 1024 * 4, 32);
            }
            bind(&huge, below(8));
        }
        if (phase >= 600 && phase < 606) {
            for (i = 0; i < 500 && s_ntex < NTEX; i++) {
                make_tex(&s_tex[s_ntex], 1);
                bind(&s_tex[s_ntex++], below(8));
                binds++;
            }
            if (s_ntex >= NTEX) s_ntex = 1206;   /* reuse the tiny slots */
        }
        if (below(40) == 0) load_tluts();
        if (below(25) == 0) {   /* a palette rewritten */
            uint32_t k = below(NTLUT);
            s_heap[s_tlut_off[k] + below(32)] ^= 0x40;
        }
        mats_per_frame = 20 + (int)below(60);
        for (i = 0; i < mats_per_frame; i++) {
            /* a material: one to three textures on maps 0.., bound for each
             * of its pobjs, the same objects again */
            const Tex* m[3];
            int n = 1 + (int)below(3), k, pobjs = 1 + (int)below(3), p;
            uint32_t base = below(6) == 0 ? below(5) : 0;
            for (k = 0; k < n; k++) m[k] = scene[below((uint32_t)pick)];
            for (p = 0; p < pobjs; p++)
                for (k = 0; k < n; k++) {
                    if (below(12) == 0) {   /* the same image through another TObj: one sampler setting differs */
                        static const float k_bias2[] = { 0.0f, -0.0f, 0.25f, 1.0f };
                        Tex v = *m[k];
                        switch (below(5)) {
                            case 0: v.bias = k_bias2[below(4)], v.lod = 1; break;
                            case 1: v.ws = (uint8_t)below(3); break;
                            case 2: v.wt = (uint8_t)below(3); break;
                            case 3: v.min_f = (uint8_t)below(6), v.lod = 1; break;
                            default: v.mag_f = (uint8_t)below(2), v.lod = 1; break;
                        }
                        bind(&v, base + (uint32_t)k);
                    } else {
                        bind(m[k], base + (uint32_t)k);
                    }
                    binds++;
                }
            if (below(50) == 0) bind_bad();
            if (below(30) == 0) efb_copy((int)below(NEFB));
            if (below(400) == 0) change_texels(scene[below((uint32_t)pick)]);
        }
        if (below(300) == 0) {
            tr("flush all");
            gx_tex_flush_all();
            fold_maps();
        }
        if (below(200) == 0) {
            uint32_t want = below(1024) * 1024;
            tr("make room %u -> %d", want, gx_tex_make_room(want));
            fold_maps();
        }
        if (below(100) == 0) {
            tr("grow for frame -> %d", gx_tex_grow_for_frame());
            GXInvalidateTexAll();
            fold_maps();
        }
        if (s_count > max_count) max_count = s_count;
        for (i = 0, k = 0; i < s_count; i++) k += COLD(i)->stable >= TEX_STABLE;
        if (k > max_stable) max_stable = k;
        if (s_frame % XGX_STATS_EVERY == 0) {   /* the counters before frame_end clears them */
            fast += s_st_fast;
            chg_data += s_st_chg_data;
            chg_tlut += s_st_chg_tlut;
            drops += s_st_drops;
            evicts += s_st_evicts;
            copies += s_st_new_copies;
        }
        gx_tex_frame_end();
        fold_maps();
        fold_cache();
        if (frame % 100 == 99) printf("frame %d: %08x\n", frame + 1, s_digest);
    }
    printf("binds %llu, events %llu, flushes %llu; back end: %llu creates, %llu failed, %llu destroys; most "
           "entries %d of %d\n",
           (unsigned long long)binds, (unsigned long long)s_events, (unsigned long long)s_flushes,
           (unsigned long long)s_creates, (unsigned long long)s_create_fails, (unsigned long long)s_destroys,
           max_count, CACHE_MAX);
    printf("binds skipped %llu, changed texels %llu, palettes %llu, drops %llu, evictions %llu, EFB copies to a new "
           "destination %llu; most entries past %d revalidations %d\n",
           (unsigned long long)fast, (unsigned long long)chg_data, (unsigned long long)chg_tlut,
           (unsigned long long)drops, (unsigned long long)evicts, (unsigned long long)copies, TEX_STABLE, max_stable);
    printf("digest %08x\n", s_digest);
    return 0;
}
