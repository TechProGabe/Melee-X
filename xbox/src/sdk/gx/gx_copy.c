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

static int is_intensity(uint32_t fmt) {
    switch (fmt) {
        case GX_TF_I4: case GX_TF_I8: case GX_TF_IA4: case GX_TF_IA8: return 1;
        default: return 0;
    }
}

void GXCopyTex(void* dest, GXBool clear) {
    uint32_t tex;
    gx_vtx_flush();
    tex = xgx_tex_from_efb(g_gx.tex_copy_src, g_gx.tex_copy_w, g_gx.tex_copy_h, is_intensity(g_gx.tex_copy_fmt),
                           gx_tex_efb_texture(dest));
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
