/* xhw_perf.c - where the game thread's time goes, measured on the console.
 *
 * Every 5 s: frames presented, fps, and ms per frame in each bucket of
 * xhw.h's XHW_PERF_* list, plus the share of wall time the audio mixer
 * thread took. rdtsc is cheap enough to switch buckets per display list and
 * per draw. The mixer thread preempts the game thread, so its time also
 * shows up in whichever bucket was current, mostly LOGIC. */
#include <stdio.h>

#include "xhw.h"

#ifndef XHW_PERF_SECS
#define XHW_PERF_SECS 5
#endif

static int s_cur;
static uint64_t s_mark;
static uint64_t s_acc[XHW_PERF_N];
static volatile uint64_t s_audio;
static uint64_t s_t0, s_ns0;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}
static uint32_t s_frames, s_draws, s_verts;

static void charge(void) {
    uint64_t now = rdtsc();
    if (s_mark) s_acc[s_cur] += now - s_mark;
    s_mark = now;
}

int xhw_perf_enter(int bucket) {
    int prev = s_cur;
    charge();
    s_cur = bucket;
    return prev;
}

void xhw_perf_leave(int prev) {
    charge();
    s_cur = prev;
}

uint64_t xhw_perf_now(void) { return rdtsc(); }

void xhw_perf_audio(uint64_t ticks) { __atomic_fetch_add(&s_audio, ticks, __ATOMIC_RELAXED); }

/* tenths, for printing without floating point (pdclib's %f is unreliable) */
static unsigned tenths(uint64_t num, uint64_t den) { return den ? (unsigned)((num * 10 + den / 2) / den) : 0; }

void xhw_perf_frame(uint32_t draws, uint32_t verts) {
    static const char* const k_names[XHW_PERF_N] = { "logic", "dlist", "draw", "tex", "efb", "gpu", "vsync" };
    uint64_t now = rdtsc(), ns = xhw_time_ns(), span, span_ns, audio, per_ms;
    char line[320];
    int i, n;
    s_frames++;
    s_draws += draws;
    s_verts += verts;
    if (!s_t0) {
        s_t0 = now;
        s_ns0 = ns;
        return;
    }
    span_ns = ns - s_ns0;
    if (span_ns < (uint64_t)XHW_PERF_SECS * 1000000000ull) return;
    charge();
    span = now - s_t0;
    per_ms = span * 1000000ull / span_ns;   /* rdtsc ticks per ms */
    audio = __atomic_exchange_n(&s_audio, 0, __ATOMIC_RELAXED);
    {
        unsigned fps = tenths((uint64_t)s_frames * 1000000000ull, span_ns);
        n = snprintf(line, sizeof line, "[PERF] %u frames %u.%u fps | ms/frame", s_frames, fps / 10, fps % 10);
        for (i = 0; i < XHW_PERF_N; i++) {
            unsigned t = tenths(s_acc[i], per_ms * s_frames);
            n += snprintf(line + n, sizeof line - (size_t)n, " %s %u.%u", k_names[i], t / 10, t % 10);
        }
        snprintf(line + n, sizeof line - (size_t)n, " | audio %u%% | %u draws %u verts per frame | cpu %u MHz",
                 (unsigned)(audio * 100 / span), s_draws / s_frames, s_verts / s_frames, (unsigned)(per_ms / 1000));
    }
    xhw_log(line);
    for (i = 0; i < XHW_PERF_N; i++) s_acc[i] = 0;
    s_frames = s_draws = s_verts = 0;
    s_t0 = now;
    s_ns0 = ns;
}
