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

/* before the video mode is chosen */
void xsdk_early(void) {
    xsdk_settings_load();
    xhw_video_set_pref_720p(g_xsdk_settings.video_720p);
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
