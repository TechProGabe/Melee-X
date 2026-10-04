/* simhash.c - [SIMH]: a hash of the simulation's state, test builds only.
 *
 * gmscene.c's frame loop (PORT: there) calls xsdk_sim_tick after every
 * simulation tick. While fighters exist, every 60th tick since the first one
 * logs an FNV-1a hash of each fighter's kind, port, action state, facing,
 * position, own and knockback velocity and damage, plus the random seed.
 * Two runs of one scenario must log the same lines; a toolchain or
 * simulation change must reproduce the baseline's (docs/fps-plan.md step
 * 0.5, tools/xbox/icount_report.py --simh). Without fighters the count
 * starts over, so each match counts from its first tick. */
#include <stdlib.h>

#include <melee/ft/forward.h>
#include <melee/ft/types.h>
#include <string.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/random.h>

#include "xhw.h"
#include "xsdk.h"

#if XHW_TEST_BUILD
static uint32_t s_tick;
static int s_verbose = -1;

static uint32_t fnv(uint32_t h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    while (n--) h = (h ^ *b++) * 16777619u;
    return h;
}

void xsdk_sim_tick(void) {
    HSD_GObj* cur = HSD_GObjPLinkHead[HSD_GOBJ_PLINK_FIGHTER];
    uint32_t h = 2166136261u, seed;
    int n = 0;
    if (!cur) {
        s_tick = 0;
        return;
    }
    if (s_verbose < 0) {
        const char* e = getenv("MX_SIMH_VERBOSE");   /* autopad env: every tick, each fighter's fields */
        s_verbose = e && *e == '1';
    }
    xhw_autopad_tick(++s_tick);   /* an autopad script's TSHOT */
    if (s_tick % (s_verbose ? 1 : 60)) return;
    seed = *HSD_RandSeedPtr;
    h = fnv(h, &seed, sizeof seed);
    for (; cur; cur = cur->next) {
        const Fighter* fp = (const Fighter*)cur->user_data;
        if (!fp) continue;
        h = fnv(h, &fp->kind, sizeof fp->kind);
        h = fnv(h, &fp->player_idx, sizeof fp->player_idx);
        h = fnv(h, &fp->motion_id, sizeof fp->motion_id);
        h = fnv(h, &fp->facing_dir, sizeof fp->facing_dir);
        h = fnv(h, &fp->cur_pos, sizeof fp->cur_pos);
        h = fnv(h, &fp->self_vel, sizeof fp->self_vel);
        h = fnv(h, &fp->x8c_kb_vel, sizeof fp->x8c_kb_vel);
        h = fnv(h, &fp->dmg.x1830_percent, sizeof fp->dmg.x1830_percent);
        n++;
        if (s_verbose) {   /* to find which fighter and field two runs part on */
            const uint32_t* p = (const uint32_t*)&fp->cur_pos;
            const uint32_t* v = (const uint32_t*)&fp->self_vel;
            const uint32_t* k = (const uint32_t*)&fp->x8c_kb_vel;
            xhw_logf("[SIMH]   %d: kind %d motion %d facing %08x pos %08x %08x %08x vel %08x %08x kb %08x %08x "
                     "dmg %08x",
                     n, (int)fp->kind, (int)fp->motion_id, *(const uint32_t*)&fp->facing_dir, p[0], p[1], p[2], v[0],
                     v[1], k[0], k[1], *(const uint32_t*)&fp->dmg.x1830_percent);
        }
    }
    xhw_logf("[SIMH] tick %u: %08x (%d fighters, seed %08x)", s_tick, h, n, seed);
}
#else
void xsdk_sim_tick(void) {}
#endif
