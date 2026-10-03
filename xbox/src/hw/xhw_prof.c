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
 * [PROFS] buckets count only the samples taken while [PERF]'s current
 * bucket was the simulation (xhw_perf_bucket), to tell its cost from the
 * render pass's in the shared HSD code (animation, matrices).
 *
 * The whole match: every bucket (all samples and the simulation's) is also
 * counted in 32-bit histograms the periodic report doesn't reset. They
 * restart at each scene's entry (xhw_prof_scene_enter) and are written once
 * at the match's end (xhw_prof_match_end, TIME!/GAME!): as [PROFH] lines in
 * autopad builds, and as E:\UDATA\4d580001\prof.bin in every profiler
 * build (prof_report.py --full reads either). The game thread only raises a
 * flag; the sampler, which already logs from its own thread, does the work.
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
static uint16_t* s_hist_sim;   /* the samples taken during the simulation ticks */
static uint32_t s_nbuckets;
static uint32_t s_placed, s_outside, s_waiting, s_noframe, s_sim_placed;

/* the whole match: not reset by report() */
#ifndef XHW_AUTOPAD
#define XHW_AUTOPAD 0
#endif
#define PROFBIN_MAGIC 0x4850584Du   /* "MXPH" */
#define PROFBIN_VERSION 1
typedef struct {   /* prof.bin's header; then u32 all[nbuckets], u32 sim[nbuckets] */
    uint32_t magic, version, image_base, bucket_shift, nbuckets;
    uint32_t match;                                  /* 1 for the boot's first match */
    uint32_t ms;                                     /* from the scene's entry to the match's end */
    uint32_t placed, outside, waiting, noframe, sim_placed;
} ProfBinHeader;
static uint32_t* s_whole;      /* the code's buckets only (code_end) */
static uint32_t* s_whole_sim;
static uint32_t s_wn;
static ProfBinHeader s_wh;     /* its counters run with the histograms */
static uint64_t s_whole_t0;
static volatile int s_whole_reset, s_whole_dump;   /* raised by the game thread, served by the sampler */

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
static char s_rep[24576];
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

/* the hottest buckets of a histogram as `tag` lines */
static void report_hist(const char* tag, uint16_t* hist) {
    uint32_t i, k, best[TOP], bestn[TOP];
    memset(best, 0, sizeof best);
    memset(bestn, 0, sizeof bestn);
    for (i = 0; i < s_nbuckets; i++) {
        uint32_t c = hist[i];
        if (!c || c <= bestn[TOP - 1]) continue;
        for (k = TOP - 1; k > 0 && bestn[k - 1] < c; k--) {
            best[k] = best[k - 1];
            bestn[k] = bestn[k - 1];
        }
        best[k] = i;
        bestn[k] = c;
    }
    for (k = 0; k < TOP && bestn[k]; k += 6) {
        int j;
        rep("%s", tag);
        for (j = 0; j < 6 && k + j < TOP && bestn[k + j]; j++)
            rep(" %08x:%u", xhw_image_base + (best[k + j] << BUCKET_SHIFT), bestn[k + j]);
        rep("\n");
    }
    memset(hist, 0, s_nbuckets * sizeof hist[0]);
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
    if (s_hist_sim) {
        rep("[PROFS] %u samples in the simulation\n", s_sim_placed);
        report_hist("[PROFS]", s_hist_sim);
    }
    s_placed = s_outside = s_waiting = s_noframe = s_sim_placed = 0;
    report_callers("[PROFL]", "in memcpy/memset/memcmp/memmove, by caller", s_libc);
    report_callers("[PROFC]", "by caller (one frame up)", s_callers);
    if (s_rlen && s_rep[s_rlen - 1] == '\n') s_rlen--;
    s_rep[s_rlen] = '\0';
    xhw_log(s_rep);
}

/* the end of the XBE's executable sections (.text): the game thread runs
 * nothing past it, and the whole image (.bss included) would be ~1.7x the
 * buckets. XBE header: +0x11C section count, +0x120 their headers (56 bytes:
 * flags, address, size, ...; flag 4 = executable). */
static uint32_t code_end(void) {
    const uint8_t* xbe = (const uint8_t*)xhw_image_base;
    const uint8_t* sh = (const uint8_t*)*(const uint32_t*)(xbe + 0x120);
    uint32_t n = *(const uint32_t*)(xbe + 0x11C), i, end = 0;
    for (i = 0; i < n && i < 64; i++, sh += 56) {
        uint32_t flags = *(const uint32_t*)sh, va = *(const uint32_t*)(sh + 4), size = *(const uint32_t*)(sh + 8);
        if ((flags & 4) && va + size > end) end = va + size;
    }
    return end > xhw_image_base && end <= xhw_image_end ? end : xhw_image_end;
}

void xhw_prof_scene_enter(void) { s_whole_reset = 1; }
void xhw_prof_match_end(void) { s_whole_dump = 1; }

static void whole_clear(void) {
    if (!s_whole) return;
    memset(s_whole, 0, s_wn * sizeof s_whole[0]);
    memset(s_whole_sim, 0, s_wn * sizeof s_whole_sim[0]);
    s_wh.placed = s_wh.outside = s_wh.waiting = s_wh.noframe = s_wh.sim_placed = 0;
    s_whole_t0 = xhw_time_ns();
}

/* s_rep out as one log write once it is nearly full, and at the end */
static void rep_flush(int force) {
    if (!s_rlen || (!force && s_rlen < (int)sizeof s_rep - 1024)) return;
    if (s_rep[s_rlen - 1] == '\n') s_rlen--;
    s_rep[s_rlen] = '\0';
    xhw_log(s_rep);
    s_rlen = 0;
}

/* One histogram as [PROFH] lines: "<tag> <first bucket> <count>,<count>,..."
 * in hex, where "+n" stands for n empty buckets; lines stay under ~500
 * bytes. Returns the sum of the counts (the end line's check). */
static uint32_t whole_lines(const char* tag, const uint32_t* h) {
    uint32_t i = 0, sum = 0, gap;
    int line = -1;   /* s_rlen where the open line started, -1: none */
    while (i < s_wn) {
        if (!h[i]) {
            for (gap = 0; i < s_wn && !h[i]; i++) gap++;
            if (line < 0) continue;
            if (i < s_wn && s_rlen - line <= 480) {
                rep(",+%x", gap);
            } else {
                rep("\n");
                line = -1;
            }
            continue;
        }
        if (line >= 0 && s_rlen - line > 480) {
            rep("\n");
            line = -1;
        }
        if (line < 0) {
            rep_flush(0);
            line = s_rlen;
            rep("[PROFH] %s %x %x", tag, i, h[i]);
        } else {
            rep(",%x", h[i]);
        }
        sum += h[i++];
    }
    if (line >= 0) rep("\n");
    return sum;
}

/* E:\UDATA\4d580001\prof.bin: the header, then both histograms */
static void whole_file(void) {
    DWORD w;
    HANDLE f = CreateFileA(XHW_UDATA_DIR "prof.bin", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           NULL);
    if (f == INVALID_HANDLE_VALUE) {
        xhw_log("[PROFH] could not create prof.bin");
        return;
    }
    WriteFile(f, &s_wh, sizeof s_wh, &w, NULL);
    WriteFile(f, s_whole, s_wn * sizeof s_whole[0], &w, NULL);
    WriteFile(f, s_whole_sim, s_wn * sizeof s_whole_sim[0], &w, NULL);
    xhw_flush_handle(f);
    CloseHandle(f);
}

static void whole_dump(void) {
    uint64_t t = xhw_time_ns();
    s_wh.match++;
    s_wh.ms = (uint32_t)((t - s_whole_t0) / 1000000u);
    whole_file();
    if (XHW_AUTOPAD) {
        uint32_t all, sim;
        s_rlen = 0;
        rep("[PROFH] begin match %u: base %08x shift %u buckets %x, %u ms, %u samples: %u in image, %u outside, "
            "%u while waiting, %u unreadable, %u in the simulation\n",
            s_wh.match, xhw_image_base, BUCKET_SHIFT, s_wn, s_wh.ms, s_wh.placed + s_wh.outside,
            s_wh.placed, s_wh.outside, s_wh.waiting, s_wh.noframe, s_wh.sim_placed);
        all = whole_lines("all", s_whole);
        sim = whole_lines("sim", s_whole_sim);
        rep("[PROFH] end match %u: all %u sim %u\n", s_wh.match, all, sim);
        rep_flush(1);
    }
    xhw_logf("[PROF] whole-match profile %u written: %u samples in %u ms (prof.bin%s), %u ms to write",
             s_wh.match, s_wh.placed + s_wh.outside, s_wh.ms, XHW_AUTOPAD ? ", [PROFH]" : "",
             (unsigned)((xhw_time_ns() - t) / 1000000u));
}

static DWORD WINAPI sampler(LPVOID arg) {
    uint64_t next = xhw_time_ns() + (uint64_t)XHW_PROF_SECS * 1000000000ull;
    (void)arg;
    for (;;) {
        ULONG eip, esp = 0, caller = 0;
        KIRQL old;
        Sleep(1);
        if (s_whole_dump) {   /* before a reset raised with it: the match's end comes first */
            s_whole_dump = 0;
            if (s_whole) whole_dump();
            whole_clear();
        }
        if (s_whole_reset) {
            s_whole_reset = 0;
            whole_clear();
        }
        old = KeRaiseIrqlToDpcLevel();   /* the game thread can't run or exit while we read its stack */
        if (!s_game || s_game->State != 1) {
            s_waiting++;
            s_wh.waiting++;
            eip = 0;
        } else {
            eip = xhw_thread_eip(s_game, &esp);
            if (!eip) {
                s_noframe++;
                s_wh.noframe++;
            } else if (esp) {
                caller = caller_of(esp);
            }
        }
        KfLowerIrql(old);
        if (eip >= xhw_image_base && eip < xhw_image_end) {
            uint32_t b = (eip - xhw_image_base) >> BUCKET_SHIFT;
            int sim = xhw_perf_bucket() == XHW_PERF_LOGIC;
            if (s_hist[b] != 0xFFFF) s_hist[b]++;
            s_placed++;
            if (s_hist_sim && sim) {
                if (s_hist_sim[b] != 0xFFFF) s_hist_sim[b]++;
                s_sim_placed++;
            }
            if (s_whole && b < s_wn) {
                s_whole[b]++;
                s_wh.placed++;
                if (sim) {
                    s_whole_sim[b]++;
                    s_wh.sim_placed++;
                }
            }
            if (caller) {
                ct_add(s_callers, caller);
                if (eip >= s_libc_lo && eip < s_libc_hi) ct_add(s_libc, caller);
            }
        } else if (eip) {
            s_outside++;
            s_wh.outside++;
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
    s_hist_sim = (uint16_t*)calloc(s_nbuckets, sizeof s_hist_sim[0]);
    s_libc = (CallerTable*)calloc(1, sizeof *s_libc);
    s_callers = (CallerTable*)calloc(1, sizeof *s_callers);
    s_wn = (code_end() - xhw_image_base + (1u << BUCKET_SHIFT) - 1) >> BUCKET_SHIFT;
    s_whole = (uint32_t*)calloc(s_wn, sizeof s_whole[0]);
    s_whole_sim = (uint32_t*)calloc(s_wn, sizeof s_whole_sim[0]);
    if (!s_whole || !s_whole_sim) {   /* the periodic report still works without them */
        free(s_whole);
        free(s_whole_sim);
        s_whole = s_whole_sim = NULL;
    }
    s_wh.magic = PROFBIN_MAGIC;
    s_wh.version = PROFBIN_VERSION;
    s_wh.image_base = xhw_image_base;
    s_wh.bucket_shift = BUCKET_SHIFT;
    s_wh.nbuckets = s_wn;
    s_whole_t0 = xhw_time_ns();
    DeleteFileA(XHW_UDATA_DIR "prof.bin");   /* an older boot's would read as this one's */
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
    xhw_logf("[PROF] sampling the game thread every 1 ms, %u KB of buckets (%u KB for the whole match), "
             "callers of %08x-%08x",
             s_nbuckets * 4 / 1024, s_whole ? s_wn * 8 / 1024 : 0, s_libc_lo, s_libc_hi);
}
#else
void xhw_prof_start(void) {}
void xhw_prof_scene_enter(void) {}
void xhw_prof_match_end(void) {}
#endif
