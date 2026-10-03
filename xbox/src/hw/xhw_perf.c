/* xhw_perf.c - where the game thread's time goes, measured on the console.
 *
 * Every 5 s: frames presented, fps, and ms per frame in each bucket of
 * xhw.h's XHW_PERF_* list, plus the share of wall time the audio mixer
 * thread took. rdtsc is cheap enough to switch buckets per display list and
 * per draw. The mixer thread preempts the game thread, so its time also
 * shows up in whichever bucket was current, mostly LOGIC.
 *
 * Test builds also log a [PERFX] line after each [PERF]: the bucket sums in
 * microseconds and the raw counts, unrounded, for tools/xbox/icount_report.py.
 * Under xemu's -icount shift=1 a microsecond of guest time is 500 guest
 * instructions, so [PERFX] gives instructions per draw and per tick
 * (docs/fps-plan.md step 0.1); [CAL] at boot checks that scale. */
#include <stdio.h>

#include "xhw.h"
#include "xhw_internal.h"

#ifndef XHW_PMC
#define XHW_PMC 0
#endif

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
static uint32_t s_frames, s_draws, s_verts, s_ticks, s_renders;

static void charge(void) {
    uint64_t now = rdtsc();
    if (s_mark) s_acc[s_cur] += now - s_mark;
    s_mark = now;
#if XHW_PMC
    xhw_pmc_charge(s_cur);
#endif
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

int xhw_perf_bucket(void) { return *(volatile int*)&s_cur; }

/* Melee runs one simulation tick per pad poll queued since the last frame
 * (up to 5), then renders once: a slow frame makes the next one run more
 * ticks. "ticks" is that count per render pass. */
void xhw_perf_ticks(uint32_t n) {
    s_ticks += n;
    s_renders++;
}

/* the mixer preempts the game thread: its time is also charged to the
 * bucket the game was in, which [PERFX] lists so it can be taken out */
static uint64_t s_audio_in[XHW_PERF_N];

void xhw_perf_audio(uint64_t ticks) {
    __atomic_fetch_add(&s_audio, ticks, __ATOMIC_RELAXED);
    s_audio_in[*(volatile int*)&s_cur] += ticks;
}

/* tenths, for printing without floating point (pdclib's %f is unreliable) */
static unsigned tenths(uint64_t num, uint64_t den) { return den ? (unsigned)((num * 10 + den / 2) / den) : 0; }

void xhw_perf_frame(uint32_t draws, uint32_t verts) {
    static const char* const k_names[XHW_PERF_N] = { "sim", "render", "dlist", "draw", "tex", "efb", "gpu", "vsync" };
    uint64_t now = rdtsc(), ns = xhw_time_ns(), span, span_ns, audio, per_ms;
    char line[400];
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
        unsigned tk = tenths(s_ticks, s_renders);
        snprintf(line + n, sizeof line - (size_t)n,
                 " | %u.%u ticks per render | audio %u%% | %u draws %u verts per frame | cpu %u MHz", tk / 10, tk % 10,
                 (unsigned)(audio * 100 / span), s_draws / s_frames, s_verts / s_frames, (unsigned)(per_ms / 1000));
    }
    xhw_log(line);
#if XHW_TEST_BUILD
    {
        uint16_t fcw;
        uint32_t mxcsr;
        __asm__ volatile("fnstcw %0" : "=m"(fcw));
        __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
        n = snprintf(line, sizeof line, "[PERFX] %u frames %u draws %u verts %u ticks %u renders | us", s_frames,
                     s_draws, s_verts, s_ticks, s_renders);
        for (i = 0; i < XHW_PERF_N; i++)
            n += snprintf(line + n, sizeof line - (size_t)n, " %s %lu", k_names[i],
                          (unsigned long)(s_acc[i] * 1000 / per_ms));
        n += snprintf(line + n, sizeof line - (size_t)n, " | span %lu us audio %lu us in", (unsigned long)(span_ns / 1000),
                      (unsigned long)(audio * 1000 / per_ms));
        for (i = 0; i < XHW_PERF_N; i++) {
            n += snprintf(line + n, sizeof line - (size_t)n, " %lu", (unsigned long)(s_audio_in[i] * 1000 / per_ms));
            s_audio_in[i] = 0;
        }
        snprintf(line + n, sizeof line - (size_t)n, " | fcw %04x mxcsr %08lx", fcw, (unsigned long)mxcsr);
        xhw_log(line);
    }
#endif
#if XHW_PMC
    xhw_pmc_period();
#endif
    for (i = 0; i < XHW_PERF_N; i++) s_acc[i] = 0;
    s_frames = s_draws = s_verts = s_ticks = s_renders = 0;
    s_t0 = now;
    s_ns0 = ns;
}

/* Boot, test builds: a loop of a known instruction count against rdtsc and
 * the kernel's clock. Under xemu -icount shift=N it should take
 * 2^N ns per instruction; on the console it is just a short spin. */
void xhw_perf_calibrate(void) {
#if XHW_TEST_BUILD
    const uint32_t iters = 10000000;   /* dec + jnz: 2 instructions each */
    uint64_t t0, t1, n0, n1;
    uint32_t c = iters;
    n0 = xhw_time_ns();
    t0 = rdtsc();
    __asm__ volatile("1: dec %0\n\tjnz 1b" : "+r"(c));
    t1 = rdtsc();
    n1 = xhw_time_ns();
    xhw_logf("[CAL] %lu instructions: %lu us, %lu kTSC", (unsigned long)(iters * 2),
             (unsigned long)((n1 - n0) / 1000), (unsigned long)((t1 - t0) / 1000));
#endif
}
