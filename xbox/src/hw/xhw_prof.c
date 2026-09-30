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
 * placed.
 *
 * Callers: everything runs in ring 0, so the interrupt pushed no stack
 * switch and the interrupted ESP is just above that frame. The first word
 * there that points into the image right after a call instruction is the
 * return address of the function being executed (or, inside a function that
 * has already made a call, of its caller: one frame up either way). Those are
 * counted too: [PROFL] for samples inside memcpy/memset/memcmp/memmove (who
 * copies), [PROFC] for every sample (the hottest call sites one level up).
 *
 * xhw_thread_eip() is always built: the watchdog uses it to say where the
 * game thread is stuck. */
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <stdarg.h>
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
#ifndef XHW_PROF_TOP
#define XHW_PROF_TOP 192   /* buckets and call sites per report */
#endif
#define TOP XHW_PROF_TOP

static PKTHREAD s_game;

void xhw_prof_set_game_thread(void) { s_game = KeGetCurrentThread(); }
void* xhw_game_thread(void) { return s_game; }

/* the interrupted EIP from a thread's kernel stack, 0 when none */
unsigned long xhw_thread_eip(void* kthread, unsigned long* esp_out) {
    PKTHREAD t = (PKTHREAD)kthread;
    ULONG* sp;
    ULONG* top;
    int i;
    if (!t || t->State != 1 /* Ready: preempted */) return 0;
    sp = (ULONG*)t->KernelStack;
    top = (ULONG*)t->StackBase;
    if (!sp || !top || sp >= top || ((ULONG)sp & 3)) return 0;
    for (i = 0; i < 160 && sp + 2 < top; i++, sp++)
        if (sp[1] == 0x08 && (sp[2] & 0x202) == 0x202 && !(sp[2] & 0xFFC00000u)) {
            if (esp_out) *esp_out = (ULONG)(sp + 3);
            return sp[0];
        }
    return 0;
}

#if XHW_PROF
static uint16_t* s_hist;
static uint32_t s_nbuckets;
static uint32_t s_placed, s_outside, s_waiting, s_noframe;

/* return address -> samples, open addressing */
#define CT_SIZE 4096
typedef struct { uint32_t addr, n; } CallerCount;
typedef struct {
    CallerCount e[CT_SIZE];
    uint32_t used, samples, lost;
} CallerTable;
static CallerTable* s_libc;     /* samples inside the string routines, by caller */
static CallerTable* s_callers;  /* every placed sample, one frame up */
static uint32_t s_libc_lo, s_libc_hi;

static void ct_add(CallerTable* t, uint32_t addr) {
    uint32_t h = (addr * 2654435761u) >> 20, k;
    t->samples++;
    for (k = 0; k < 16; k++, h = (h + 1) & (CT_SIZE - 1)) {
        CallerCount* c = &t->e[h];
        if (c->addr == addr) {
            c->n++;
            return;
        }
        if (!c->addr) {
            if (t->used >= CT_SIZE * 3 / 4) break;
            c->addr = addr;
            c->n = 1;
            t->used++;
            return;
        }
    }
    t->lost++;
}

static int in_image(uint32_t a) { return a >= xhw_image_base + 0x1000 && a < xhw_image_end; }

/* `ret` is a return address if a call instruction ends right before it:
 * E8 rel32, or FF /2 (call through a register or memory operand) */
static int after_call(uint32_t ret) {
    const uint8_t* p = (const uint8_t*)ret;
    /* at DPC level: a page of the image that isn't mapped (its tail, past
     * the last section) would bugcheck, not fault */
    if (!in_image(ret - 7) || !MmIsAddressValid((PVOID)(ret - 7)) || !MmIsAddressValid((PVOID)(ret - 1))) return 0;
    if (p[-5] == 0xE8) return 1;
    if (p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0) return 1;                   /* call reg */
    if (p[-3] == 0xFF && ((p[-2] & 0xF8) == 0x50 || p[-2] == 0x14)) return 1; /* call [reg+d8], [sib] */
    if (p[-4] == 0xFF && p[-3] == 0x54) return 1;                             /* call [sib+d8] */
    if (p[-6] == 0xFF && (p[-5] == 0x15 || (p[-5] & 0xF8) == 0x90)) return 1; /* call [abs], [reg+d32] */
    if (p[-7] == 0xFF && p[-6] == 0x94) return 1;                             /* call [sib+d32] */
    return 0;
}

static uint32_t caller_of(uint32_t esp) {
    const ULONG* sp = (const ULONG*)esp;
    const ULONG* top = (const ULONG*)s_game->StackBase;
    int i;
    for (i = 0; i < 24 && sp + i < top; i++)
        if (in_image(sp[i]) && after_call(sp[i])) return sp[i];
    return 0;
}

/* The report is written as one block: one log write and one flush. Line by
 * line, each flushed (the log does that for the first 600 frames), it took
 * ~10 s on the console and starved the disc image reads on the same disk:
 * the character select load stalled long enough to trip the watchdog. */
static char s_rep[16384];
static int s_rlen;

static void rep(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void rep(const char* fmt, ...) {
    va_list ap;
    int n;
    if (s_rlen >= (int)sizeof s_rep - 1) return;
    va_start(ap, fmt);
    n = vsnprintf(s_rep + s_rlen, sizeof s_rep - (size_t)s_rlen, fmt, ap);
    va_end(ap);
    if (n > 0) s_rlen += n;
    if (s_rlen > (int)sizeof s_rep - 1) s_rlen = (int)sizeof s_rep - 1;
}

static void report_callers(const char* tag, const char* what, CallerTable* t) {
    uint32_t i, k;
    CallerCount best[TOP];
    memset(best, 0, sizeof best);
    for (i = 0; i < CT_SIZE; i++) {
        CallerCount c = t->e[i];
        if (!c.addr || c.n <= best[TOP - 1].n) continue;
        for (k = TOP - 1; k > 0 && best[k - 1].n < c.n; k--) best[k] = best[k - 1];
        best[k] = c;
    }
    rep("%s %u samples %s, %u call sites, %u not placed\n", tag, t->samples, what, t->used, t->lost);
    for (k = 0; k < TOP && best[k].n; k += 6) {
        int j;
        rep("%s", tag);
        for (j = 0; j < 6 && k + j < TOP && best[k + j].n; j++) rep(" %08x:%u", best[k + j].addr, best[k + j].n);
        rep("\n");
    }
    memset(t, 0, sizeof *t);
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
    s_rlen = 0;
    rep("[PROF] %u samples: %u in image, %u outside, %u while waiting, %u unreadable\n", total, s_placed,
        s_outside, s_waiting, s_noframe);
    for (k = 0; k < TOP && bestn[k]; k += 6) {
        int j;
        rep("[PROF]");
        for (j = 0; j < 6 && k + j < TOP && bestn[k + j]; j++)
            rep(" %08x:%u", xhw_image_base + (best[k + j] << BUCKET_SHIFT), bestn[k + j]);
        rep("\n");
    }
    memset(s_hist, 0, s_nbuckets * sizeof s_hist[0]);
    s_placed = s_outside = s_waiting = s_noframe = 0;
    report_callers("[PROFL]", "in memcpy/memset/memcmp/memmove, by caller", s_libc);
    report_callers("[PROFC]", "by caller (one frame up)", s_callers);
    if (s_rlen && s_rep[s_rlen - 1] == '\n') s_rlen--;
    s_rep[s_rlen] = '\0';
    xhw_log(s_rep);
}

static DWORD WINAPI sampler(LPVOID arg) {
    uint64_t next = xhw_time_ns() + (uint64_t)XHW_PROF_SECS * 1000000000ull;
    (void)arg;
    for (;;) {
        ULONG eip, esp = 0, caller = 0;
        KIRQL old;
        Sleep(1);
        old = KeRaiseIrqlToDpcLevel();   /* the game thread can't run or exit while we read its stack */
        if (!s_game || s_game->State != 1) {
            s_waiting++;
            eip = 0;
        } else {
            eip = xhw_thread_eip(s_game, &esp);
            if (!eip) s_noframe++;
            else if (esp) caller = caller_of(esp);
        }
        KfLowerIrql(old);
        if (eip >= xhw_image_base && eip < xhw_image_end) {
            uint32_t b = (eip - xhw_image_base) >> BUCKET_SHIFT;
            if (s_hist[b] != 0xFFFF) s_hist[b]++;
            s_placed++;
            if (caller) {
                ct_add(s_callers, caller);
                if (eip >= s_libc_lo && eip < s_libc_hi) ct_add(s_libc, caller);
            }
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
    uint32_t fn[4] = { (uint32_t)&memcpy, (uint32_t)&memmove, (uint32_t)&memset, (uint32_t)&memcmp }, i;
    s_nbuckets = ((xhw_image_end - xhw_image_base) >> BUCKET_SHIFT) + 1;
    s_hist = (uint16_t*)calloc(s_nbuckets, sizeof s_hist[0]);
    s_libc = (CallerTable*)calloc(1, sizeof *s_libc);
    s_callers = (CallerTable*)calloc(1, sizeof *s_callers);
    if (!s_hist || !s_libc || !s_callers) return;
    s_libc_lo = 0xFFFFFFFFu;
    for (i = 0; i < 4; i++) {
        if (fn[i] < s_libc_lo) s_libc_lo = fn[i];
        if (fn[i] + 0x80 > s_libc_hi) s_libc_hi = fn[i] + 0x80;   /* xhw_string.c: the four, back to back */
    }
    /* 64 KB: report() writes boot.log from this thread, and on the Xbox the
     * kernel's file-system path runs on the caller's stack */
    h = CreateThread(NULL, 64 * 1024, sampler, NULL, 0, NULL);
    if (h) {
        SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);
        CloseHandle(h);
    }
    xhw_logf("[PROF] sampling the game thread every 1 ms, %u KB of buckets, callers of %08x-%08x",
             s_nbuckets * 2 / 1024, s_libc_lo, s_libc_hi);
}
#else
void xhw_prof_start(void) {}
#endif
