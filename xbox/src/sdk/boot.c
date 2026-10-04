/* boot.c - from the Xbox entry point (xbox/src/hw/xhw_main.c) into the game. */
#include <dolphin/os.h>
#include <stdio.h>
#include <string.h>

#include "pc/discfont.h"
#include "pc/pc.h"
#include "pc/region.h"
#include "pc/widescreen.h"
#include "xhw.h"
#include "xsdk.h"
#include "xsdk_settings.h"

int melee_main(void);

/* BACK held on any controller while Melee-X starts (the splash says so):
 * 480i for a TV that doesn't show the mode the settings ask for (720p by
 * default where the dashboard allows it). Read before the settings load,
 * so the press can't reach the BACK screenshot. */
static int safe_video_held(void) {
    xhw_pad p;
    int i;
    xhw_pad_poll();
    for (i = 0; i < 4; i++)
        if (xhw_pad_get(i, &p) && (p.buttons & XHW_BTN_BACK)) return 1;
    return 0;
}

/* before the video mode is chosen */
void xsdk_early(void) {
    int safe = safe_video_held();
    /* 128 MB consoles always run in 64 MB (no setting since v53,
     * docs/decisions.md): held before the settings file's allocations */
    uint32_t held = xhw_mem_hold_upper();
    if (held) xhw_logf("[MEM] 128 MB console: running in 64 MB, %u KB above it held back", held);
    xsdk_settings_load();
    if (safe) {
        /* saved, so the next boot stays visible too; the menu turns them back on */
        g_xsdk_settings.video_720p = 0;
        g_xsdk_settings.progressive = 0;
        xsdk_settings_save();
        g_xsdk_settings_boot = g_xsdk_settings;
        xhw_logf("[VIDEO] BACK held at boot: 480i (720p and progressive off in settings.ini)");
    }
    xhw_video_set_pref_720p(g_xsdk_settings.video_720p);
    xhw_video_set_pref_480p(g_xsdk_settings.progressive);
}

void xsdk_boot(const char* disc) {
    char why[200];
    const xhw_video_mode* vm = xhw_video();
    const u8* dol;
    s32 dol_size;

    OSInit();
    xhw_splash_progress(0.3f);
    if (!xsdk_dvd_open(disc, why, sizeof why)) xhw_fatal("Wrong or damaged disc image", why);
    pc_region_set((const char*)DVDGetCurrentDiskID());
    xhw_splash_progress(0.6f);

    /* The debug and SIS font atlases are pixel data in main.dol; lift them
     * from the user's disc (discfont.c), then drop the DOL copy. */
    dol = DVDGetDOLLocation(&dol_size);
    if (!dol || !pc_load_disc_fonts(disc)) xhw_logf("[BOOT] fonts not found in main.dol: menu text will be missing");
    xsdk_dvd_free_dol();
    xhw_splash_progress(1.0f);

    /* The title card stays up through the loads above; the mode change
     * ends it. */
    xhw_video_boot();

    /* 16:9 at 720p always; at 480 when the dashboard is set to widescreen */
    pc_widescreen_set_mode(vm->widescreen && g_xsdk_settings.widescreen ? 1 : 0);

    xhw_logf("[BOOT] melee_main");
    melee_main();
    xhw_quit_to_dashboard();
}
