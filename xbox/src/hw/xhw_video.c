/* xhw_video.c - picks the output mode once, before pbkit starts.
 *
 * 720p is used whenever the dashboard allows it on this AV pack (and the
 * user hasn't turned it off in settings.ini): 1280x720, always 16:9. It runs
 * at 16-bit colour with a Z16 depth buffer: three 1280x720x32 framebuffers
 * plus depth don't fit next to the game in 64 MB (OpenCrossing-Xbox's
 * measurement). Otherwise 640x480 at 32 bits, progressive when allowed, 16:9
 * when the dashboard is set to widescreen. */
#include <hal/video.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "xhw.h"
#include "xhw_internal.h"

static xhw_video_mode s_mode = { 640, 480, 32, 0, 0 };
static int s_pref_720p = 1;   /* xsdk settings may clear it before boot */

const xhw_video_mode* xhw_video(void) { return &s_mode; }

int xhw_video_720p_allowed(void) {
    DWORD enc = XVideoGetEncoderSettings();
    DWORD pack = enc & VIDEO_ADAPTER_MASK;
    return (enc & VIDEO_MODE_720P) && (pack == AV_PACK_HDTV || pack == AV_PACK_VGA);
}

int xhw_video_480p_allowed(void) {
    DWORD enc = XVideoGetEncoderSettings();
    DWORD pack = enc & VIDEO_ADAPTER_MASK;
    return (enc & VIDEO_MODE_480P) && (pack == AV_PACK_HDTV || pack == AV_PACK_VGA);
}

int xhw_video_widescreen_set(void) { return (XVideoGetEncoderSettings() & VIDEO_WIDESCREEN) != 0; }

void xhw_video_set_pref_720p(int on) { s_pref_720p = on; }

void xhw_video_boot(void) {
    s_mode.widescreen = xhw_video_widescreen_set();
    if (s_pref_720p && xhw_video_720p_allowed() && xhw_mem_free_kb() >= 32 * 1024 &&
        XVideoSetMode(1280, 720, 16, REFRESH_DEFAULT)) {
        s_mode.width = 1280;
        s_mode.height = 720;
        s_mode.bpp = 16;
        s_mode.widescreen = 1;
        s_mode.progressive = 1;
    } else {
        XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
        s_mode.progressive = xhw_video_480p_allowed();
    }
    xhw_logf("[VIDEO] %dx%d %d-bit%s%s", s_mode.width, s_mode.height, s_mode.bpp,
             s_mode.progressive ? " progressive" : " interlaced", s_mode.widescreen ? " 16:9" : " 4:3");
}

/* The renderer calls this when 720p can't start (pb_init or its contiguous
 * allocations fail): a saved setting must never leave a black screen. */
void xhw_video_fallback_480(void) {
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    s_mode.width = 640;
    s_mode.height = 480;
    s_mode.bpp = 32;
    s_mode.widescreen = xhw_video_widescreen_set();
    s_mode.progressive = xhw_video_480p_allowed();
    xhw_logf("[VIDEO] fell back to 640x480");
}

void xhw_wait_vblank(void) { XVideoWaitForVBlank(); }
