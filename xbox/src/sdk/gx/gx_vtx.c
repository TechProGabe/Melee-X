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
#include <string.h>

#include "gx_internal.h"
#include "xhw.h"

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
        memcpy(v + s->dst, out, s->attr == GX_VA_POS || s->attr == GX_VA_NRM ? 12 : 8);
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
        memcpy(B.cur + s->dst, out, attr == GX_VA_POS || attr == GX_VA_NRM ? 12 : 8);
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
void GXPosition2f32(f32 x, f32 y) { float f[3] = { x, y, 0 }; imm_floats(GX_VA_POS, f, 3); }
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

void GXCallDisplayList(const void* list, u32 nbytes) {
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
            uint32_t prim = cmd & 0xF8, fmt = cmd & 7, n = gx_be16(dl + at + 1), v;
            uint32_t vbytes;
            at += 3;
            begin_batch(prim, (int)fmt, n);
            vbytes = dl_vertex_bytes(&B.plan);
            if (at + n * vbytes > nbytes || !B.base) {
                B.open = 0;
                return;
            }
            for (v = 0; v < n; v++) {
                int i;
                uint8_t* out = B.base + v * B.plan.layout.stride;
                memset(out, 0, B.plan.layout.stride);
                for (i = 0; i < B.plan.n; i++) {
                    const Slot* s = &B.plan.slot[i];
                    if (s->type == GX_DIRECT) {
                        store(s, dl + at, 1, out);
                        at += (uint32_t)data_size(s);
                    } else {
                        int k, idx_count = s->attr == GX_VA_NRM && s->fmt.cnt == GX_NRM_NBT3 ? 3 : 1;
                        for (k = 0; k < idx_count; k++) {
                            uint32_t idx = s->type == GX_INDEX8 ? dl[at] : gx_be16(dl + at);
                            at += s->type == GX_INDEX8 ? 1 : 2;
                            if (k == 0) fetch_indexed(s, idx, out);
                        }
                    }
                }
            }
            B.done = n;
            end_batch();
            continue;
        }
        break;   /* unknown command: stop rather than misparse */
    }
}
