/* xhw_video.c - picks the output mode once, before pbkit starts.
 *
 * 640x480 at 32 bits by default: progressive when the dashboard allows 480p
 * (settings.ini can force 480i), 16:9 when the dashboard is set to
 * widescreen. 720p is the default where the dashboard allows it on this AV
 * pack (settings.ini `720p = 1`; BACK held at boot gives 480i, boot.c):
 * 1280x720, always 16:9, at
 * 16-bit colour with a Z16 depth buffer, since three 1280x720x32
 * framebuffers plus depth don't fit next to the game in 64 MB
 * (OpenCrossing-Xbox's measurement). */
#include <hal/video.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "xhw.h"
#include "xhw_internal.h"

/* nxdk's video.c: the region in the encoder settings, and its 640x480i
 * mode for the HDTV pack (NTSC-M and NTSC-J use the same one) */
#define XHW_VIDEO_REGION_PAL 0x00000300
#define XHW_MODE_640x480I_HDTV 0x0801010du
void XVideoInit(DWORD dwMode, int width, int height, int bpp);

static xhw_video_mode s_mode = { 640, 480, 32, 0, 0 };
static int s_pref_720p = 0;   /* settings.ini, before boot */
static int s_pref_480p = 1;

const xhw_video_mode* xhw_video(void) { return &s_mode; }

int xhw_video_720p_allowed(void) {
    DWORD enc = XVideoGetEncoderSettings();
    DWORD pack = enc & VIDEO_ADAPTER_MASK;
    return (enc & VIDEO_MODE_720P) && (pack == AV_PACK_HDTV || pack == AV_PACK_VGA);
}

int xhw_video_480p_allowed(void) {
    DWORD enc = XVideoGetEncoderSettings();
    DWORD pack = enc & VIDEO_ADAPTER_MASK;
    /* nxdk has no PAL progressive modes */
    return (enc & VIDEO_MODE_480P) && (pack == AV_PACK_HDTV || pack == AV_PACK_VGA) &&
           (enc & VIDEO_STANDARD_MASK) != XHW_VIDEO_REGION_PAL;
}

int xhw_video_widescreen_set(void) { return (XVideoGetEncoderSettings() & VIDEO_WIDESCREEN) != 0; }

void xhw_video_set_pref_720p(int on) { s_pref_720p = on; }
void xhw_video_set_pref_480p(int on) { s_pref_480p = on; }

/* Test switch: -DXHW_VIDEO_480_BPP=16 runs 480 the way 720p runs (R5G6B5
 * colour, Z16 depth, 720p's pool sizes), so xemu, which has no 720p, can
 * run the 16-bit path. */
#ifndef XHW_VIDEO_480_BPP
#define XHW_VIDEO_480_BPP 32
#endif

/* 640x480x32: 480p when the dashboard allows it and settings.ini doesn't
 * say otherwise. XVideoSetMode always picks 480p on an HDTV pack set to
 * 480p, so 480i there is set with nxdk's own XVideoInit, after
 * XVideoSetMode has recorded the size for pbkit (XVideoGetMode). */
static void set_mode_480(void) {
    int p480 = xhw_video_480p_allowed();
    XVideoSetMode(640, 480, XHW_VIDEO_480_BPP, REFRESH_DEFAULT);
    if (p480 && !s_pref_480p && (XVideoGetEncoderSettings() & VIDEO_ADAPTER_MASK) == AV_PACK_HDTV)
        XVideoInit(XHW_MODE_640x480I_HDTV, 640, 480, XHW_VIDEO_480_BPP);
    s_mode.width = 640;
    s_mode.height = 480;
    s_mode.bpp = XHW_VIDEO_480_BPP;
    s_mode.progressive = p480 && (s_pref_480p || (XVideoGetEncoderSettings() & VIDEO_ADAPTER_MASK) != AV_PACK_HDTV);
}

void xhw_video_boot(void) {
    xhw_splash_release();   /* XVideoSetMode frees the splash's framebuffer */
    s_mode.widescreen = xhw_video_widescreen_set();
#if defined(XHW_AUTOPAD) && XHW_AUTOPAD
    {   /* test builds: "env MX_VIDEO=480" / "=480i" / "=720" from the autopad
         * script, over settings.ini, so a console round can run another
         * mode without changing the user's settings (docs/testing.md) */
        const char* e = getenv("MX_VIDEO");
        if (e) {
            s_pref_720p = strcmp(e, "720") == 0;
            s_pref_480p = strcmp(e, "480i") != 0;
            xhw_logf("[VIDEO] MX_VIDEO=%s", e);
        }
    }
#endif
    if (s_pref_720p && xhw_video_720p_allowed() && xhw_mem_free_kb() >= 32 * 1024 &&
        XVideoSetMode(1280, 720, 16, REFRESH_DEFAULT)) {
        s_mode.width = 1280;
        s_mode.height = 720;
        s_mode.bpp = 16;
        s_mode.widescreen = 1;
        s_mode.progressive = 1;
    } else {
        set_mode_480();
    }
    xhw_logf("[VIDEO] dashboard: encoder %08x, AV pack %u, widescreen %d, 480p %d, 720p %d",
             (unsigned)XVideoGetEncoderSettings(), (unsigned)(XVideoGetEncoderSettings() & VIDEO_ADAPTER_MASK),
             xhw_video_widescreen_set(), xhw_video_480p_allowed(), xhw_video_720p_allowed());
    xhw_logf("[VIDEO] %dx%d %d-bit%s%s", s_mode.width, s_mode.height, s_mode.bpp,
             s_mode.progressive ? " progressive" : " interlaced", s_mode.widescreen ? " 16:9" : " 4:3");
}

/* The renderer calls this when 720p can't start (pb_init or its contiguous
 * allocations fail): a saved setting must never leave a black screen. */
void xhw_video_fallback_480(void) {
    set_mode_480();
    s_mode.widescreen = xhw_video_widescreen_set();
    xhw_logf("[VIDEO] fell back to 640x480");
}

void xhw_wait_vblank(void) { XVideoWaitForVBlank(); }
