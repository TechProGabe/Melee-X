/* gx_vtx.c - GX vertices -> the canonical vertex layout (xgx.h).
 *
 * Two sources:
 *  - immediate mode (GXBegin, GXPosition*, ..., GXEnd): native values, one
 *    call per attribute in GX attribute order;
 *  - display lists (GXCallDisplayList): the big-endian GX command stream HSD
 *    loads from the disc, with indexed attributes read from GXSetArray arrays
 *    (big-endian disc data, or native when the game built them).
 * Both decode each attribute by the current vertex descriptor and the VAT
 * of the batch's vertex format, into a vertex slot the back end handed out. */
#include <stdlib.h>
#include <string.h>

#include "gx_internal.h"
#include "xhw.h"

#ifndef XGX_STATS_EVERY
#define XGX_STATS_EVERY 600
#endif

/* GX attribute order within a vertex */
static const uint8_t k_order[] = {
    GX_VA_PNMTXIDX, GX_VA_TEX0MTXIDX, GX_VA_TEX1MTXIDX, GX_VA_TEX2MTXIDX, GX_VA_TEX3MTXIDX,
    GX_VA_TEX4MTXIDX, GX_VA_TEX5MTXIDX, GX_VA_TEX6MTXIDX, GX_VA_TEX7MTXIDX, GX_VA_POS,
    GX_VA_NRM, GX_VA_CLR0, GX_VA_CLR1, GX_VA_TEX0, GX_VA_TEX1, GX_VA_TEX2, GX_VA_TEX3,
    GX_VA_TEX4, GX_VA_TEX5, GX_VA_TEX6, GX_VA_TEX7,
};
#define N_ORDER (int)(sizeof k_order / sizeof k_order[0])

typedef struct {
    uint8_t attr;       /* GX_VA_* (GX_VA_NRM also stands for NBT) */
    uint8_t type;       /* GX_DIRECT / INDEX8 / INDEX16 */
    int32_t dst;        /* byte offset in the canonical vertex, -1: consumed only */
    GxAttrFmt fmt;
} Slot;

typedef struct {
    Slot slot[N_ORDER];
    int n;
    XgxLayout layout;
} Plan;

/* ---- the batch being built ---- */
static struct {
    int open;
    uint32_t prim;
    int vtxfmt;
    uint32_t expected, done;
    Plan plan;
    uint8_t* base;       /* back-end vertex memory */
    uint8_t* cur;        /* current vertex */
    int cursor;          /* next slot within the vertex */
    float pos_carry[3];   /* GXPosition2f32 components not yet a whole XYZ position */
    int npos_carry;
} B;

void gx_vtx_reset(void) { memset(&B, 0, sizeof B); }

static void make_plan(Plan* p, int vtxfmt) {
    int i, off = 12;
    XgxLayout* l = &p->layout;
    uint8_t nrm_type = g_gx.desc[GX_VA_NRM] != GX_NONE ? g_gx.desc[GX_VA_NRM] : g_gx.desc[GX_VA_NBT];
    memset(l, 0xFF, sizeof *l);   /* every offset -1 */
    l->off_pos = 0;
    p->n = 0;
    for (i = 0; i < N_ORDER; i++) {
        uint8_t a = k_order[i], type = a == GX_VA_NRM ? nrm_type : g_gx.desc[a];
        Slot* s;
        if (type == GX_NONE) continue;
        s = &p->slot[p->n++];
        s->attr = a;
        s->type = type;
        s->fmt = g_gx.vat[vtxfmt][a == GX_VA_NRM && g_gx.desc[GX_VA_NRM] == GX_NONE ? GX_VA_NBT : a];
        s->dst = -1;
        switch (a) {
            case GX_VA_PNMTXIDX: s->dst = l->off_mtx = off; off += 4; break;
            case GX_VA_POS: s->dst = 0; break;
            case GX_VA_NRM: s->dst = l->off_nrm = off; off += 12; break;
            case GX_VA_CLR0: s->dst = l->off_col[0] = off; off += 4; break;
            case GX_VA_CLR1: s->dst = l->off_col[1] = off; off += 4; break;
            default:
                if (a >= GX_VA_TEX0 && a <= GX_VA_TEX7) {
                    s->dst = l->off_tc[a - GX_VA_TEX0] = off;
                    off += 8;
                }
                break;   /* TEXnMTXIDX: consumed, not used */
        }
    }
    l->stride = (uint32_t)off;
}

/* ---- element decoding ---- */
static int comp_count(const Slot* s) {
    switch (s->attr) {
        case GX_VA_POS: return s->fmt.cnt == GX_POS_XY ? 2 : 3;
        case GX_VA_NRM: return s->fmt.cnt == GX_NRM_XYZ ? 3 : 9;
        default: return s->fmt.cnt == GX_TEX_S ? 1 : 2;
    }
}

static int elem_size(uint8_t type) {
    switch (type) {
        case GX_U8: case GX_S8: return 1;
        case GX_U16: case GX_S16: return 2;
        default: return 4;
    }
}

static int color_size(uint8_t type) {
    switch (type) {
        case GX_RGB565: case GX_RGBA4: return 2;
        case GX_RGB8: case GX_RGBA6: return 3;
        default: return 4;
    }
}

/* bytes one attribute's data occupies (direct in a DL, or one array entry) */
static int data_size(const Slot* s) {
    if (s->attr <= GX_VA_TEX7MTXIDX) return 1;
    if (s->attr == GX_VA_CLR0 || s->attr == GX_VA_CLR1) return color_size(s->fmt.type);
    return comp_count(s) * elem_size(s->fmt.type);
}

static float read_elem(const uint8_t* p, uint8_t type, float scale, int be) {
    switch (type) {
        case GX_U8: return p[0] * scale;
        case GX_S8: return (int8_t)p[0] * scale;
        case GX_U16: return (be ? gx_be16(p) : *(const uint16_t*)p) * scale;
        case GX_S16: return (int16_t)(be ? gx_be16(p) : *(const uint16_t*)p) * scale;
        default: {
            float f;
            if (be) return gx_bef(p);
            memcpy(&f, p, 4);
            return f;
        }
    }
}

static void read_color(const uint8_t* p, uint8_t type, int be, uint8_t* out) {
    uint32_t v;
    switch (type) {
        case GX_RGB565:
            v = be ? gx_be16(p) : *(const uint16_t*)p;
            out[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31);
            out[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
            out[2] = (uint8_t)((v & 31) * 255 / 31);
            out[3] = 255;
            break;
        case GX_RGB8:
            out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255;
            break;
        case GX_RGBX8:
            out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255;
            break;
        case GX_RGBA4:
            v = be ? gx_be16(p) : *(const uint16_t*)p;
            out[0] = (uint8_t)(((v >> 12) & 15) * 17);
            out[1] = (uint8_t)(((v >> 8) & 15) * 17);
            out[2] = (uint8_t)(((v >> 4) & 15) * 17);
            out[3] = (uint8_t)((v & 15) * 17);
            break;
        case GX_RGBA6:
            v = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
            out[0] = (uint8_t)(((v >> 18) & 63) * 255 / 63);
            out[1] = (uint8_t)(((v >> 12) & 63) * 255 / 63);
            out[2] = (uint8_t)(((v >> 6) & 63) * 255 / 63);
            out[3] = (uint8_t)((v & 63) * 255 / 63);
            break;
        default: /* RGBA8 */
            out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
            break;
    }
}

/* one attribute's data at p (direct data or an array entry) -> vertex */
static void store(const Slot* s, const uint8_t* p, int be, uint8_t* v) {
    if (s->dst < 0) return;
    if (s->attr == GX_VA_PNMTXIDX) {
        float f = (float)p[0];
        memcpy(v + s->dst, &f, 4);
    } else if (s->attr == GX_VA_CLR0 || s->attr == GX_VA_CLR1) {
        read_color(p, s->fmt.type, be, v + s->dst);
    } else {
        float out[3] = { 0, 0, 0 };
        int n = comp_count(s), i, es = elem_size(s->fmt.type);
        float scale;
        if (s->attr == GX_VA_NRM) {
            /* normals have fixed fractions: S8 1.6, S16 1.14 */
            scale = s->fmt.type == GX_S8 || s->fmt.type == GX_U8 ? 1.0f / 64 : 1.0f / 16384;
            if (n > 3) n = 3;   /* NBT: the normal comes first */
        } else {
            scale = 1.0f / (float)(1u << s->fmt.frac);
        }
        for (i = 0; i < n && i < 3; i++) out[i] = read_elem(p + i * es, s->fmt.type, scale, be);
        /* constant sizes: inline moves (xbuiltin.h) */
        if (s->attr == GX_VA_POS || s->attr == GX_VA_NRM) memcpy(v + s->dst, out, 12);
        else memcpy(v + s->dst, out, 8);
    }
}

static void fetch_indexed(const Slot* s, uint32_t idx, uint8_t* v) {
    uint8_t a = s->attr == GX_VA_NRM && g_gx.desc[GX_VA_NRM] == GX_NONE ? GX_VA_NBT : s->attr;
    const uint8_t* base = g_gx.array[a];
    if (!base) return;
    store(s, base + idx * g_gx.array_stride[a], !g_gx.array_le[a], v);
}

/* ---- batches ---- */
static void begin_batch(uint32_t prim, int vtxfmt, uint32_t n) {
    make_plan(&B.plan, vtxfmt);
    B.prim = prim;
    B.vtxfmt = vtxfmt;
    B.expected = n;
    B.done = 0;
    B.cursor = 0;
    B.base = (uint8_t*)xgx_vtx_alloc(n, B.plan.layout.stride);
    B.cur = B.base;
    B.open = 1;
    B.npos_carry = 0;
    if (B.base && n) memset(B.base, 0, B.plan.layout.stride);
}

static void end_batch(void) {
    uint32_t count = B.done;
    B.open = 0;
    if (!B.base || count == 0) return;
    xgx_draw(B.prim, count, &B.plan.layout, &g_xgx);
    g_xgx.dirty = 0;
}

void gx_vtx_flush(void) {
    if (B.open) end_batch();
}

void GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts) {
    if (B.open) end_batch();
    begin_batch((uint32_t)type, vtxfmt, nverts);
}

void GXEnd(void) {
    if (B.open) end_batch();
}

/* The slot for the next call of `attr` kind in immediate mode. Calls come in
 * GX order; a caller that skips an attribute leaves its data zeroed. */
static const Slot* next_slot(int want_mtx, uint8_t attr) {
    Plan* p = &B.plan;
    int i;
    if (!B.open || !B.base || B.done >= B.expected) return NULL;
    for (i = B.cursor; i < p->n; i++) {
        const Slot* s = &p->slot[i];
        if (want_mtx ? s->attr <= GX_VA_TEX7MTXIDX : s->attr == attr) {
            B.cursor = i + 1;
            return s;
        }
    }
    return NULL;
}

static void after_attr(void) {
    if (B.cursor >= B.plan.n) {
        B.done++;
        B.cursor = 0;
        B.cur += B.plan.layout.stride;
        if (B.done < B.expected) memset(B.cur, 0, B.plan.layout.stride);
    }
}

static void imm_floats(uint8_t attr, const float* f, int n) {
    const Slot* s = next_slot(0, attr);
    if (!s) return;
    if (s->dst >= 0) {
        float out[3] = { 0, 0, 0 };
        int i;
        for (i = 0; i < n && i < 3; i++) out[i] = f[i];
        if (attr == GX_VA_POS || attr == GX_VA_NRM) memcpy(B.cur + s->dst, out, 12);
        else memcpy(B.cur + s->dst, out, 8);
    }
    after_attr();
}

static void imm_index(uint8_t attr, uint32_t idx) {
    const Slot* s = next_slot(0, attr);
    if (!s) return;
    fetch_indexed(s, idx, B.cur);
    after_attr();
}

static float q(int v, uint8_t attr) { return (float)v / (float)(1u << g_gx.vat[B.vtxfmt][attr].frac); }

void GXPosition3f32(f32 x, f32 y, f32 z) { float f[3] = { x, y, z }; imm_floats(GX_VA_POS, f, 3); }
void GXPosition3u16(u16 x, u16 y, u16 z) { float f[3] = { q(x, GX_VA_POS), q(y, GX_VA_POS), q(z, GX_VA_POS) }; imm_floats(GX_VA_POS, f, 3); }
void GXPosition3s16(s16 x, s16 y, s16 z) { float f[3] = { q(x, GX_VA_POS), q(y, GX_VA_POS), q(z, GX_VA_POS) }; imm_floats(GX_VA_POS, f, 3); }
void GXPosition3u8(u8 x, u8 y, u8 z) { float f[3] = { q(x, GX_VA_POS), q(y, GX_VA_POS), q(z, GX_VA_POS) }; imm_floats(GX_VA_POS, f, 3); }
void GXPosition3s8(s8 x, s8 y, s8 z) { float f[3] = { q(x, GX_VA_POS), q(y, GX_VA_POS), q(z, GX_VA_POS) }; imm_floats(GX_VA_POS, f, 3); }
/* The GX FIFO takes components as a stream, so code that writes an XYZ
 * format's positions as pairs (HSD's shadow background quad: 12 floats in
 * six GXPosition2f32 calls) still makes whole vertices. Collect them. */
void GXPosition2f32(f32 x, f32 y) {
    float f[3] = { x, y, 0 };
    if (B.open && g_gx.vat[B.vtxfmt][GX_VA_POS].cnt == GX_POS_XYZ) {
        B.pos_carry[B.npos_carry++] = x;
        if (B.npos_carry == 3) {
            imm_floats(GX_VA_POS, B.pos_carry, 3);
            B.npos_carry = 0;
        }
        B.pos_carry[B.npos_carry++] = y;
        if (B.npos_carry == 3) {
            imm_floats(GX_VA_POS, B.pos_carry, 3);
            B.npos_carry = 0;
        }
        return;
    }
    imm_floats(GX_VA_POS, f, 3);
}
void GXPosition2u16(u16 x, u16 y) { float f[3] = { q(x, GX_VA_POS), q(y, GX_VA_POS), 0 }; imm_floats(GX_VA_POS, f, 3); }
void GXPosition2s16(s16 x, s16 y) { float f[3] = { q(x, GX_VA_POS), q(y, GX_VA_POS), 0 }; imm_floats(GX_VA_POS, f, 3); }
void GXPosition2u8(u8 x, u8 y) { float f[3] = { q(x, GX_VA_POS), q(y, GX_VA_POS), 0 }; imm_floats(GX_VA_POS, f, 3); }
void GXPosition2s8(s8 x, s8 y) { float f[3] = { q(x, GX_VA_POS), q(y, GX_VA_POS), 0 }; imm_floats(GX_VA_POS, f, 3); }
void GXPosition1x16(u16 i) { imm_index(GX_VA_POS, i); }
void GXPosition1x8(u8 i) { imm_index(GX_VA_POS, i); }

void GXNormal3f32(f32 x, f32 y, f32 z) { float f[3] = { x, y, z }; imm_floats(GX_VA_NRM, f, 3); }
void GXNormal3s16(s16 x, s16 y, s16 z) { float f[3] = { x / 16384.0f, y / 16384.0f, z / 16384.0f }; imm_floats(GX_VA_NRM, f, 3); }
void GXNormal3s8(s8 x, s8 y, s8 z) { float f[3] = { x / 64.0f, y / 64.0f, z / 64.0f }; imm_floats(GX_VA_NRM, f, 3); }
void GXNormal1x16(u16 i) { imm_index(GX_VA_NRM, i); }
void GXNormal1x8(u8 i) { imm_index(GX_VA_NRM, i); }

static void imm_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    /* colours go to CLR0, then CLR1, in order */
    const Slot* s = next_slot(0, GX_VA_CLR0);
    if (!s) s = next_slot(0, GX_VA_CLR1);
    if (!s) return;
    if (s->dst >= 0) {
        B.cur[s->dst] = r;
        B.cur[s->dst + 1] = g;
        B.cur[s->dst + 2] = b;
        B.cur[s->dst + 3] = a;
    }
    after_attr();
}

void GXColor4u8(u8 r, u8 g, u8 b, u8 a) { imm_color(r, g, b, a); }
void GXColor3u8(u8 r, u8 g, u8 b) { imm_color(r, g, b, 255); }
void GXColor1u32(u32 c) { imm_color((u8)(c >> 24), (u8)(c >> 16), (u8)(c >> 8), (u8)c); }
void GXColor1u16(u16 c) {
    uint8_t b[2] = { (uint8_t)(c >> 8), (uint8_t)c }, out[4];
    read_color(b, g_gx.vat[B.vtxfmt][GX_VA_CLR0].type, 1, out);
    imm_color(out[0], out[1], out[2], out[3]);
}
void GXColor1x16(u16 i) {
    const Slot* s = next_slot(0, GX_VA_CLR0);
    if (!s) s = next_slot(0, GX_VA_CLR1);
    if (!s) return;
    fetch_indexed(s, i, B.cur);
    after_attr();
}
void GXColor1x8(u8 i) { GXColor1x16(i); }

static void imm_tex(float s, float t) {
    const Slot* sl = NULL;
    int a;
    for (a = GX_VA_TEX0; a <= GX_VA_TEX7 && !sl; a++) sl = next_slot(0, (uint8_t)a);
    if (!sl) return;
    if (sl->dst >= 0) {
        float f[2] = { s, t };
        memcpy(B.cur + sl->dst, f, 8);
    }
    after_attr();
}

static float tq(int v) {
    int a;
    for (a = GX_VA_TEX0; a <= GX_VA_TEX7; a++)
        if (g_gx.desc[a] != GX_NONE) return (float)v / (float)(1u << g_gx.vat[B.vtxfmt][a].frac);
    return (float)v;
}

void GXTexCoord2f32(f32 s, f32 t) { imm_tex(s, t); }
void GXTexCoord2u16(u16 s, u16 t) { imm_tex(tq(s), tq(t)); }
void GXTexCoord2s16(s16 s, s16 t) { imm_tex(tq(s), tq(t)); }
void GXTexCoord2u8(u8 s, u8 t) { imm_tex(tq(s), tq(t)); }
void GXTexCoord2s8(s8 s, s8 t) { imm_tex(tq(s), tq(t)); }
void GXTexCoord1f32(f32 s) { imm_tex(s, 0); }
void GXTexCoord1u16(u16 s) { imm_tex(tq(s), 0); }
void GXTexCoord1s16(s16 s) { imm_tex(tq(s), 0); }
void GXTexCoord1u8(u8 s) { imm_tex(tq(s), 0); }
void GXTexCoord1s8(s8 s) { imm_tex(tq(s), 0); }
void GXTexCoord1x16(u16 i) {
    const Slot* sl = NULL;
    int a;
    for (a = GX_VA_TEX0; a <= GX_VA_TEX7 && !sl; a++) sl = next_slot(0, (uint8_t)a);
    if (!sl) return;
    fetch_indexed(sl, i, B.cur);
    after_attr();
}
void GXTexCoord1x8(u8 i) { GXTexCoord1x16(i); }

/* GXCmd1u8 inside a batch is a matrix index (PNMTXIDX, TEXnMTXIDX) */
void GXCmd1u8(const u8 x) {
    const Slot* s = next_slot(1, 0);
    if (!s) return;
    if (s->dst >= 0) {
        float f = (float)x;
        memcpy(B.cur + s->dst, &f, 4);
    }
    after_attr();
}
void GXCmd1u16(const u16 x) { (void)x; }
void GXCmd1u32(const u32 x) { (void)x; }
void GXParam1u8(const u8 x) { (void)x; }
void GXParam1u16(const u16 x) { (void)x; }
void GXParam1u32(const u32 x) { (void)x; }

/* ---- vertex descriptor / formats / arrays ---- */
void GXSetVtxDesc(GXAttr attr, GXAttrType type) {
    gx_vtx_flush();
    if (attr < GX_VA_MAX_ATTR) g_gx.desc[attr] = (uint8_t)type;
}

void GXSetVtxDescv(GXVtxDescList* list) {
    for (; list->attr != GX_VA_NULL; list++) GXSetVtxDesc(list->attr, list->type);
}

void GXClearVtxDesc(void) {
    gx_vtx_flush();
    memset(g_gx.desc, GX_NONE, sizeof g_gx.desc);
}

void GXSetVtxAttrFmt(GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    gx_vtx_flush();
    if (fmt >= 8 || attr >= GX_VA_MAX_ATTR) return;
    g_gx.vat[fmt][attr].cnt = (uint8_t)cnt;
    g_gx.vat[fmt][attr].type = (uint8_t)type;
    g_gx.vat[fmt][attr].frac = frac;
}

void GXSetArray(GXAttr attr, const void* data, u32 size, u8 stride, bool le) {
    (void)size;
    gx_vtx_flush();
    if (attr >= GX_VA_MAX_ATTR) return;
    g_gx.array[attr] = (const uint8_t*)data;
    g_gx.array_stride[attr] = stride;
    g_gx.array_le[attr] = le;
}

/* ---- display lists ---- */
static uint32_t dl_vertex_bytes(const Plan* p) {
    uint32_t n = 0;
    int i;
    for (i = 0; i < p->n; i++) {
        const Slot* s = &p->slot[i];
        int idx_count = s->attr == GX_VA_NRM && s->fmt.cnt == GX_NRM_NBT3 ? 3 : 1;
        if (s->type == GX_DIRECT) n += (uint32_t)data_size(s);
        else n += (uint32_t)(s->type == GX_INDEX8 ? 1 : 2) * (uint32_t)idx_count;
    }
    return n;
}

/* index range each indexed attribute used, for the cache's array hash */
typedef struct {
    uint32_t lo[GX_VA_MAX_ATTR], hi[GX_VA_MAX_ATTR];
} IdxRange;

static uint8_t array_attr(const Slot* s) {
    return s->attr == GX_VA_NRM && g_gx.desc[GX_VA_NRM] == GX_NONE ? GX_VA_NBT : s->attr;
}

/* n vertices of plan p from the list at `at` into out; returns the new
 * offset. idx (optional): the index of every indexed slot that lands in the
 * vertex, vertex by vertex in slot order (the display-list cache's dynamic
 * lists fetch from them again). */
static uint32_t decode_verts(const uint8_t* dl, uint32_t at, const Plan* p, uint32_t n, uint8_t* out,
                             IdxRange* r, uint16_t* idx_out) {
    uint32_t v;
    for (v = 0; v < n; v++, out += p->layout.stride) {
        int i;
        memset(out, 0, p->layout.stride);
        for (i = 0; i < p->n; i++) {
            const Slot* s = &p->slot[i];
            if (s->type == GX_DIRECT) {
                store(s, dl + at, 1, out);
                at += (uint32_t)data_size(s);
            } else {
                int k, idx_count = s->attr == GX_VA_NRM && s->fmt.cnt == GX_NRM_NBT3 ? 3 : 1;
                for (k = 0; k < idx_count; k++) {
                    uint32_t idx = s->type == GX_INDEX8 ? dl[at] : gx_be16(dl + at);
                    at += s->type == GX_INDEX8 ? 1 : 2;
                    if (k) continue;
                    if (idx_out && s->dst >= 0) *idx_out++ = (uint16_t)idx;
                    fetch_indexed(s, idx, out);
                    if (r) {
                        uint8_t a = array_attr(s);
                        if (idx < r->lo[a]) r->lo[a] = idx;
                        if (idx + 1 > r->hi[a]) r->hi[a] = idx + 1;
                    }
                }
            }
        }
    }
    return at;
}

static void call_display_list(const void* list, u32 nbytes) {
    const uint8_t* dl = (const uint8_t*)list;
    uint32_t at = 0;
    gx_vtx_flush();
    while (at < nbytes) {
        uint8_t cmd = dl[at];
        if (cmd == 0x00 || cmd == 0x48) { at++; continue; }                 /* NOP, INVL_VC */
        if (cmd == 0x08) { at += 6; continue; }                             /* LOAD_CP_REG */
        if (cmd == 0x10) {                                                  /* LOAD_XF_REG */
            uint32_t n = at + 5 <= nbytes ? (uint32_t)gx_be16(dl + at + 1) + 1 : 0;
            at += 5 + n * 4;
            continue;
        }
        if ((cmd & 0xE7) == 0x20) { at += 5; continue; }                    /* LOAD_INDX_A..D */
        if (cmd == 0x40) { at += 9; continue; }                             /* CALL_DL (nested: unsupported) */
        if (cmd == 0x44) { at++; continue; }
        if (cmd == 0x61) { at += 5; continue; }                             /* LOAD_BP_REG */
        if (cmd >= 0x80 && cmd < 0xC0 && at + 3 <= nbytes) {
            uint32_t prim = cmd & 0xF8, fmt = cmd & 7, n = gx_be16(dl + at + 1);
            uint32_t vbytes;
            at += 3;
            begin_batch(prim, (int)fmt, n);
            vbytes = dl_vertex_bytes(&B.plan);
            if (at + n * vbytes > nbytes || !B.base) {
                B.open = 0;
                return;
            }
            at = decode_verts(dl, at, &B.plan, n, B.base, NULL, NULL);
            B.done = n;
            end_batch();
            continue;
        }
        break;   /* unknown command: stop rather than misparse */
    }
}

/* ---- display-list cache ----
 * HSD's display lists are model data: the same list, with the same arrays,
 * is drawn every frame. The first call decodes it into a vertex buffer that
 * outlives the frame (xgx_vbuf_alloc); later calls replay the stored draws.
 * An entry is keyed by the list's address and size, and checked on every
 * call against a signature of the vertex descriptor, the formats the list
 * uses and the arrays it reads. At most once a frame a sampled hash of the
 * list and of the array ranges it indexed is compared too, because HSD
 * reuses memory and some arrays are rewritten (shape animation).
 *
 * A list whose arrays keep changing (skinned and morphed models: HSD
 * rewrites their positions and normals, or points them at another buffer,
 * every frame) goes dynamic: its decode plan, the indices of every vertex
 * and a decoded template in ordinary cached memory are kept. Each call
 * re-fetches only the attributes whose array moved or changed (a sampled
 * hash per array, per call; one that changed once is fetched every call
 * from then on), then copies the template into the vertex ring in one
 * sequential write. Before, such a list was parsed and fully decoded on
 * every call: ~200 a frame, most of the "dlist" time. A dynamic list that
 * doesn't fit the template budget is decoded every call as before
 * (volatile). */
#define DLC_MAX 2048
#define DLC_BUCKETS 4096
#define DLC_MAX_BATCH 64
#define DLC_MAX_RANGE 12
#define DLC_VOLATILE 4          /* rebuilds before a list goes dynamic */
#define DLC_DYN_BUDGET (1024u * 1024)   /* template + index bytes for all dynamic lists */

typedef struct {
    uint32_t prim, count, offset;   /* offset: bytes into the entry's buffer */
    XgxLayout layout;
} DlBatch;

typedef struct {
    const uint8_t* p;
    uint32_t bytes;
} DlRange;

/* one array a dynamic list reads */
typedef struct {
    uint8_t attr;                  /* array_attr() */
    uint8_t always;                /* seen changing: fetched every call */
    uint32_t lo, hi;               /* index range */
    const uint8_t* base;           /* g_gx.array[attr] when fetched */
    uint32_t hash;
} DynArray;

typedef struct {
    uint32_t prim, count, offset;  /* offset: bytes into the template */
    uint32_t idx;                  /* first index (uint16) of this batch */
    uint8_t ncol;                  /* indexed slots that land in the vertex */
    uint8_t col_slot[N_ORDER];     /* their plan slots */
    Plan plan;
} DynBatch;

typedef struct {
    DynBatch* batch;
    uint8_t* tmpl;
    uint16_t* idx;
    uint32_t bytes;                /* charged to the budget */
    uint32_t sig, dl_hash;
    DynArray arr[DLC_MAX_RANGE];
    uint8_t narr, nbatch;
} DynList;

typedef struct {
    const uint8_t* dl;
    uint32_t nbytes;
    uint32_t sig, hash;
    uint32_t checked, last_used;
    uint8_t* mem;
    uint32_t mem_bytes;
    DlBatch* batch;
    DynList* dyn;
    uint16_t nbatch;
    uint8_t fmts, nrange, rebuilds, is_volatile;
    DlRange range[DLC_MAX_RANGE];
    int next;                      /* bucket chain, -1: end */
} DlEntry;

static DlEntry s_dlc[DLC_MAX];
static int s_dlc_n;
static int s_dlc_bucket[DLC_BUCKETS];
static int s_dlc_ready;
static uint32_t s_dyn_bytes;
static uint32_t s_st_dl_hits, s_st_dl_builds, s_st_dl_direct, s_st_dyn_calls, s_st_dyn_fetch, s_st_dyn_builds;
static uint32_t s_st_chg_sig, s_st_chg_data;   /* why cached lists were rebuilt: formats/arrays, or contents */

/* FNV-1a, 32-bit words (the tail a byte at a time) */
static uint32_t fnv(uint32_t h, const void* p, uint32_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (; n >= 4; n -= 4, b += 4) {
        uint32_t w;
        memcpy(&w, b, 4);
        h = (h ^ w) * 16777619u;
    }
    while (n--) h = (h ^ *b++) * 16777619u;
    return h;
}

/* 64 sampled words plus the tail, as the texture cache does */
static uint32_t sample_hash(uint32_t h, const uint8_t* p, uint32_t n) {
    uint32_t i, step;
    if (n < 256) return fnv(h, p, n);
    step = (n - 4) / 64;
    for (i = 0; i < 64; i++) {
        uint32_t w;
        memcpy(&w, p + i * step, 4);
        h = (h ^ w) * 16777619u;
    }
    return fnv(h, p + n - 4, 4);
}

/* the vertex descriptor and the formats `fmts` use; with_arrays: the array
 * bases too (a dynamic list takes moving arrays as they come) */
static uint32_t vtx_sig(uint32_t fmts, int with_arrays) {
    uint32_t h = fnv(2166136261u, g_gx.desc, sizeof g_gx.desc), f, a;
    for (f = 0; f < 8; f++)
        if (fmts & (1u << f)) h = fnv(h, g_gx.vat[f], sizeof g_gx.vat[f]);
    for (a = 0; a < GX_VA_MAX_ATTR; a++)
        if (g_gx.desc[a] == GX_INDEX8 || g_gx.desc[a] == GX_INDEX16) {
            if (with_arrays) h = fnv(h, &g_gx.array[a], sizeof g_gx.array[a]);
            h = fnv(h, &g_gx.array_stride[a], sizeof g_gx.array_stride[a]);
            h = (h ^ g_gx.array_le[a]) * 16777619u;
        }
    return h;
}

static uint32_t content_hash(const DlEntry* e) {
    uint32_t h = sample_hash(2166136261u, e->dl, e->nbytes);
    int i;
    for (i = 0; i < e->nrange; i++) h = sample_hash(h, e->range[i].p, e->range[i].bytes);
    return h;
}

static uint32_t dl_bucket(const void* dl) { return ((uint32_t)(uintptr_t)dl >> 5) % DLC_BUCKETS; }

static void dlc_unlink(int idx) {
    int* link = &s_dlc_bucket[dl_bucket(s_dlc[idx].dl)];
    while (*link >= 0 && *link != idx) link = &s_dlc[*link].next;
    if (*link == idx) *link = s_dlc[idx].next;
}

static void dyn_free(DlEntry* e) {
    DynList* d = e->dyn;
    if (!d) return;
    s_dyn_bytes -= d->bytes;
    free(d->batch);
    free(d->tmpl);
    free(d->idx);
    free(d);
    e->dyn = NULL;
}

static void dlc_release(DlEntry* e) {
    xgx_vbuf_free(e->mem);
    free(e->batch);
    e->mem = NULL;
    e->batch = NULL;
    e->mem_bytes = 0;
    e->nbatch = 0;
    dyn_free(e);
}

/* entries stay in their slot; freed slots go on a stack */
static int s_dlc_free[DLC_MAX];
static int s_dlc_nfree;

static void dlc_drop(int idx) {
    dlc_unlink(idx);
    dlc_release(&s_dlc[idx]);
    s_dlc[idx].dl = NULL;
    s_dlc_free[s_dlc_nfree++] = idx;
    s_dlc_n--;
}

/* evicts the least recently used entry not drawn this frame, other than
 * `keep`; 0: none */
static int dlc_evict_one(uint32_t frame, const DlEntry* keep) {
    int i, pick = -1;
    for (i = 0; i < DLC_MAX; i++)
        if (s_dlc[i].dl && &s_dlc[i] != keep && s_dlc[i].last_used != frame &&
            (pick < 0 || s_dlc[i].last_used < s_dlc[pick].last_used))
            pick = i;
    if (pick < 0) return 0;
    dlc_drop(pick);
    return 1;
}

static DlEntry* dlc_find(const uint8_t* dl, uint32_t nbytes) {
    int i;
    for (i = s_dlc_bucket[dl_bucket(dl)]; i >= 0; i = s_dlc[i].next)
        if (s_dlc[i].dl == dl && s_dlc[i].nbytes == nbytes) return &s_dlc[i];
    return NULL;
}

/* skips a non-draw command at `at`; 0: not one this cache knows */
static uint32_t dl_skip(const uint8_t* dl, uint32_t at, uint32_t nbytes) {
    uint8_t cmd = dl[at];
    if (cmd == 0x00 || cmd == 0x48 || cmd == 0x44) return at + 1;
    if (cmd == 0x08) return at + 6;
    if (cmd == 0x10) return at + 5 + (at + 5 <= nbytes ? ((uint32_t)gx_be16(dl + at + 1) + 1) * 4 : 0);
    if ((cmd & 0xE7) == 0x20 || cmd == 0x61) return at + 5;
    if (cmd == 0x40) return at + 9;
    return 0;
}

/* pass 1 over a list: its draws and their layouts; 0 when it can't be cached */
static int dl_scan(const uint8_t* dl, uint32_t nbytes, DlBatch* batch, uint32_t* total, uint8_t* fmts) {
    Plan plan;
    uint32_t at = 0, next;
    int nb = 0;
    *total = 0;
    *fmts = 0;
    while (at < nbytes) {
        uint8_t cmd = dl[at];
        if (cmd >= 0x80 && cmd < 0xC0 && at + 3 <= nbytes) {
            uint32_t n = gx_be16(dl + at + 1), fmt = cmd & 7, vbytes;
            if (nb == DLC_MAX_BATCH) return 0;
            make_plan(&plan, (int)fmt);
            vbytes = dl_vertex_bytes(&plan);
            at += 3;
            if (at + n * vbytes > nbytes) return 0;
            batch[nb].prim = cmd & 0xF8;
            batch[nb].count = n;
            batch[nb].offset = *total;
            batch[nb].layout = plan.layout;
            *total += (n * plan.layout.stride + 15) & ~15u;
            *fmts |= (uint8_t)(1u << fmt);
            at += n * vbytes;
            nb++;
            continue;
        }
        if (!(next = dl_skip(dl, at, nbytes))) break;
        at = next;
    }
    return *total ? nb : 0;
}

static void range_reset(IdxRange* r) {
    uint32_t a;
    for (a = 0; a < GX_VA_MAX_ATTR; a++) {
        r->lo[a] = 0xFFFFFFFFu;
        r->hi[a] = 0;
    }
}

/* pass 2: decode the draws found by dl_scan into out (template or buffer) */
static void dl_decode_all(const uint8_t* dl, uint32_t nbytes, const DlBatch* batch, int nb, uint8_t* out, IdxRange* r,
                          uint16_t* idx, const uint32_t* idx_first) {
    Plan plan;
    uint32_t at = 0;
    int i;
    for (i = 0; i < nb && at < nbytes;) {
        uint8_t cmd = dl[at];
        if (cmd >= 0x80 && cmd < 0xC0) {
            make_plan(&plan, cmd & 7);
            at = decode_verts(dl, at + 3, &plan, batch[i].count, out + batch[i].offset, r,
                              idx ? idx + idx_first[i] : NULL);
            i++;
            continue;
        }
        at = dl_skip(dl, at, nbytes);
        if (!at) break;
    }
}

/* decode the whole list into one buffer; 0 when it can't be cached */
static int dlc_build(DlEntry* e, uint32_t frame) {
    static IdxRange r;
    DlBatch batch[DLC_MAX_BATCH];
    uint32_t total, a;
    uint8_t fmts;
    int nb = dl_scan(e->dl, e->nbytes, batch, &total, &fmts);
    if (!nb) return 0;
    e->mem = (uint8_t*)xgx_vbuf_alloc(total);
    while (!e->mem && dlc_evict_one(frame, e)) e->mem = (uint8_t*)xgx_vbuf_alloc(total);
    if (!e->mem) return 0;
    e->batch = (DlBatch*)malloc(sizeof(DlBatch) * (size_t)nb);
    if (!e->batch) {
        dlc_release(e);
        return 0;
    }
    range_reset(&r);
    dl_decode_all(e->dl, e->nbytes, batch, nb, e->mem, &r, NULL, NULL);
    memcpy(e->batch, batch, sizeof(DlBatch) * (size_t)nb);
    e->nbatch = (uint16_t)nb;
    e->mem_bytes = total;
    e->fmts = fmts;
    e->nrange = 0;
    for (a = 0; a < GX_VA_MAX_ATTR; a++)
        if (r.hi[a] > r.lo[a] && g_gx.array[a] && e->nrange < DLC_MAX_RANGE) {
            e->range[e->nrange].p = g_gx.array[a] + r.lo[a] * g_gx.array_stride[a];
            e->range[e->nrange].bytes = (r.hi[a] - r.lo[a]) * g_gx.array_stride[a];
            e->nrange++;
        }
    e->sig = vtx_sig(fmts, 1);
    e->hash = content_hash(e);
    e->checked = frame;
    s_st_dl_builds++;
    return 1;
}

static uint32_t dyn_array_hash(const DynArray* a, const uint8_t* base) {
    uint32_t stride = g_gx.array_stride[a->attr];
    return sample_hash(2166136261u, base + a->lo * stride, (a->hi - a->lo) * stride);
}

/* the dynamic form of e (see the comment above DLC_MAX); 0: over budget or
 * not cacheable */
static int dyn_build(DlEntry* e, uint32_t frame) {
    static IdxRange r;
    DlBatch batch[DLC_MAX_BATCH];
    uint32_t idx_first[DLC_MAX_BATCH], total, nidx = 0, a, bytes;
    uint8_t fmts;
    DynList* d;
    int nb = dl_scan(e->dl, e->nbytes, batch, &total, &fmts), i, k;
    if (!nb) return 0;
    d = (DynList*)calloc(1, sizeof *d);
    if (!d) return 0;
    d->batch = (DynBatch*)calloc((size_t)nb, sizeof(DynBatch));
    if (!d->batch) {
        free(d);
        return 0;
    }
    for (i = 0; i < nb; i++) {
        d->batch[i].prim = batch[i].prim;
        d->batch[i].count = batch[i].count;
        d->batch[i].offset = batch[i].offset;
    }
    /* the plans (dl_scan keeps only the layouts) and the indexed columns */
    {
        uint32_t at = 0;
        for (i = 0; i < nb && at < e->nbytes;) {
            uint8_t cmd = e->dl[at];
            if (cmd >= 0x80 && cmd < 0xC0) {
                DynBatch* b = &d->batch[i];
                make_plan(&b->plan, cmd & 7);
                for (k = 0; k < b->plan.n; k++) {
                    const Slot* sl = &b->plan.slot[k];
                    if (sl->type != GX_DIRECT && sl->dst >= 0) b->col_slot[b->ncol++] = (uint8_t)k;
                }
                b->idx = idx_first[i] = nidx;
                nidx += b->count * b->ncol;
                at += 3 + b->count * dl_vertex_bytes(&b->plan);
                i++;
                continue;
            }
            at = dl_skip(e->dl, at, e->nbytes);
            if (!at) break;
        }
        if (i != nb) {
            free(d->batch);
            free(d);
            return 0;
        }
    }
    bytes = total + nidx * 2 + (uint32_t)nb * sizeof(DynBatch);
    if (s_dyn_bytes + bytes > DLC_DYN_BUDGET) {
        free(d->batch);
        free(d);
        return 0;
    }
    d->tmpl = (uint8_t*)malloc(total);
    d->idx = (uint16_t*)malloc(nidx ? nidx * 2 : 2);
    if (!d->tmpl || !d->idx) {
        free(d->tmpl);
        free(d->idx);
        free(d->batch);
        free(d);
        return 0;
    }
    range_reset(&r);
    dl_decode_all(e->dl, e->nbytes, batch, nb, d->tmpl, &r, d->idx, idx_first);
    d->nbatch = (uint8_t)nb;
    d->bytes = bytes;
    s_dyn_bytes += bytes;
    for (a = 0; a < GX_VA_MAX_ATTR; a++)
        if (r.hi[a] > r.lo[a] && g_gx.array[a] && d->narr < DLC_MAX_RANGE) {
            DynArray* x = &d->arr[d->narr++];
            x->attr = (uint8_t)a;
            x->lo = r.lo[a];
            x->hi = r.hi[a];
            x->base = g_gx.array[a];
            x->hash = dyn_array_hash(x, x->base);
        }
    d->sig = vtx_sig(fmts, 0);
    d->dl_hash = sample_hash(2166136261u, e->dl, e->nbytes);
    e->dyn = d;
    e->fmts = fmts;
    e->checked = frame;
    s_st_dyn_builds++;
    return 1;
}

/* fetch attribute array `attr` again for every vertex of the template */
static void dyn_fetch(DynList* d, uint8_t attr, const uint8_t* base) {
    uint32_t stride = g_gx.array_stride[attr];
    int be = !g_gx.array_le[attr], i, c;
    for (i = 0; i < d->nbatch; i++) {
        const DynBatch* b = &d->batch[i];
        for (c = 0; c < b->ncol; c++) {
            const Slot* sl = &b->plan.slot[b->col_slot[c]];
            const uint16_t* idx = d->idx + b->idx + c;
            uint8_t* out = d->tmpl + b->offset;
            uint32_t v, vs = b->plan.layout.stride;
            if (array_attr(sl) != attr) continue;
            for (v = 0; v < b->count; v++, idx += b->ncol, out += vs) store(sl, base + *idx * stride, be, out);
        }
    }
    s_st_dyn_fetch++;
}

/* 1: drawn from the dynamic template */
static int dyn_call(DlEntry* e, uint32_t frame) {
    DynList* d = e->dyn;
    int i;
    if (d->sig != vtx_sig(e->fmts, 0) ||
        (e->checked != frame && (e->checked = frame, d->dl_hash != sample_hash(2166136261u, e->dl, e->nbytes)))) {
        dyn_free(e);
        if (++e->rebuilds >= DLC_VOLATILE * 4 || !dyn_build(e, frame)) {
            e->is_volatile = 1;
            return 0;
        }
        d = e->dyn;
    }
    for (i = 0; i < d->narr; i++) {
        DynArray* x = &d->arr[i];
        const uint8_t* base = g_gx.array[x->attr];
        if (!base) continue;
        if (!x->always) {
            uint32_t h;
            if (base == x->base && (h = dyn_array_hash(x, base)) == x->hash) continue;
            x->always = 1;
        }
        x->base = base;
        dyn_fetch(d, x->attr, base);
    }
    for (i = 0; i < d->nbatch; i++) {
        const DynBatch* b = &d->batch[i];
        uint32_t bytes = b->count * b->plan.layout.stride;
        uint8_t* v;
        if (!b->count || !(v = (uint8_t*)xgx_vtx_alloc(b->count, b->plan.layout.stride))) continue;
        memcpy(v, d->tmpl + b->offset, bytes);
        xgx_draw(b->prim, b->count, &b->plan.layout, &g_xgx);
        g_xgx.dirty = 0;
    }
    e->last_used = frame;
    s_st_dyn_calls++;
    return 1;
}

/* 1: drawn from the cache */
static int dlc_call(const uint8_t* dl, uint32_t nbytes) {
    uint32_t frame = xgx_present_count();
    DlEntry* e;
    int i;
    if (!s_dlc_ready) {
        memset(s_dlc_bucket, 0xFF, sizeof s_dlc_bucket);
        for (i = 0; i < DLC_MAX; i++) s_dlc_free[i] = DLC_MAX - 1 - i;
        s_dlc_nfree = DLC_MAX;
        s_dlc_ready = 1;
    }
    e = dlc_find(dl, nbytes);
    if (e && e->dyn) return dyn_call(e, frame);
    if (e && e->is_volatile) return 0;
    if (e && e->mem && (e->sig != vtx_sig(e->fmts, 1) ||
                        (e->checked != frame && (e->checked = frame, e->hash != content_hash(e))))) {
        if (e->sig != vtx_sig(e->fmts, 1)) s_st_chg_sig++;
        else s_st_chg_data++;
        dlc_release(e);   /* changed: rebuild below */
        if (++e->rebuilds >= DLC_VOLATILE) {
            if (dyn_build(e, frame)) return dyn_call(e, frame);
            e->is_volatile = 1;
            return 0;
        }
    }
    if (!e) {
        int idx;
        if (!s_dlc_nfree && !dlc_evict_one(frame, NULL)) return 0;
        idx = s_dlc_free[--s_dlc_nfree];
        e = &s_dlc[idx];
        memset(e, 0, sizeof *e);
        e->dl = dl;
        e->nbytes = nbytes;
        e->next = s_dlc_bucket[dl_bucket(dl)];
        s_dlc_bucket[dl_bucket(dl)] = idx;
        s_dlc_n++;
    }
    if (!e->mem && !dlc_build(e, frame)) return 0;
    e->last_used = frame;
    s_st_dl_hits++;
    for (i = 0; i < e->nbatch; i++) {
        const DlBatch* b = &e->batch[i];
        if (!b->count) continue;
        xgx_vtx_use(e->mem + b->offset);
        xgx_draw(b->prim, b->count, &b->layout, &g_xgx);
        g_xgx.dirty = 0;
    }
    return 1;
}

void gx_vtx_frame_end(void) {
    if (xgx_present_count() % XGX_STATS_EVERY == 0 && s_dlc_ready) {
        int i, vol = 0, dyn = 0;
        for (i = 0; i < DLC_MAX; i++) {
            vol += s_dlc[i].dl && s_dlc[i].is_volatile;
            dyn += s_dlc[i].dl && s_dlc[i].dyn;
        }
        xhw_logf("[DLC] %d of %d lists (%d dynamic, %u KB; %d volatile), vertex pool %u of %u KB free | per %u: %u "
                 "cached calls, %u builds (%u after a format/array change, %u after a content change), %u dynamic "
                 "calls (%u array fetches, %u builds), %u decoded",
                 s_dlc_n, DLC_MAX, dyn, s_dyn_bytes / 1024, vol, xgx_vbuf_pool_free_kb(), xgx_vbuf_pool_kb(),
                 XGX_STATS_EVERY, s_st_dl_hits, s_st_dl_builds, s_st_chg_sig, s_st_chg_data, s_st_dyn_calls,
                 s_st_dyn_fetch, s_st_dyn_builds, s_st_dl_direct);
        s_st_dl_hits = s_st_dl_builds = s_st_dl_direct = s_st_dyn_calls = s_st_dyn_fetch = s_st_dyn_builds = 0;
        s_st_chg_sig = s_st_chg_data = 0;
    }
}

void GXCallDisplayList(const void* list, u32 nbytes) {
    int pf = xhw_perf_enter(XHW_PERF_DLIST);
    gx_vtx_flush();
    if (!list || !nbytes || !dlc_call((const uint8_t*)list, nbytes)) {
        s_st_dl_direct++;
        call_display_list(list, nbytes);
    }
    xhw_perf_leave(pf);
}
