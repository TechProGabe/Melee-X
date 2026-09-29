/* vi.c - VI on the Xbox. Melee waits in HSD_VIWaitXFBFlush ->
 * VIWaitForRetrace once per frame, so that call is the frame boundary
 * (melee-pc's model, src/pc/vi.c there): present the frame the GX backend
 * built, pace the simulation to 60.000 Hz, run due alarms (the pad-poll
 * alarm reads the controllers), then the retrace callbacks. */
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>

#include "pc/pc.h"
#include "pc/widescreen.h"
#include "xhw.h"
#include "xsdk.h"

static u32 s_retrace;
static VIRetraceCallback s_pre_cb, s_post_cb;
static void* s_next_fb;
static void* s_cur_fb;
static BOOL s_black = TRUE;

bool pc_exit_requested;

GXRenderModeObj GXNtsc480IntDf = {
    VI_TVMODE_NTSC_INT, 640, 480, 480, 40, 0, 640, 480, VI_XFBMODE_DF, 0, 0,
    { 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6 },
    { 8, 8, 10, 12, 10, 8, 8 },
};
GXRenderModeObj GXNtsc480Int = {
    VI_TVMODE_NTSC_INT, 640, 480, 480, 40, 0, 640, 480, VI_XFBMODE_DF, 0, 0,
    { 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6 },
    { 0, 0, 21, 22, 21, 0, 0 },
};

u64 pc_sim_period_ns(void) { return 1000000000ull / 60; }

unsigned xsdk_frame_count(void) { return s_retrace; }

void VIInit(void) {}
void VIConfigure(const GXRenderModeObj* rm) { (void)rm; }
void VIConfigurePan(u16 x, u16 y, u16 w, u16 h) { (void)x; (void)y; (void)w; (void)h; }
void VIFlush(void) {}
u32 VIGetRetraceCount(void) { return s_retrace; }
u32 VIGetNextField(void) { return s_retrace & 1; }
u32 VIGetDTVStatus(void) { return xhw_video()->progressive; }
u32 VIGetTvFormat(void) { return VI_NTSC; }
u32 VIGetCurrentLine(void) { return 0; }
void* VIGetCurrentFrameBuffer(void) { return s_cur_fb; }
void* VIGetNextFrameBuffer(void) { return s_next_fb; }
void VISetNextFrameBuffer(void* fb) { s_next_fb = fb; }
void VISetBlack(BOOL black) { s_black = black; }
int xsdk_vi_black(void) { return s_black; }
u16 VIPadFrameBufferWidth(u16 width) { return (u16)((width + 15) & ~15); }

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback old = s_pre_cb;
    s_pre_cb = cb;
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback old = s_post_cb;
    s_post_cb = cb;
    return old;
}

void xsdk_frame_boundary(void) {
    static u64 next_ns;
    const u64 period = pc_sim_period_ns();
    u64 now;

    /* the flip itself happens at GXCopyDisp (gx_copy.c) */
    pc_widescreen_update();

    /* 60.000 Hz regardless of the output: 50 Hz PAL 480i consoles too */
    now = xhw_time_ns();
    if (next_ns == 0 || now > next_ns + period * 2) {
        next_ns = now;   /* a hitch (loading): don't try to catch up */
    } else {
        while (now + 1500000ull < next_ns) {
            xhw_sleep_ms(1);
            now = xhw_time_ns();
        }
        while (now < next_ns) now = xhw_time_ns();
    }
    next_ns += period;

    s_retrace++;
    xsdk_run_alarms();
    if (s_pre_cb) s_pre_cb(s_retrace);
    s_cur_fb = s_next_fb;
    if (s_post_cb) s_post_cb(s_retrace);
}

void VIWaitForRetrace(void) { xsdk_frame_boundary(); }
