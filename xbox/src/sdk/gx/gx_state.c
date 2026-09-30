/* gx_state.c - GX state calls: they update g_xgx (xgx.h), which the NV2A
 * back end reads at the next draw. A pending GXBegin batch is flushed
 * before any state it depends on changes. */
#include <math.h>
#include <string.h>

#include "pc/pc.h"
#include "gx_internal.h"
#include "xhw.h"

XgxState g_xgx;
GxFront g_gx;

static GXDrawDoneCallback s_draw_done_cb;

#define DIRTY(bits) (g_xgx.dirty |= (bits))
#define FLUSH() gx_vtx_flush()
/* HSD sets the whole TEV, channel and pixel state again for every material
 * and polygon object, mostly to what it already is. A setter that changes
 * nothing returns before flushing or marking its group dirty, so the back
 * end doesn't rebuild and re-hash the combiner setup, the vertex-program
 * key and the texture units for each of ~2700 draws a frame. */
#define SAME4(a, x0, x1, x2, x3) ((a)[0] == (x0) && (a)[1] == (x1) && (a)[2] == (x2) && (a)[3] == (x3))

static void identity34(float m[3][4]) {
    memset(m, 0, sizeof(float) * 12);
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
}

GXFifoObj* GXInit(void* base, u32 size) {
    static GXFifoObj s_fifo;
    int i;
    (void)base;
    (void)size;
    memset(&g_xgx, 0, sizeof g_xgx);
    memset(&g_gx, 0, sizeof g_gx);
    for (i = 0; i < XGX_NUM_POSMTX; i++) {
        identity34(g_xgx.posmtx[i]);
        g_xgx.nrmmtx[i][0][0] = g_xgx.nrmmtx[i][1][1] = g_xgx.nrmmtx[i][2][2] = 1.0f;
    }
    for (i = 0; i < XGX_NUM_TEXMTX; i++) identity34(g_xgx.texmtx[i]);
    for (i = 0; i < XGX_NUM_PTMTX; i++) identity34(g_xgx.ptmtx[i]);
    for (i = 0; i < XGX_MAX_TEXGEN; i++) {
        g_xgx.texgen[i].type = GX_TG_MTX2x4;
        g_xgx.texgen[i].src = GX_TG_TEX0 + i;
        g_xgx.texgen[i].mtx = GX_IDENTITY;
        g_xgx.texgen[i].pt_mtx = GX_PTIDENTITY;
    }
    for (i = 0; i < XGX_MAX_TEV; i++) {
        XgxTevStage* t = &g_xgx.tev[i];
        t->cin[0] = GX_CC_ZERO; t->cin[1] = GX_CC_ZERO; t->cin[2] = GX_CC_ZERO; t->cin[3] = GX_CC_RASC;
        t->ain[0] = GX_CA_ZERO; t->ain[1] = GX_CA_ZERO; t->ain[2] = GX_CA_ZERO; t->ain[3] = GX_CA_RASA;
        t->cclamp = t->aclamp = 1;
        t->texcoord = GX_TEXCOORD_NULL;
        t->texmap = GX_TEXMAP_NULL;
        t->chan = GX_COLOR0A0;
        t->kcsel = GX_TEV_KCSEL_1;
        t->kasel = GX_TEV_KASEL_1;
    }
    for (i = 0; i < 4; i++) {
        g_xgx.swap[i][0] = 0; g_xgx.swap[i][1] = 1; g_xgx.swap[i][2] = 2; g_xgx.swap[i][3] = 3;
    }
    g_xgx.ntev = 1;
    g_xgx.nchans = 1;
    g_xgx.alpha_comp0 = g_xgx.alpha_comp1 = GX_ALWAYS;
    g_xgx.z_enable = 1;
    g_xgx.z_func = GX_LEQUAL;
    g_xgx.z_update = 1;
    g_xgx.color_update = 1;
    g_xgx.cull = GX_CULL_BACK;
    g_xgx.blend_src = GX_BL_ONE;
    g_xgx.dither = 1;
    g_xgx.viewport[2] = XGX_EFB_W;
    g_xgx.viewport[3] = XGX_EFB_H;
    g_xgx.viewport[5] = 1.0f;
    g_xgx.scissor[2] = XGX_EFB_W;
    g_xgx.scissor[3] = XGX_EFB_H;
    g_xgx.dirty = XGX_DIRTY_ALL;
    g_xgx.posmtx_mask = (1u << XGX_NUM_POSMTX) - 1;
    g_gx.clear_z = 0xFFFFFF;
    gx_tex_init();
    gx_vtx_reset();
    xgx_init();
    return &s_fifo;
}

/* ---- FIFO / sync: everything is immediate here ---- */
/* HSD waits for the draw-done callback (HSD_VIDrawDoneXFB) after each frame;
 * everything is already submitted here, so it fires at once */
void GXSetDrawDone(void) {
    FLUSH();
    if (s_draw_done_cb) s_draw_done_cb();
}
void GXWaitDrawDone(void) { FLUSH(); }
void GXDrawDone(void) { FLUSH(); }
void GXFlush(void) { FLUSH(); }
void GXPixModeSync(void) {}
void GXInvalidateVtxCache(void) {}
void GXInsertDebugMarker(const char* label) { (void)label; }
void GXSetMisc(GXMiscToken token, u32 val) { (void)token; (void)val; }
void GXSetDrawSync(u16 token) { (void)token; }
GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback cb) {
    GXDrawDoneCallback old = s_draw_done_cb;
    s_draw_done_cb = cb;
    return old;
}
/* HSD's XFB bookkeeping (HSD_VIDrawDoneXFB) waits for this after each frame */
void xsdk_gx_draw_done(void) {
    if (s_draw_done_cb) s_draw_done_cb();
}

/* ---- transform ---- */
/* The hardware keeps six terms and rebuilds the rest from the type, so a
 * matrix passed with the "wrong" type (sislib's screen text: MTXOrtho with
 * GX_PERSPECTIVE) must be read the way the GameCube reads it. */
void GXSetProjection(const void* mtx, GXProjectionType type) {
    const float(*m)[4] = (const float(*)[4])mtx;
    float p[4][4];
    memset(p, 0, sizeof p);
    p[0][0] = m[0][0];
    p[1][1] = m[1][1];
    p[2][2] = m[2][2];
    p[2][3] = m[2][3];
    if (type == GX_ORTHOGRAPHIC) {
        p[0][3] = m[0][3];
        p[1][3] = m[1][3];
        p[3][3] = 1.0f;
    } else {
        p[0][2] = m[0][2];
        p[1][2] = m[1][2];
        p[3][2] = -1.0f;
    }
    if (g_xgx.proj_ortho == (type == GX_ORTHOGRAPHIC) && memcmp(g_xgx.proj, p, sizeof p) == 0) return;
    FLUSH();
    memcpy(g_xgx.proj, p, sizeof p);
    g_xgx.proj_ortho = type == GX_ORTHOGRAPHIC;
    DIRTY(XGX_DIRTY_PROJ);
}

void GXGetProjectionv(f32* p) {
    const float(*m)[4] = g_xgx.proj;
    p[0] = g_xgx.proj_ortho ? 1.0f : 0.0f;
    p[1] = m[0][0];
    p[2] = g_xgx.proj_ortho ? m[0][3] : m[0][2];
    p[3] = m[1][1];
    p[4] = g_xgx.proj_ortho ? m[1][3] : m[1][2];
    p[5] = m[2][2];
    p[6] = m[2][3];
}

void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    const float* v = g_xgx.viewport;
    if (v[0] == left && v[1] == top && v[2] == wd && v[3] == ht && v[4] == nearz && v[5] == farz) return;
    FLUSH();
    g_xgx.viewport[0] = left;
    g_xgx.viewport[1] = top;
    g_xgx.viewport[2] = wd;
    g_xgx.viewport[3] = ht;
    g_xgx.viewport[4] = nearz;
    g_xgx.viewport[5] = farz;
    DIRTY(XGX_DIRTY_VIEWPORT);
}

void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field) {
    (void)field;
    GXSetViewport(left, top, wd, ht, nearz, farz);
}

void GXGetViewportv(f32* vp) { memcpy(vp, g_xgx.viewport, sizeof g_xgx.viewport); }

void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) {
    if (SAME4(g_xgx.scissor, (int32_t)left, (int32_t)top, (int32_t)wd, (int32_t)ht)) return;
    FLUSH();
    g_xgx.scissor[0] = (int32_t)left;
    g_xgx.scissor[1] = (int32_t)top;
    g_xgx.scissor[2] = (int32_t)wd;
    g_xgx.scissor[3] = (int32_t)ht;
    DIRTY(XGX_DIRTY_SCISSOR);
}

void GXGetScissor(u32* left, u32* top, u32* wd, u32* ht) {
    *left = (u32)g_xgx.scissor[0];
    *top = (u32)g_xgx.scissor[1];
    *wd = (u32)g_xgx.scissor[2];
    *ht = (u32)g_xgx.scissor[3];
}

void GXSetScissorBoxOffset(s32 x, s32 y) { (void)x; (void)y; }
void GXSetClipMode(GXClipMode mode) { (void)mode; }

void GXLoadPosMtxImm(const void* mtx, u32 id) {
    u32 k = id / 3;
    if (k >= XGX_NUM_POSMTX) return;
    if (memcmp(g_xgx.posmtx[k], mtx, sizeof g_xgx.posmtx[k]) == 0) return;
    FLUSH();
    memcpy(g_xgx.posmtx[k], mtx, sizeof g_xgx.posmtx[k]);
    g_xgx.posmtx_mask |= 1u << k;
    DIRTY(XGX_DIRTY_POSMTX);
}

void GXLoadNrmMtxImm(const void* mtx, u32 id) {
    const float(*m)[4] = (const float(*)[4])mtx;
    u32 k = id / 3, r;
    if (k >= XGX_NUM_POSMTX) return;
    for (r = 0; r < 3; r++)
        if (g_xgx.nrmmtx[k][r][0] != m[r][0] || g_xgx.nrmmtx[k][r][1] != m[r][1] || g_xgx.nrmmtx[k][r][2] != m[r][2])
            break;
    if (r == 3) return;
    FLUSH();
    for (r = 0; r < 3; r++) {
        g_xgx.nrmmtx[k][r][0] = m[r][0];
        g_xgx.nrmmtx[k][r][1] = m[r][1];
        g_xgx.nrmmtx[k][r][2] = m[r][2];
    }
    g_xgx.posmtx_mask |= 1u << k;
    DIRTY(XGX_DIRTY_POSMTX);
}

void GXLoadNrmMtxImm3x3(const void* mtx, u32 id) {
    u32 k = id / 3;
    if (k >= XGX_NUM_POSMTX) return;
    FLUSH();
    memcpy(g_xgx.nrmmtx[k], mtx, sizeof g_xgx.nrmmtx[k]);
    g_xgx.posmtx_mask |= 1u << k;
    DIRTY(XGX_DIRTY_POSMTX);
}

void GXLoadTexMtxImm(const void* mtx, u32 id, GXTexMtxType type) {
    float(*dst)[4];
    FLUSH();
    if (id >= GX_PTTEXMTX0) {
        u32 k = (id - GX_PTTEXMTX0) / 3;
        if (k >= XGX_NUM_PTMTX) return;
        dst = g_xgx.ptmtx[k];
    } else if (id >= GX_TEXMTX0 && id < GX_IDENTITY) {
        dst = g_xgx.texmtx[(id - GX_TEXMTX0) / 3];
    } else if (id < GX_TEXMTX0) {
        dst = g_xgx.posmtx[id / 3];   /* texgens may read position matrices */
        g_xgx.posmtx_mask |= 1u << (id / 3);
    } else {
        return;
    }
    if (type == GX_MTX2x4) {
        memcpy(dst, mtx, sizeof(float) * 8);
        dst[2][0] = dst[2][1] = dst[2][3] = 0.0f;
        dst[2][2] = 1.0f;
    } else {
        memcpy(dst, mtx, sizeof(float) * 12);
    }
    DIRTY(XGX_DIRTY_TEXMTX | XGX_DIRTY_POSMTX);
}

void GXSetCurrentMtx(u32 id) {
    if (g_xgx.cur_posmtx == id) return;
    FLUSH();
    g_xgx.cur_posmtx = id;
    DIRTY(XGX_DIRTY_POSMTX);
}

void GXProject(f32 x, f32 y, f32 z, const f32 mtx[3][4], const f32* pm, const f32* vp, f32* sx, f32* sy, f32* sz) {
    float px = mtx[0][0] * x + mtx[0][1] * y + mtx[0][2] * z + mtx[0][3];
    float py = mtx[1][0] * x + mtx[1][1] * y + mtx[1][2] * z + mtx[1][3];
    float pz = mtx[2][0] * x + mtx[2][1] * y + mtx[2][2] * z + mtx[2][3];
    float xc, yc, zc, wc;
    if (pm[0] == 0.0f) {
        xc = px * pm[1] + pz * pm[2];
        yc = py * pm[3] + pz * pm[4];
        zc = pz * pm[5] + pm[6];
        wc = 1.0f / -pz;
    } else {
        xc = px * pm[1] + pm[2];
        yc = py * pm[3] + pm[4];
        zc = pz * pm[5] + pm[6];
        wc = 1.0f;
    }
    *sx = vp[2] / 2 * xc * wc + vp[0] + vp[2] / 2;
    *sy = -vp[3] / 2 * yc * wc + vp[1] + vp[3] / 2;
    *sz = vp[5] + (vp[5] - vp[4]) * zc * wc;
}

/* ---- lighting ---- */
typedef struct {
    float pos[3], dir[3], a[3], k[3];
    GXColor color;
} LightObj;
_Static_assert(sizeof(LightObj) <= sizeof(GXLightObj), "GXLightObj too small");

void GXInitLightPos(GXLightObj* lt, f32 x, f32 y, f32 z) {
    LightObj* l = (LightObj*)lt;
    l->pos[0] = x; l->pos[1] = y; l->pos[2] = z;
}
void GXInitLightDir(GXLightObj* lt, f32 nx, f32 ny, f32 nz) {
    LightObj* l = (LightObj*)lt;
    /* the SDK stores the negated direction */
    l->dir[0] = -nx; l->dir[1] = -ny; l->dir[2] = -nz;
}
void GXInitLightColor(GXLightObj* lt, GXColor color) { ((LightObj*)lt)->color = color; }
void GXInitLightAttn(GXLightObj* lt, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
    LightObj* l = (LightObj*)lt;
    l->a[0] = a0; l->a[1] = a1; l->a[2] = a2;
    l->k[0] = k0; l->k[1] = k1; l->k[2] = k2;
}
void GXInitLightAttnA(GXLightObj* lt, f32 a0, f32 a1, f32 a2) {
    LightObj* l = (LightObj*)lt;
    l->a[0] = a0; l->a[1] = a1; l->a[2] = a2;
}
void GXInitLightAttnK(GXLightObj* lt, f32 k0, f32 k1, f32 k2) {
    LightObj* l = (LightObj*)lt;
    l->k[0] = k0; l->k[1] = k1; l->k[2] = k2;
}

/* SDK GXInitLightSpot */
void GXInitLightSpot(GXLightObj* lt, f32 cutoff, GXSpotFn fn) {
    float a0, a1, a2, r, d, cr;
    if (cutoff <= 0.0f || cutoff > 90.0f) fn = GX_SP_OFF;
    r = cutoff * 3.14159265f / 180.0f;
    cr = cosf(r);
    switch (fn) {
        case GX_SP_FLAT: a0 = -1000.0f * cr; a1 = 1000.0f; a2 = 0.0f; break;
        case GX_SP_COS: a0 = -cr / (1.0f - cr); a1 = 1.0f / (1.0f - cr); a2 = 0.0f; break;
        case GX_SP_COS2: a0 = 0.0f; a1 = -cr / (1.0f - cr); a2 = 1.0f / (1.0f - cr); break;
        case GX_SP_SHARP:
            d = (1.0f - cr) * (1.0f - cr);
            a0 = cr * (cr - 2.0f) / d; a1 = 2.0f / d; a2 = -1.0f / d;
            break;
        case GX_SP_RING1:
            d = (1.0f - cr) * (1.0f - cr);
            a0 = -4.0f * cr / d; a1 = 4.0f * (1.0f + cr) / d; a2 = -4.0f / d;
            break;
        case GX_SP_RING2:
            d = (1.0f - cr) * (1.0f - cr);
            a0 = 1.0f - 2.0f * cr * cr / d; a1 = 4.0f * cr / d; a2 = -2.0f / d;
            break;
        default: a0 = 1.0f; a1 = 0.0f; a2 = 0.0f; break;
    }
    GXInitLightAttnA(lt, a0, a1, a2);
}

/* SDK GXInitLightDistAttn */
void GXInitLightDistAttn(GXLightObj* lt, f32 ref_dist, f32 ref_bright, GXDistAttnFn fn) {
    float k0, k1, k2;
    if (ref_dist < 0.0f || ref_bright <= 0.0f || ref_bright >= 1.0f) fn = GX_DA_OFF;
    switch (fn) {
        case GX_DA_GENTLE: k0 = 1.0f; k1 = (1.0f - ref_bright) / (ref_bright * ref_dist); k2 = 0.0f; break;
        case GX_DA_MEDIUM:
            k0 = 1.0f;
            k1 = 0.5f * (1.0f - ref_bright) / (ref_bright * ref_dist);
            k2 = 0.5f * (1.0f - ref_bright) / (ref_bright * ref_dist * ref_dist);
            break;
        case GX_DA_STEEP: k0 = 1.0f; k1 = 0.0f; k2 = (1.0f - ref_bright) / (ref_bright * ref_dist * ref_dist); break;
        default: k0 = 1.0f; k1 = 0.0f; k2 = 0.0f; break;
    }
    GXInitLightAttnK(lt, k0, k1, k2);
}

void GXLoadLightObjImm(GXLightObj* lt, GXLightID light) {
    const LightObj* l = (const LightObj*)lt;
    int i = 0;
    while (i < 8 && !(light & (1u << i))) i++;
    if (i == 8) return;
    if (memcmp(g_xgx.light[i].pos, l->pos, sizeof l->pos) == 0 && memcmp(g_xgx.light[i].dir, l->dir, sizeof l->dir) == 0 &&
        memcmp(g_xgx.light[i].a, l->a, sizeof l->a) == 0 && memcmp(g_xgx.light[i].k, l->k, sizeof l->k) == 0 &&
        SAME4(g_xgx.light[i].color, l->color.r, l->color.g, l->color.b, l->color.a))
        return;
    FLUSH();
    memcpy(g_xgx.light[i].pos, l->pos, sizeof l->pos);
    memcpy(g_xgx.light[i].dir, l->dir, sizeof l->dir);
    memcpy(g_xgx.light[i].a, l->a, sizeof l->a);
    memcpy(g_xgx.light[i].k, l->k, sizeof l->k);
    g_xgx.light[i].color[0] = l->color.r;
    g_xgx.light[i].color[1] = l->color.g;
    g_xgx.light[i].color[2] = l->color.b;
    g_xgx.light[i].color[3] = l->color.a;
    DIRTY(XGX_DIRTY_LIGHTS);
}

void GXSetNumChans(u8 n) {
    if (g_xgx.nchans == n) return;
    FLUSH();
    g_xgx.nchans = n;
    DIRTY(XGX_DIRTY_CHANS);
}

void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src, u32 light_mask,
                   GXDiffuseFn diff_fn, GXAttnFn attn_fn) {
    XgxChan c;
    c.enable = enable;
    c.amb_src = amb_src;
    c.mat_src = mat_src;
    c.light_mask = light_mask;
    c.diff_fn = attn_fn == GX_AF_SPEC ? GX_DF_NONE : diff_fn;
    c.attn_fn = attn_fn;
    switch (chan) {
        case GX_COLOR0: case GX_COLOR0A0: if (memcmp(&g_xgx.chan[0], &c, sizeof c) != 0) break;
            if (chan == GX_COLOR0 || memcmp(&g_xgx.chan[1], &c, sizeof c) == 0) return;
            break;
        case GX_ALPHA0: if (memcmp(&g_xgx.chan[1], &c, sizeof c) == 0) return; break;
        case GX_COLOR1: case GX_COLOR1A1: if (memcmp(&g_xgx.chan[2], &c, sizeof c) != 0) break;
            if (chan == GX_COLOR1 || memcmp(&g_xgx.chan[3], &c, sizeof c) == 0) return;
            break;
        case GX_ALPHA1: if (memcmp(&g_xgx.chan[3], &c, sizeof c) == 0) return; break;
        default: break;
    }
    FLUSH();
    switch (chan) {
        case GX_COLOR0: g_xgx.chan[0] = c; break;
        case GX_ALPHA0: g_xgx.chan[1] = c; break;
        case GX_COLOR1: g_xgx.chan[2] = c; break;
        case GX_ALPHA1: g_xgx.chan[3] = c; break;
        case GX_COLOR0A0: g_xgx.chan[0] = g_xgx.chan[1] = c; break;
        case GX_COLOR1A1: g_xgx.chan[2] = g_xgx.chan[3] = c; break;
        default: break;
    }
    DIRTY(XGX_DIRTY_CHANS);
}

static void set_chan_color(uint8_t dst[2][4], GXChannelID chan, GXColor c) {
    switch (chan) {
        case GX_COLOR0: if (dst[0][0] == c.r && dst[0][1] == c.g && dst[0][2] == c.b) return; break;
        case GX_ALPHA0: if (dst[0][3] == c.a) return; break;
        case GX_COLOR1: if (dst[1][0] == c.r && dst[1][1] == c.g && dst[1][2] == c.b) return; break;
        case GX_ALPHA1: if (dst[1][3] == c.a) return; break;
        case GX_COLOR0A0: if (SAME4(dst[0], c.r, c.g, c.b, c.a)) return; break;
        case GX_COLOR1A1: if (SAME4(dst[1], c.r, c.g, c.b, c.a)) return; break;
        default: break;
    }
    FLUSH();
    switch (chan) {
        case GX_COLOR0: dst[0][0] = c.r; dst[0][1] = c.g; dst[0][2] = c.b; break;
        case GX_ALPHA0: dst[0][3] = c.a; break;
        case GX_COLOR1: dst[1][0] = c.r; dst[1][1] = c.g; dst[1][2] = c.b; break;
        case GX_ALPHA1: dst[1][3] = c.a; break;
        case GX_COLOR0A0: dst[0][0] = c.r; dst[0][1] = c.g; dst[0][2] = c.b; dst[0][3] = c.a; break;
        case GX_COLOR1A1: dst[1][0] = c.r; dst[1][1] = c.g; dst[1][2] = c.b; dst[1][3] = c.a; break;
        default: break;
    }
    DIRTY(XGX_DIRTY_CHANS);
}

void GXSetChanAmbColor(GXChannelID chan, GXColor c) { set_chan_color(g_xgx.amb, chan, c); }
void GXSetChanMatColor(GXChannelID chan, GXColor c) { set_chan_color(g_xgx.mat, chan, c); }

/* ---- texgen ---- */
void GXSetNumTexGens(u8 n) {
    if (g_xgx.ntexgen == n) return;
    FLUSH();
    g_xgx.ntexgen = n;
    DIRTY(XGX_DIRTY_TEXGEN);
}

void GXSetTexCoordGen2(GXTexCoordID dst, GXTexGenType func, GXTexGenSrc src, u32 mtx, GXBool normalize,
                       u32 postmtx) {
    XgxTexGen* t;
    if (dst >= XGX_MAX_TEXGEN) return;
    t = &g_xgx.texgen[dst];
    if (t->type == func && t->src == src && t->mtx == mtx && t->normalize == normalize && t->pt_mtx == postmtx) return;
    FLUSH();
    g_xgx.texgen[dst].type = func;
    g_xgx.texgen[dst].src = src;
    g_xgx.texgen[dst].mtx = mtx;
    g_xgx.texgen[dst].normalize = normalize;
    g_xgx.texgen[dst].pt_mtx = postmtx;
    DIRTY(XGX_DIRTY_TEXGEN);
}

/* ---- TEV ---- */
void GXSetNumTevStages(u8 n) {
    if (g_xgx.ntev == n) return;
    FLUSH();
    g_xgx.ntev = n;
    DIRTY(XGX_DIRTY_TEV);
}

void GXSetTevOp(GXTevStageID id, GXTevMode mode) {
    XgxTevStage stage = g_xgx.tev[id], *t = &stage;
    uint32_t carg = id == GX_TEVSTAGE0 ? GX_CC_RASC : GX_CC_CPREV;
    uint32_t aarg = id == GX_TEVSTAGE0 ? GX_CA_RASA : GX_CA_APREV;
    switch (mode) {
        case GX_MODULATE:
            t->cin[0] = GX_CC_ZERO; t->cin[1] = GX_CC_TEXC; t->cin[2] = carg; t->cin[3] = GX_CC_ZERO;
            t->ain[0] = GX_CA_ZERO; t->ain[1] = GX_CA_TEXA; t->ain[2] = aarg; t->ain[3] = GX_CA_ZERO;
            break;
        case GX_DECAL:
            t->cin[0] = carg; t->cin[1] = GX_CC_TEXC; t->cin[2] = GX_CC_TEXA; t->cin[3] = GX_CC_ZERO;
            t->ain[0] = GX_CA_ZERO; t->ain[1] = GX_CA_ZERO; t->ain[2] = GX_CA_ZERO; t->ain[3] = aarg;
            break;
        case GX_BLEND:
            t->cin[0] = carg; t->cin[1] = GX_CC_ONE; t->cin[2] = GX_CC_TEXC; t->cin[3] = GX_CC_ZERO;
            t->ain[0] = GX_CA_ZERO; t->ain[1] = GX_CA_TEXA; t->ain[2] = aarg; t->ain[3] = GX_CA_ZERO;
            break;
        case GX_REPLACE:
            t->cin[0] = GX_CC_ZERO; t->cin[1] = GX_CC_ZERO; t->cin[2] = GX_CC_ZERO; t->cin[3] = GX_CC_TEXC;
            t->ain[0] = GX_CA_ZERO; t->ain[1] = GX_CA_ZERO; t->ain[2] = GX_CA_ZERO; t->ain[3] = GX_CA_TEXA;
            break;
        default: /* GX_PASSCLR */
            t->cin[0] = GX_CC_ZERO; t->cin[1] = GX_CC_ZERO; t->cin[2] = GX_CC_ZERO; t->cin[3] = carg;
            t->ain[0] = GX_CA_ZERO; t->ain[1] = GX_CA_ZERO; t->ain[2] = GX_CA_ZERO; t->ain[3] = aarg;
            break;
    }
    t->cop = t->aop = GX_TEV_ADD;
    t->cbias = t->abias = GX_TB_ZERO;
    t->cscale = t->ascale = GX_CS_SCALE_1;
    t->cclamp = t->aclamp = 1;
    t->cout = t->aout = GX_TEVPREV;
    if (memcmp(&g_xgx.tev[id], t, sizeof *t) == 0) return;
    FLUSH();
    g_xgx.tev[id] = stage;
    DIRTY(XGX_DIRTY_TEV);
}

void GXSetTevColorIn(GXTevStageID s, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
    XgxTevStage* t = &g_xgx.tev[s];
    if (SAME4(t->cin, (uint32_t)a, (uint32_t)b, (uint32_t)c, (uint32_t)d)) return;
    FLUSH();
    t->cin[0] = a; t->cin[1] = b; t->cin[2] = c; t->cin[3] = d;
    DIRTY(XGX_DIRTY_TEV);
}

void GXSetTevAlphaIn(GXTevStageID s, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
    XgxTevStage* t = &g_xgx.tev[s];
    if (SAME4(t->ain, (uint32_t)a, (uint32_t)b, (uint32_t)c, (uint32_t)d)) return;
    FLUSH();
    t->ain[0] = a; t->ain[1] = b; t->ain[2] = c; t->ain[3] = d;
    DIRTY(XGX_DIRTY_TEV);
}

void GXSetTevColorOp(GXTevStageID s, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out) {
    XgxTevStage* t = &g_xgx.tev[s];
    if (t->cop == op && t->cbias == bias && t->cscale == scale && t->cclamp == clamp && t->cout == out) return;
    FLUSH();
    t->cop = op; t->cbias = bias; t->cscale = scale; t->cclamp = clamp; t->cout = out;
    DIRTY(XGX_DIRTY_TEV);
}

void GXSetTevAlphaOp(GXTevStageID s, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out) {
    XgxTevStage* t = &g_xgx.tev[s];
    if (t->aop == op && t->abias == bias && t->ascale == scale && t->aclamp == clamp && t->aout == out) return;
    FLUSH();
    t->aop = op; t->abias = bias; t->ascale = scale; t->aclamp = clamp; t->aout = out;
    DIRTY(XGX_DIRTY_TEV);
}

void GXSetTevClampMode(GXTevStageID s, GXTevClampMode mode) { (void)s; (void)mode; }

void GXSetTevOrder(GXTevStageID s, GXTexCoordID coord, GXTexMapID map, GXChannelID color) {
    XgxTevStage* t = &g_xgx.tev[s];
    if (t->texcoord == coord && t->texmap == map && t->chan == color) return;
    FLUSH();
    t->texcoord = coord;
    t->texmap = map;
    t->chan = color;
    DIRTY(XGX_DIRTY_TEV);
}

void GXSetTevColor(GXTevRegID id, GXColor c) {
    if (SAME4(g_xgx.tevreg[id], c.r, c.g, c.b, c.a)) return;
    FLUSH();
    g_xgx.tevreg[id][0] = c.r; g_xgx.tevreg[id][1] = c.g; g_xgx.tevreg[id][2] = c.b; g_xgx.tevreg[id][3] = c.a;
    DIRTY(XGX_DIRTY_TEVREG);
}

void GXSetTevColorS10(GXTevRegID id, GXColorS10 c) {
    if (SAME4(g_xgx.tevreg[id], c.r, c.g, c.b, c.a)) return;
    FLUSH();
    g_xgx.tevreg[id][0] = c.r; g_xgx.tevreg[id][1] = c.g; g_xgx.tevreg[id][2] = c.b; g_xgx.tevreg[id][3] = c.a;
    DIRTY(XGX_DIRTY_TEVREG);
}

void GXSetTevKColor(GXTevKColorID id, GXColor c) {
    if (SAME4(g_xgx.konst[id], c.r, c.g, c.b, c.a)) return;
    FLUSH();
    g_xgx.konst[id][0] = c.r; g_xgx.konst[id][1] = c.g; g_xgx.konst[id][2] = c.b; g_xgx.konst[id][3] = c.a;
    DIRTY(XGX_DIRTY_TEVREG);
}

void GXSetTevKColorSel(GXTevStageID s, GXTevKColorSel sel) {
    if (g_xgx.tev[s].kcsel == sel) return;
    FLUSH();
    g_xgx.tev[s].kcsel = sel;
    DIRTY(XGX_DIRTY_TEV);
}

void GXSetTevKAlphaSel(GXTevStageID s, GXTevKAlphaSel sel) {
    if (g_xgx.tev[s].kasel == sel) return;
    FLUSH();
    g_xgx.tev[s].kasel = sel;
    DIRTY(XGX_DIRTY_TEV);
}

void GXSetTevSwapMode(GXTevStageID s, GXTevSwapSel ras, GXTevSwapSel tex) {
    if (g_xgx.tev[s].ras_swap == ras && g_xgx.tev[s].tex_swap == tex) return;
    FLUSH();
    g_xgx.tev[s].ras_swap = ras;
    g_xgx.tev[s].tex_swap = tex;
    DIRTY(XGX_DIRTY_TEV);
}

void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan r, GXTevColorChan g, GXTevColorChan b,
                           GXTevColorChan a) {
    if (SAME4(g_xgx.swap[table], (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a)) return;
    FLUSH();
    g_xgx.swap[table][0] = (uint8_t)r;
    g_xgx.swap[table][1] = (uint8_t)g;
    g_xgx.swap[table][2] = (uint8_t)b;
    g_xgx.swap[table][3] = (uint8_t)a;
    DIRTY(XGX_DIRTY_TEV);
}

/* ---- indirect texturing (recorded; the back end approximates) ---- */
void GXSetNumIndStages(u8 n) {
    if (g_xgx.nind == n) return;
    FLUSH();
    g_xgx.nind = n;
    DIRTY(XGX_DIRTY_TEV);
}

void GXSetTevDirect(GXTevStageID s) {
    GXSetTevIndirect(s, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_NONE, GX_ITM_OFF, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE,
                     GX_FALSE, GX_ITBA_OFF);
}

void GXSetTevIndirect(GXTevStageID s, GXIndTexStageID ind, GXIndTexFormat fmt, GXIndTexBiasSel bias,
                      GXIndTexMtxID mtx, GXIndTexWrap ws, GXIndTexWrap wt, GXBool add_prev, GXBool lod,
                      GXIndTexAlphaSel alpha) {
    XgxTevStage* t = &g_xgx.tev[s];
    if (t->ind_stage == ind && t->ind_format == fmt && t->ind_bias == bias && t->ind_mtx == mtx &&
        t->ind_wrap_s == ws && t->ind_wrap_t == wt && t->ind_add_prev == add_prev && t->ind_utc_lod == lod &&
        t->ind_alpha == alpha)
        return;
    FLUSH();
    t->ind_stage = ind; t->ind_format = fmt; t->ind_bias = bias; t->ind_mtx = mtx;
    t->ind_wrap_s = ws; t->ind_wrap_t = wt; t->ind_add_prev = add_prev; t->ind_utc_lod = lod;
    t->ind_alpha = alpha;
    DIRTY(XGX_DIRTY_TEV);
}

void GXSetIndTexOrder(GXIndTexStageID s, GXTexCoordID coord, GXTexMapID map) {
    FLUSH();
    g_xgx.ind_order[s][0] = coord;
    g_xgx.ind_order[s][1] = map;
    DIRTY(XGX_DIRTY_TEV);
}

void GXSetIndTexCoordScale(GXIndTexStageID s, GXIndTexScale ss, GXIndTexScale st) {
    FLUSH();
    g_xgx.ind_scale[s][0] = ss;
    g_xgx.ind_scale[s][1] = st;
    DIRTY(XGX_DIRTY_TEV);
}

void GXSetIndTexMtx(GXIndTexMtxID id, const void* offset, s8 scale_exp) {
    const float(*m)[3] = (const float(*)[3])offset;
    int k = id - GX_ITM_0, r, c;
    float s = ldexpf(1.0f, scale_exp);
    if (k < 0 || k > 2) return;
    FLUSH();
    for (r = 0; r < 2; r++)
        for (c = 0; c < 3; c++) g_xgx.ind_mtx[k][r][c] = m[r][c] * s;
    DIRTY(XGX_DIRTY_TEV);
}

/* ---- pixel ---- */
void GXSetAlphaCompare(GXCompare c0, u8 r0, GXAlphaOp op, GXCompare c1, u8 r1) {
    if (g_xgx.alpha_comp0 == c0 && g_xgx.alpha_ref0 == r0 && g_xgx.alpha_op == op && g_xgx.alpha_comp1 == c1 &&
        g_xgx.alpha_ref1 == r1)
        return;
    FLUSH();
    g_xgx.alpha_comp0 = c0; g_xgx.alpha_ref0 = r0; g_xgx.alpha_op = op;
    g_xgx.alpha_comp1 = c1; g_xgx.alpha_ref1 = r1;
    DIRTY(XGX_DIRTY_PIXEL);
}

void GXSetBlendMode(GXBlendMode type, GXBlendFactor src, GXBlendFactor dst, GXLogicOp op) {
    if (g_xgx.blend_type == type && g_xgx.blend_src == src && g_xgx.blend_dst == dst && g_xgx.blend_logic == op) return;
    FLUSH();
    g_xgx.blend_type = type; g_xgx.blend_src = src; g_xgx.blend_dst = dst; g_xgx.blend_logic = op;
    DIRTY(XGX_DIRTY_PIXEL);
}

void GXSetZMode(GXBool enable, GXCompare func, GXBool update) {
    if (g_xgx.z_enable == enable && g_xgx.z_func == func && g_xgx.z_update == update) return;
    FLUSH();
    g_xgx.z_enable = enable; g_xgx.z_func = func; g_xgx.z_update = update;
    DIRTY(XGX_DIRTY_PIXEL);
}

void GXSetZCompLoc(GXBool before_tex) { (void)before_tex; }

void GXSetColorUpdate(GXBool on) {
    if (g_xgx.color_update == on) return;
    FLUSH();
    g_xgx.color_update = on;
    DIRTY(XGX_DIRTY_PIXEL);
}

void GXSetAlphaUpdate(GXBool on) {
    if (g_xgx.alpha_update == on) return;
    FLUSH();
    g_xgx.alpha_update = on;
    DIRTY(XGX_DIRTY_PIXEL);
}

void GXSetDstAlpha(GXBool enable, u8 alpha) {
    if (g_xgx.dst_alpha_enable == enable && g_xgx.dst_alpha == alpha) return;
    FLUSH();
    g_xgx.dst_alpha_enable = enable;
    g_xgx.dst_alpha = alpha;
    DIRTY(XGX_DIRTY_PIXEL);
}

void GXSetCullMode(GXCullMode mode) {
    if (g_xgx.cull == mode) return;
    FLUSH();
    g_xgx.cull = mode;
    DIRTY(XGX_DIRTY_PIXEL);
}

void GXSetDither(GXBool on) {
    if (g_xgx.dither == on) return;
    FLUSH();
    g_xgx.dither = on;
    DIRTY(XGX_DIRTY_PIXEL);
}

void GXSetPixelFmt(GXPixelFmt pix, GXZFmt16 z) { (void)pix; (void)z; }
void GXSetFieldMode(GXBool field_mode, GXBool half_aspect) { (void)field_mode; (void)half_aspect; }
void GXSetFieldMask(GXBool odd, GXBool even) { (void)odd; (void)even; }
void GXSetLineWidth(u8 width, GXTexOffset off) { (void)width; (void)off; }
void GXSetPointSize(u8 size, GXTexOffset off) { (void)size; (void)off; }
void GXEnableTexOffsets(GXTexCoordID coord, GXBool line, GXBool point) { (void)coord; (void)line; (void)point; }
void GXSetZTexture(GXZTexOp op, GXTexFmt fmt, u32 bias) { (void)op; (void)fmt; (void)bias; }
void GXSetCoPlanar(GXBool on) { (void)on; }

void GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    FLUSH();
    g_xgx.fog_type = type;
    g_xgx.fog_start = startz;
    g_xgx.fog_end = endz;
    g_xgx.fog_near = nearz;
    g_xgx.fog_far = farz;
    g_xgx.fog_color[0] = color.r;
    g_xgx.fog_color[1] = color.g;
    g_xgx.fog_color[2] = color.b;
    g_xgx.fog_color[3] = color.a;
    DIRTY(XGX_DIRTY_FOG);
}

void GXSetFogRangeAdj(GXBool enable, u16 center, GXFogAdjTable* table) { (void)enable; (void)center; (void)table; }

/* SDK GXInitFogAdjTable: range adjustment is not applied, but the table must
 * still be valid data */
void GXInitFogAdjTable(struct _GXFogAdjTable* table, uint16_t width, float projmtx[4][4]) {
    int i;
    (void)width;
    (void)projmtx;
    for (i = 0; i < 10; i++) table->r[i] = 0x100;
}
