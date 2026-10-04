/* simhash.c - [SIMH]: a hash of the simulation's state, test builds only.
 *
 * gmscene.c's frame loop (PORT: there) calls xsdk_sim_tick after every
 * simulation tick. While fighters exist, every 60th tick since the first one
 * logs an FNV-1a hash of each fighter's kind, port, action state, facing,
 * position, own and knockback velocity and damage, plus the random seed.
 * Two runs of one scenario must log the same lines; a toolchain or
 * simulation change must reproduce the baseline's (docs/fps-plan.md step
 * 0.5, tools/xbox/icount_report.py --simh). Without fighters the count
 * starts over, so each match counts from its first tick.
 *
 * The RNG trace (docs/lan-plan.md phase 0A): random.c (PORT: there) reports
 * every HSD_Rand draw here with its caller. With env MX_RAND_TRACE=1 each
 * tick of a match logs [RAND]: the tick's draws (those made during the
 * render pass and off the game thread counted apart), a rolling hash of
 * every draw of the match so far (caller and new seed) and a sum of the
 * tick's callers' hashes. The seed sequence depends only on the number of
 * draws, so callers that draw in another order (the particle system's lists
 * do, in xemu) change the rolling hash for good but neither the count nor
 * the sum: tools/xbox/simh_diff.py tells the three apart. env
 * MX_RAND_DUMP=<a>-<b> also logs each
 * caller of ticks a..b as [RANDD] lines (r: in the render pass, t: off the
 * game thread), symbolized by simh_diff.py --map. Draws between two ticks
 * (render, scene entry) count toward the next tick. The trace only
 * observes: [SIMH] is the same with and without it.
 *
 * [NETM] at the match's ticks 600 and 3600 (os.c): the live bytes of the
 * OSAlloc heaps and a 1 MB copy timed, for rollback's numbers (F5). */
#include <stdlib.h>

#include <melee/ft/forward.h>
#include <melee/ft/types.h>
#include <stdio.h>
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

/* ---- the RNG trace ---- */
#define DUMP_MAX 1024
static int s_rt = -1;                  /* MX_RAND_TRACE or MX_RAND_DUMP set */
static int s_rt_log;                   /* MX_RAND_TRACE: a [RAND] line a tick */
static uint32_t s_dump_a = 1, s_dump_b;   /* MX_RAND_DUMP=a-b: ticks whose callers are logged */
static uint32_t s_rn, s_rr, s_ro;     /* this tick's draws: all, in the render pass, off the game thread */
static uint32_t s_rhash = 2166136261u;   /* every draw of the match so far */
static uint32_t s_rset;                /* this tick's callers, order left out: a sum of their hashes */
static uint32_t s_dump[DUMP_MAX];      /* this tick's callers */
static char s_dump_kind[DUMP_MAX];     /* ' ', 'r' render pass, 't' another thread */

static void rand_trace_init(void) {
    const char* t = getenv("MX_RAND_TRACE");
    const char* d = getenv("MX_RAND_DUMP");
    s_rt_log = t && *t == '1';
    if (d) {
        char* end;
        s_dump_a = (uint32_t)strtoul(d, &end, 10);
        s_dump_b = *end == '-' ? (uint32_t)strtoul(end + 1, NULL, 10) : s_dump_a;
    }
    if (s_rt_log || s_dump_b)
        xhw_logf("[RAND] trace on: a line a tick %s, callers of ticks %u-%u", s_rt_log ? "yes" : "no",
                 (unsigned)s_dump_a, (unsigned)s_dump_b);
    s_rt = s_rt_log || s_dump_b;
}

void xsdk_rand_draw(u32 seed, void* caller) {
    uint32_t c = (uint32_t)(uintptr_t)caller;
    char kind = ' ';
    if (s_rt <= 0) {
        if (s_rt == 0) return;
        rand_trace_init();
        if (!s_rt) return;
    }
    if (!xsdk_is_game_thread()) {
        kind = 't';
        s_ro++;
    } else if (xsdk_in_render()) {
        kind = 'r';
        s_rr++;
    }
    if (s_rn < DUMP_MAX) {
        s_dump[s_rn] = c;
        s_dump_kind[s_rn] = kind;
    }
    s_rn++;
    s_rset += fnv(2166136261u, &c, sizeof c);
    s_rhash = fnv(s_rhash, &c, sizeof c);
    s_rhash = fnv(s_rhash, &seed, sizeof seed);
}

static void rand_trace_reset(void) {
    s_rn = s_rr = s_ro = s_rset = 0;
}

/* The trace's lines go out in batches, one log call each: during a boot's
 * first 600 frames (a scripted match starts at frame 2) every log call is
 * a disk flush on the console, and one a tick would slow the match the
 * trace is watching. A batch goes out before each [SIMH] and [NETM] line,
 * at the match's end, and when full. */
static char s_batch[16384];
static uint32_t s_batch_len;

static void batch_flush(void) {
    if (!s_batch_len) return;
    xhw_log(s_batch);   /* ends with '\n': no newline added */
    s_batch_len = 0;
    s_batch[0] = '\0';
}

static void batch_line(const char* line, int len) {
    if (len <= 0) return;
    if (len >= (int)sizeof s_batch - 1) len = (int)sizeof s_batch - 2;
    if (s_batch_len + (uint32_t)len + 2 > sizeof s_batch) batch_flush();
    memcpy(s_batch + s_batch_len, line, (size_t)len);
    s_batch_len += (uint32_t)len;
    s_batch[s_batch_len++] = '\n';
    s_batch[s_batch_len] = '\0';
}

static void rand_trace_tick(uint32_t tick) {
    char line[1000];
    if (s_rt <= 0) return;
    if (s_rt_log) {
        int len;
        if (s_rr || s_ro)
            len = snprintf(line, sizeof line,
                           "[RAND] tick %u: %u draws (%u in render, %u off thread), hash %08x, callers %08x",
                           (unsigned)tick, (unsigned)s_rn, (unsigned)s_rr, (unsigned)s_ro, (unsigned)s_rhash,
                           (unsigned)s_rset);
        else
            len = snprintf(line, sizeof line, "[RAND] tick %u: %u draws, hash %08x, callers %08x", (unsigned)tick,
                           (unsigned)s_rn, (unsigned)s_rhash, (unsigned)s_rset);
        batch_line(line, len);
    }
    if (tick >= s_dump_a && tick <= s_dump_b) {
        uint32_t i = 0, n = s_rn < DUMP_MAX ? s_rn : DUMP_MAX;
        do {   /* ~90 callers a line; a tick without draws still gets one */
            int len = snprintf(line, sizeof line, "[RANDD] tick %u:", (unsigned)tick);
            for (; i < n && len < (int)sizeof line - 16; i++)
                len += snprintf(line + len, sizeof line - len, " %s%08x", s_dump_kind[i] == ' ' ? "" :
                                s_dump_kind[i] == 'r' ? "r:" : "t:", (unsigned)s_dump[i]);
            if (i == n && s_rn > n)
                len += snprintf(line + len, sizeof line - len, " +%u more", (unsigned)(s_rn - n));
            batch_line(line, len);
        } while (i < n);
    }
    rand_trace_reset();
}

void xsdk_sim_tick(void) {
    HSD_GObj* cur = HSD_GObjPLinkHead[HSD_GOBJ_PLINK_FIGHTER];
    uint32_t h = 2166136261u, seed;
    int n = 0;
    if (!cur) {
        s_tick = 0;
        batch_flush();
        rand_trace_reset();   /* the match's trace starts with its first tick */
        s_rhash = 2166136261u;
        return;
    }
    if (s_verbose < 0) {
        const char* e = getenv("MX_SIMH_VERBOSE");   /* autopad env: every tick, each fighter's fields */
        s_verbose = e && *e == '1';
    }
    xhw_autopad_tick(++s_tick);   /* an autopad script's TSHOT */
    rand_trace_tick(s_tick);
    if (s_tick == 600 || s_tick == 3600) {
        batch_flush();
        xsdk_netm(s_tick);
    }
    if (s_tick % (s_verbose ? 1 : 60)) return;
    batch_flush();
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
void xsdk_rand_draw(u32 seed, void* caller) {
    (void)seed;
    (void)caller;
}
#endif
