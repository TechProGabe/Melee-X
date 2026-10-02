/* xhw_overlay.c - the settings menu's text over a finished frame
 * (xbox/src/sdk/menu.c, xgx_set_overlay in xgx.h).
 *
 * xgx_present calls this once the GPU is idle and before the flip and the
 * screenshot dumps, so it is plain CPU writes into the back buffer, as the
 * boot card draws (xhw_splash.c, same font): no GPU state to set up or
 * undo, the same in every video mode. The framebuffer is write-combined, so
 * nothing here reads it back (an opaque panel, not a darkened one), and an
 * sfence puts the writes out before the flip is queued.
 *
 * 8x16 glyphs at 480 lines, 16x32 at 720p: the menu's 18 rows fill 91% of
 * the height there, about the TV-safe area; a 19th would need 96%. */
#include <stdint.h>
#include <string.h>

#include "xgx.h"
#include "xhw.h"
#include "xhw_internal.h"

typedef struct {
    uint8_t* fb;
    int w, h, bpp, pitch;
} Surf;

static uint32_t to_px(const Surf* s, uint32_t rgb) {
    if (s->bpp == 16) return ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F);
    return 0xFF000000u | rgb;
}

static void fill(const Surf* s, int x, int y, int w, int h, uint32_t rgb) {
    uint32_t px = to_px(s, rgb);
    int x1 = x + w, y1 = y + h, i, j;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    for (j = y; j < y1; j++) {
        uint8_t* row = s->fb + (size_t)j * (size_t)s->pitch;
        if (s->bpp == 16)
            for (i = x; i < x1; i++) ((uint16_t*)row)[i] = (uint16_t)px;
        else
            for (i = x; i < x1; i++) ((uint32_t*)row)[i] = px;
    }
}

static void text(const Surf* s, const char* t, int x, int y, int z, uint32_t rgb) {
    for (; *t; t++, x += 8 * z) {
        const unsigned char* g = xhw_font16 + (unsigned char)*t * 16;
        int gy, gx;
        for (gy = 0; gy < 16; gy++)
            for (gx = 0; gx < 8; gx++)
                if (g[gy] & (0x80 >> gx)) fill(s, x + gx * z, y + gy * z, z, z, rgb);
    }
}

void xhw_overlay_draw(void* fb, int w, int h, int bpp, int pitch, const xgx_overlay* o) {
    Surf s;
    int z = h >= 720 ? 2 : 1, lh = 17 * z, r;
    if (!fb || o->rows <= 0) return;
    s.fb = (uint8_t*)fb;
    s.w = w;
    s.h = h;
    s.bpp = bpp;
    s.pitch = pitch;
    if (o->kind == XGX_OVERLAY_BOX) {
        int cols = o->cols, pad = 12 * z, bw, bh, x0, y0, b = z;
        for (r = 0; r < o->rows; r++) {
            int n = (int)strlen(o->text[r]);
            if (n > cols) cols = n;
        }
        bw = cols * 8 * z + 2 * pad;
        bh = o->rows * lh - z + 2 * pad;
        x0 = (w - bw) / 2;
        y0 = (h - bh) / 2;
        fill(&s, x0, y0, bw, bh, 0x5A6A9A);   /* border */
        fill(&s, x0 + b, y0 + b, bw - 2 * b, bh - 2 * b, 0x0C1428);
        for (r = 0; r < o->rows; r++) text(&s, o->text[r], x0 + pad, y0 + pad + r * lh, z, o->rgb[r]);
    } else {
        /* one line, centred near the top (inside the TV-safe area; the
         * title screen's copyright lines fill the bottom) */
        int n = (int)strlen(o->text[0]), tw = n * 8 * z, x = (w - tw) / 2, y = h / 12;
        if (n) {
            fill(&s, x - 6 * z, y - 3 * z, tw + 12 * z, 22 * z, 0x000000);
            text(&s, o->text[0], x, y, z, o->rgb[0]);
        }
    }
    __asm__ volatile("sfence" ::: "memory");
}
