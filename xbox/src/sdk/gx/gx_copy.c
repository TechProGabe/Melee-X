/* gx_copy.c - EFB copies, the frame flip, and the output geometry the game's
 * widescreen code asks for (the Aurora* sizing calls).
 *
 * GXCopyDisp is where a GameCube frame ends: the EFB goes to the XFB and is
 * cleared for the next frame. Here the back buffer is the EFB, so the flip
 * happens right there and the new back buffer gets the copy-clear colour.
 * VIWaitForRetrace (vi.c) then only paces. GXCopyTex copies into a texture
 * that binds wherever the game points a texture object at the destination. */
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <stdio.h>
#include <string.h>

#include "gx_internal.h"
#include "xhw.h"
#include "xsdk.h"

void xsdk_gx_draw_done(void);
int xsdk_vi_black(void);

void GXSetCopyClear(GXColor color, u32 z) {
    g_gx.clear_color = color;
    g_gx.clear_z = z & 0xFFFFFF;
}

void GXSetCopyFilter(GXBool aa, u8 pattern[12][2], GXBool vf, u8 vfilter[7]) {
    (void)aa; (void)pattern; (void)vf; (void)vfilter;
}

void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
    g_gx.disp_copy_src[0] = left;
    g_gx.disp_copy_src[1] = top;
    g_gx.disp_copy_src[2] = wd;
    g_gx.disp_copy_src[3] = ht;
}

void GXSetDispCopyDst(u16 wd, u16 ht) { (void)wd; (void)ht; }
u32 GXSetDispCopyYScale(f32 vscale) { (void)vscale; return XGX_EFB_H; }
void GXSetDispCopyGamma(GXGamma gamma) { (void)gamma; }

static void clear_rect(const int32_t r[4], int color, int depth) {
    uint8_t c[4] = { g_gx.clear_color.r, g_gx.clear_color.g, g_gx.clear_color.b, g_gx.clear_color.a };
    xgx_clear(r, c, g_gx.clear_z, color, color, depth);
}

void GXCopyDisp(void* dest, GXBool clear) {
    static const int32_t full[4] = { 0, 0, XGX_EFB_W, XGX_EFB_H };
    (void)dest;
    gx_vtx_flush();
    xgx_present(xsdk_vi_black());
    gx_tex_frame_end();
    gx_vtx_frame_end();
    if (xhw_debug_flush_take()) {
        gx_tex_flush_all();
        gx_vtx_cache_flush();
        xhw_log("[DEBUG] texture and display-list caches flushed (BACK+Y)");
    }
    if (clear) clear_rect(full, 1, 1);
}

void xsdk_gx_present(int black) {
    (void)black;   /* the flip is in GXCopyDisp */
}

void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
    g_gx.tex_copy_src[0] = left;
    g_gx.tex_copy_src[1] = top;
    g_gx.tex_copy_src[2] = wd;
    g_gx.tex_copy_src[3] = ht;
}

void GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap) {
    g_gx.tex_copy_w = wd;
    g_gx.tex_copy_h = ht;
    g_gx.tex_copy_fmt = fmt;
    g_gx.tex_copy_mip = mipmap;
}

/* how the copy's pixels read back through the texture format it is sampled
 * as (XGX_COPY_*): intensity formats return I in every channel (alpha too,
 * unless the format has its own alpha), and the R/G/B/A copies are sampled
 * as I4/I8 of that channel. Shadow maps are GX_CTF_R4. */
static int copy_mode(uint32_t fmt) {
    if (fmt & _GX_TF_ZTF) return XGX_COPY_ALPHA;   /* depth: the mask xgx_ztex_mask leaves (GXCopyTex) */
    switch (fmt) {
        case GX_TF_I4: case GX_TF_I8: return XGX_COPY_LUMA;
        case GX_TF_IA4: case GX_TF_IA8: return XGX_COPY_LUMA_ALPHA;
        case GX_CTF_R4: case GX_CTF_R8: return XGX_COPY_RED;
        case GX_CTF_RA4: case GX_CTF_RA8: return XGX_COPY_RED_ALPHA;
        case GX_CTF_G8: return XGX_COPY_GREEN;
        case GX_CTF_B8: return XGX_COPY_BLUE;
        case GX_CTF_A8: return XGX_COPY_ALPHA;
        default: return XGX_COPY_COLOR;
    }
}

void GXCopyTex(void* dest, GXBool clear) {
    uint32_t tex, w = g_gx.tex_copy_w, h = g_gx.tex_copy_h, room;
    int mode = copy_mode(g_gx.tex_copy_fmt);
    gx_vtx_flush();
#ifdef XGX_DEBUG_EFBLOG
    {
        static int s_n;
        char line[128];
        if (s_n++ < 200) {
            snprintf(line, sizeof line, "[EFB] copy %d: src %d,%d %dx%d -> %ux%u fmt %x clear %d z %06x dest %p", s_n,
                     (int)g_gx.tex_copy_src[0], (int)g_gx.tex_copy_src[1], (int)g_gx.tex_copy_src[2],
                     (int)g_gx.tex_copy_src[3], w, h, (unsigned)g_gx.tex_copy_fmt, clear, (unsigned)g_gx.clear_z, dest);
            xhw_log(line);
        }
    }
#endif
    /* a depth copy for GXSetZTexture: in front of the depth this copy's
     * clear primes is what the Z-texture draw lets through (the Classic
     * team cards copy each fighter's depth and draw them back masked) */
    if (g_gx.tex_copy_fmt & _GX_TF_ZTF) mode = xgx_ztex_mask(g_gx.tex_copy_src, g_gx.clear_z, clear);
    tex = xgx_tex_from_efb(g_gx.tex_copy_src, w, h, mode, gx_tex_efb_texture(dest));
    /* pool full: the overflow pool when only this frame's textures are left
     * (as uploads do), else evict and copy again (eviction may have taken
     * the old copy) */
    if (!tex && w && h && w <= 1024 && h <= 1024 && gx_tex_grow_for_frame())
        tex = xgx_tex_from_efb(g_gx.tex_copy_src, w, h, mode, gx_tex_efb_texture(dest));
    for (room = w * h * 4; !tex && w && h && w <= 1024 && h <= 1024 && gx_tex_make_room(room); room *= 2)
        tex = xgx_tex_from_efb(g_gx.tex_copy_src, w, h, mode, gx_tex_efb_texture(dest));
    if (!tex && w && h) {
        static int s_logged;
        if (!s_logged) {
            s_logged = 1;
            xhw_logf("[TEX] copy dropped: %ux%u to %p; pool %u of %u KB free, largest block %u KB", w, h, dest,
                     xgx_tex_pool_free_kb(), xgx_tex_pool_kb(), xgx_tex_pool_largest_kb());
        }
    }
    gx_tex_note_efb_copy(dest, tex, g_gx.tex_copy_w, g_gx.tex_copy_h, g_gx.tex_copy_fmt);
    if (clear) clear_rect(g_gx.tex_copy_src, 1, 1);
}

/* ---- output geometry for src/pc/widescreen.c ---- */
void AuroraSetViewportPolicy(AuroraViewportPolicy policy) { (void)policy; }

void AuroraSetPresentationAspect(f32 aspect) { xgx_set_content_aspect(aspect); }

void AuroraGetRenderSize(u32* width, u32* height) {
    uint32_t w, h;
    xgx_content_size(&w, &h);
    *width = w;
    *height = h;
}

void AuroraGetWindowSize(u32* width, u32* height) {
    const xhw_video_mode* vm = xhw_video();
    *height = (u32)vm->height;
    *width = vm->widescreen ? (u32)(vm->height * 16 / 9) : (u32)(vm->height * 4 / 3);
}

void AuroraGXSync(void) { gx_vtx_flush(); }
