/* xhw_prof.c - sampling profiler for the game thread, on the console.
 *
 * Built with -DXHW_PROF=1. A time-critical thread wakes every millisecond.
 * The game thread was then preempted by the clock interrupt, so its kernel
 * stack holds the interrupt frame the CPU pushed: EIP, CS (0x08), EFLAGS
 * with IF set. The first such frame above the saved stack pointer gives the
 * instruction the game was executing. Samples are counted per 64 bytes of
 * the XBE image; every XHW_PROF_SECS the hottest buckets go to the log as
 * [PROF] lines, which tools/xbox/prof_report.py folds into functions with
 * the link map. Samples outside the image (kernel, waits) are counted, not
 * placed. */
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xhw.h"
#include "xhw_internal.h"

#ifndef XHW_PROF
#define XHW_PROF 0
#endif
#ifndef XHW_PROF_SECS
#define XHW_PROF_SECS 20
#endif
#define BUCKET_SHIFT 6
#define TOP 48

static PKTHREAD s_game;

void xhw_prof_set_game_thread(void) { s_game = KeGetCurrentThread(); }

#if XHW_PROF
static uint16_t* s_hist;
static uint32_t s_nbuckets;
static uint32_t s_placed, s_outside, s_waiting, s_noframe;

/* the interrupted EIP from the game thread's kernel stack, 0 when none */
static ULONG sample(void) {
    ULONG* sp;
    ULONG* top;
    int i;
    if (!s_game || s_game->State != 1 /* Ready: preempted */) {
        s_waiting++;
        return 0;
    }
    sp = (ULONG*)s_game->KernelStack;
    top = (ULONG*)s_game->StackBase;
    if (!sp || !top || sp >= top) return 0;
    for (i = 0; i < 160 && sp + 2 < top; i++, sp++)
        if (sp[1] == 0x08 && (sp[2] & 0x202) == 0x202 && !(sp[2] & 0xFFC00000u)) return sp[0];
    s_noframe++;
    return 0;
}

static void report(void) {
    uint32_t i, k, best[TOP], bestn[TOP], total = s_placed + s_outside;
    memset(best, 0, sizeof best);
    memset(bestn, 0, sizeof bestn);
    for (i = 0; i < s_nbuckets; i++) {
        uint32_t c = s_hist[i];
        if (!c || c <= bestn[TOP - 1]) continue;
        for (k = TOP - 1; k > 0 && bestn[k - 1] < c; k--) {
            best[k] = best[k - 1];
            bestn[k] = bestn[k - 1];
        }
        best[k] = i;
        bestn[k] = c;
    }
    xhw_logf("[PROF] %u samples: %u in image, %u outside, %u while waiting, %u unreadable", total, s_placed,
             s_outside, s_waiting, s_noframe);
    for (k = 0; k < TOP && bestn[k]; k += 6) {
        char line[160];
        int n = 0, j;
        for (j = 0; j < 6 && k + j < TOP && bestn[k + j]; j++)
            n += snprintf(line + n, sizeof line - (size_t)n, " %08x:%u",
                          xhw_image_base + (best[k + j] << BUCKET_SHIFT), bestn[k + j]);
        xhw_logf("[PROF]%s", line);
    }
    memset(s_hist, 0, s_nbuckets * sizeof s_hist[0]);
    s_placed = s_outside = s_waiting = s_noframe = 0;
}

static DWORD WINAPI sampler(LPVOID arg) {
    uint64_t next = xhw_time_ns() + (uint64_t)XHW_PROF_SECS * 1000000000ull;
    (void)arg;
    for (;;) {
        ULONG eip;
        KIRQL old;
        Sleep(1);
        old = KeRaiseIrqlToDpcLevel();   /* the game thread can't run or exit while we read its stack */
        eip = sample();
        KfLowerIrql(old);
        if (eip >= xhw_image_base && eip < xhw_image_end) {
            uint32_t b = (eip - xhw_image_base) >> BUCKET_SHIFT;
            if (s_hist[b] != 0xFFFF) s_hist[b]++;
            s_placed++;
        } else if (eip) {
            s_outside++;
        }
        if (xhw_time_ns() >= next) {
            report();
            next = xhw_time_ns() + (uint64_t)XHW_PROF_SECS * 1000000000ull;
        }
    }
    return 0;
}

void xhw_prof_start(void) {
    HANDLE h;
    s_nbuckets = ((xhw_image_end - xhw_image_base) >> BUCKET_SHIFT) + 1;
    s_hist = (uint16_t*)calloc(s_nbuckets, sizeof s_hist[0]);
    if (!s_hist) return;
    h = CreateThread(NULL, 16 * 1024, sampler, NULL, 0, NULL);
    if (h) {
        SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);
        CloseHandle(h);
    }
    xhw_logf("[PROF] sampling the game thread every 1 ms, %u KB of buckets", s_nbuckets * 2 / 1024);
}
#else
void xhw_prof_start(void) {}
#endif
