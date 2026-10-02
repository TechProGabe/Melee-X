/* xhw_splash.c - the boot title card, ported from OpenCrossing-Xbox
 * (xbox/src/xbox_splash.c).
 *
 * Drawn with the CPU straight into the linear framebuffer XVideoSetMode()
 * hands back, before the renderer takes the display: no GPU state, no
 * textures, no assets, nothing to undo afterwards.
 *
 *   xhw_splash_show()       "TechProGabe Presents..." on a navy gradient,
 *                           fading in; holds XHW_SPLASH_MS (any button skips)
 *   xhw_splash_progress(f)  load bar under the title, 0..1
 *   xhw_splash_release()    xhw_video_boot is about to change the mode
 *
 * Font: unscii-16 (public domain), the 8x16 face nxdk's debug console uses.
 * Kill switch: -DXHW_NO_SPLASH. Duration: -DXHW_SPLASH_MS=<n>. */
#include <hal/video.h>
#include <windows.h>
#include <string.h>

#include "xhw.h"
#include "xhw_internal.h"

#ifndef XHW_SPLASH_MS
#define XHW_SPLASH_MS 2000
#endif

#define SPLASH_TEXT "TechProGabe Presents..."
#define SCR_W 640
#define SCR_H 480

const unsigned char xhw_font16[256 * 16] = {   /* also the overlay's (xhw_overlay.c) */
#include <hal/font_unscii_16.h>
};
#define GLYPH_W 8
#define GLYPH_H 16

static unsigned int* s_fb;
static int s_ready;

static unsigned int rgb(int r, int g, int b) {
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return 0xFF000000u | ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

/* dark navy at the top fading to near-black: OpenCrossing's palette */
static unsigned int bg_at(int y) {
    int t = (y * 255) / (SCR_H - 1);
    return rgb(6 + (8 * (255 - t)) / 255, 10 + (12 * (255 - t)) / 255, 28 + (24 * (255 - t)) / 255);
}

static void fill_bg(int y0, int y1) {
    int x, y;
    for (y = y0; y < y1; y++) {
        unsigned int c = bg_at(y);
        unsigned int* row = s_fb + y * SCR_W;
        for (x = 0; x < SCR_W; x++) row[x] = c;
    }
}

static void draw_text(const char* s, int x, int y, int zoom, unsigned int col) {
    for (; *s; s++, x += GLYPH_W * zoom) {
        const unsigned char* g = xhw_font16 + (unsigned char)*s * GLYPH_H;
        int gy, gx, zy, zx;
        for (gy = 0; gy < GLYPH_H; gy++) {
            for (gx = 0; gx < GLYPH_W; gx++) {
                if (!(g[gy] & (0x80 >> gx))) continue;
                for (zy = 0; zy < zoom; zy++) {
                    int py = y + gy * zoom + zy;
                    if (py < 0 || py >= SCR_H) continue;
                    for (zx = 0; zx < zoom; zx++) {
                        int px = x + gx * zoom + zx;
                        if (px >= 0 && px < SCR_W) s_fb[py * SCR_W + px] = col;
                    }
                }
            }
        }
    }
}

static void draw_centered(const char* s, int y, int zoom, unsigned int col) {
    draw_text(s, (SCR_W - (int)strlen(s) * GLYPH_W * zoom) / 2, y, zoom, col);
}

static int init_fb(void) {
    if (s_ready) return 1;
    if (!XVideoSetMode(SCR_W, SCR_H, 32, REFRESH_DEFAULT)) return 0;
    s_fb = (unsigned int*)XVideoGetFB();
    if (!s_fb) return 0;
    s_ready = 1;
    return 1;
}

/* Any button on any controller skips the hold. The ports may still be
 * enumerating this early; then the timer runs out as usual. */
static int any_button(void) {
    xhw_pad p;
    int i;
    xhw_pad_poll();
    for (i = 0; i < 4; i++)
        if (xhw_pad_get(i, &p) && (p.buttons || p.lt > 128 || p.rt > 128)) return 1;
    return 0;
}

#define TITLE_ZOOM 3
#define TITLE_Y    ((SCR_H - GLYPH_H * TITLE_ZOOM) / 2 - 24)
#define BAR_X      120
#define BAR_W      400
#define BAR_H      8
#define BAR_Y      (TITLE_Y + GLYPH_H * TITLE_ZOOM + 32)

void xhw_splash_show(void) {
#ifndef XHW_NO_SPLASH
    DWORD t0;
    int step;
    if (!init_fb()) return;
    fill_bg(0, SCR_H);
    /* fade in over ~500 ms: redraw only the title band. No
     * XVideoWaitForVBlank(): it hooks the GPU interrupt, and pb_init() then
     * fails with -4 when the renderer starts (OpenCrossing's finding). */
    for (step = 0; step <= 16; step++) {
        int v = 40 + (215 * step) / 16;
        fill_bg(TITLE_Y, TITLE_Y + GLYPH_H * TITLE_ZOOM);
        draw_centered(SPLASH_TEXT, TITLE_Y, TITLE_ZOOM, rgb(v, v, v));
        Sleep(30);
    }
    /* the splash is in the dashboard's own mode, so this shows on any TV */
    draw_centered("Hold BACK for 480i (safe video)", SCR_H - 64, 1, rgb(110, 120, 140));
    xhw_splash_progress(0.0f);
    xhw_logf("[BOOT] splash: %s", SPLASH_TEXT);
#ifdef XHW_SPLASH_DUMP
    xhw_fbdump(s_fb, SCR_W, SCR_H, 32, SCR_W * 4);
#endif
    t0 = GetTickCount();
    while (GetTickCount() - t0 < XHW_SPLASH_MS) {
        if (any_button()) break;
        Sleep(16);
    }
#endif
}

/* XVideoSetMode frees this framebuffer: forget it, so later progress calls
 * draw nothing. */
void xhw_splash_release(void) {
    s_ready = 0;
    s_fb = NULL;
}

void xhw_splash_progress(float f) {
#ifndef XHW_NO_SPLASH
    int x, y, fill;
    if (!s_ready) return;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    fill = (int)(f * BAR_W);
    for (y = BAR_Y; y < BAR_Y + BAR_H; y++) {
        unsigned int* row = s_fb + y * SCR_W;
        for (x = 0; x < BAR_W; x++) row[BAR_X + x] = x < fill ? rgb(120, 200, 110) : rgb(30, 40, 60);
    }
#else
    (void)f;
#endif
}
