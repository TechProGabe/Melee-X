/* xhw_pmc.c - the console's stall probes (docs/fps-plan.md step 1).
 *
 * Built with -DXHW_PMC=1 (with -DXHW_PROF=1 -DXHW_AUTOPAD=1: the round 1
 * probe build). None of it runs in xemu, which has no performance counters
 * and whose MSRs differ; a plain build has none of it.
 *
 * Performance counters: the Pentium III's two, programmed through
 * PerfEvtSel0/1 (MSRs 0x186/0x187: USR and OS so every ring counts, EN in
 * 0x186 starts both) and read with rdpmc at every [PERF] bucket switch
 * (xhw_perf.c), so each bucket gets its own sums. Each [PERF] period counts
 * one pair of events and logs them as a [PMC] line, then the next pair is
 * programmed; ten pairs in turn (the table below, P6 event codes).
 *
 * [CPU] at boot: CR0, CR3, CR4, MXCSR and its mask (is DAZ there), the x87
 * control word, CPUID 1 and 2, the MTRRs and PAT, and the page directory
 * entries of 0x00000000-0x1FFFFFFF and 0x80000000-0x8FFFFFFF (does the
 * kernel map memory with 4 MB pages?).
 *
 * Ablations (step 1.3), with `env MX_ABLATE=1` in the autopad script: every
 * second [PERF] period the next window of xgx_probe.h's list starts and an
 * [AB] line labels it. The game, the back end and the mixer ask
 * xhw_ablate(window); tools/xbox/probe_report.py averages by window. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "game/xgx_probe.h"
#include "xhw.h"
#include "xhw_internal.h"

#ifndef XHW_PMC
#define XHW_PMC 0
#endif

int xhw_running_in_xemu(void) {
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    return !(d & (1u << 1));   /* xemu's CPU model has no VME */
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t)hi << 32 | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline uint64_t rdpmc(uint32_t c) {
    uint32_t lo, hi;
    __asm__ volatile("rdpmc" : "=a"(lo), "=d"(hi) : "c"(c));
    return (uint64_t)(hi & 0xFF) << 32 | lo;   /* 40 bits */
}

static uint32_t mxcsr_mask(void);

/* MXCSR flush-to-zero (and denormals-are-zero where the CPU has it) on the
 * calling thread: the ablation's window 1, for the game and mixer threads */
void xhw_set_ftz(int on) {
    uint32_t m, daz = (mxcsr_mask() & 0x40u) ? 0x40u : 0;
    __asm__ volatile("stmxcsr %0" : "=m"(m));
    m = on ? m | 0x8000u | daz : m & ~(0x8000u | daz);
    __asm__ volatile("ldmxcsr %0" : : "m"(m));
}

#if XHW_PMC
static int s_on;        /* counters running (the console, not xemu) */
static int s_ablate;    /* MX_ABLATE=1 or a list: the windows rotate */
static int s_window;    /* the current ablation window */
static int s_list[32], s_nlist, s_at;   /* the rotation: window numbers, s_list[s_at] running */
static int s_pair;      /* the current event pair */
static uint32_t s_periods;
static uint64_t s_mark[2];
static uint64_t s_acc[XHW_PERF_N][2];

static struct {
    uint8_t ev0, um0, ev1, um1;
    const char *name0, *name1;
} k_pairs[] = {
    { 0x79, 0, 0xC0, 0, "CPU_CLK_UNHALTED", "INST_RETIRED" },
    { 0x86, 0, 0x48, 0, "IFU_MEM_STALL", "DCU_MISS_OUTSTANDING" },
    { 0x81, 0, 0x85, 0, "IFU_IFETCH_MISS", "ITLB_MISS" },
    { 0x28, 0x0F, 0x29, 0x0F, "L2_IFETCH", "L2_LD" },
    { 0x24, 0, 0x45, 0, "L2_LINES_IN", "DCU_LINES_IN" },
    { 0xC5, 0, 0xC4, 0, "BR_MISS_PRED_RETIRED", "BR_INST_RETIRED" },
    { 0xA2, 0, 0x11, 0, "RESOURCE_STALLS", "FP_ASSIST" },          /* FP_ASSIST: counter 1 only */
    { 0x14, 0, 0x13, 0, "CYCLES_DIV_BUSY", "DIV" },                /* counter 0 only, counter 1 only */
    { 0x03, 0, 0x05, 0, "LD_BLOCKS", "MISALIGN_MEM_REF" },
    { 0x07, 0, 0x4B, 0, "EMON_KNI_PREF_DISPATCHED", "EMON_KNI_PREF_MISS" },   /* umask: pref_umask() */
};
#define NPAIRS (int)(sizeof k_pairs / sizeof k_pairs[0])

#define SEL_USR (1u << 16)
#define SEL_OS (1u << 17)
#define SEL_EN (1u << 22)

static void program(int pair) {
    wrmsr(0x186, 0);   /* EN off: both stop */
    wrmsr(0x187, 0);
    wrmsr(0xC1, 0);
    wrmsr(0xC2, 0);
    wrmsr(0x187, k_pairs[pair].ev1 | (uint32_t)k_pairs[pair].um1 << 8 | SEL_USR | SEL_OS);
    wrmsr(0x186, k_pairs[pair].ev0 | (uint32_t)k_pairs[pair].um0 << 8 | SEL_USR | SEL_OS | SEL_EN);
    s_mark[0] = rdpmc(0);
    s_mark[1] = rdpmc(1);
}

/* The P6 event table gives 0x07/0x4B unit masks for prefetchnta, t1, t2 and
 * weakly-ordered stores, none for prefetcht0, which HSD_PREFETCH emits
 * (__builtin_prefetch). Count 1000 prefetcht0 under each mask and give
 * pair 10 the one that sees them. */
static void pref_umask(void) {
    static uint8_t buf[1000 * 32];
    uint32_t um, best = 0, n[4];
    int i;
    for (um = 0; um < 4; um++) {
        wrmsr(0x186, 0);
        wrmsr(0xC1, 0);
        wrmsr(0x186, 0x07 | um << 8 | SEL_USR | SEL_OS | SEL_EN);
        for (i = 0; i < 1000; i++) __asm__ volatile("prefetcht0 %0" : : "m"(buf[i * 32]));
        n[um] = (uint32_t)rdpmc(0);
        wrmsr(0x186, 0);
        if (n[um] > n[best]) best = um;
    }
    k_pairs[9].um0 = k_pairs[9].um1 = (uint8_t)best;
    xhw_logf("[PMC] 1000 prefetcht0 under 0x07 umask 0-3: %lu %lu %lu %lu -> umask %lu for pair 10", (unsigned long)n[0],
             (unsigned long)n[1], (unsigned long)n[2], (unsigned long)n[3], (unsigned long)best);
}

/* xhw_perf.c, at every bucket switch: what ran since the last one goes to `bucket` */
void xhw_pmc_charge(int bucket) {
    uint64_t c0, c1;
    if (!s_on) return;
    c0 = rdpmc(0);
    c1 = rdpmc(1);
    s_acc[bucket][0] += (c0 - s_mark[0]) & 0xFFFFFFFFFFull;
    s_acc[bucket][1] += (c1 - s_mark[1]) & 0xFFFFFFFFFFull;
    s_mark[0] = c0;
    s_mark[1] = c1;
}

/* xhw_perf.c, on the game thread right after each [PERF] line */
void xhw_pmc_period(void) {
    static const char* const k_names[XHW_PERF_N] = { "sim", "render", "dlist", "draw", "tex", "efb", "gpu", "vsync" };
    char line[400];
    int i, n;
    if (s_periods == 0) {
        const char* e = getenv("MX_ABLATE");
        s_ablate = e && *e;
        if (s_ablate && strchr(e, ',')) {   /* a list of windows */
            while (*e && s_nlist < 32) {
                int w = atoi(e);
                if (w >= 0 && w < XHW_AB_WINDOWS) s_list[s_nlist++] = w;
                while (*e && *e != ',') e++;
                if (*e) e++;
            }
        } else if (s_ablate && *e == '1') {   /* round 1's rotation */
            for (s_nlist = 0; s_nlist < 8; s_nlist++) s_list[s_nlist] = s_nlist;
        } else {
            s_ablate = 0;
        }
        if (s_ablate) {
            s_window = s_list[0];
            xhw_logf("[AB] %d %s", s_window, s_window == 0 ? "none" : "(first)");
            if (s_window == XHW_AB_FTZ) xhw_set_ftz(1);
        }
    }
    if (s_on) {
        xhw_pmc_charge(xhw_perf_bucket());
        n = snprintf(line, sizeof line, "[PMC] %d %s %s | k", s_pair + 1, k_pairs[s_pair].name0,
                     k_pairs[s_pair].name1);
        for (i = 0; i < XHW_PERF_N; i++) {
            n += snprintf(line + n, sizeof line - (size_t)n, " %s %lu %lu", k_names[i],
                          (unsigned long)(s_acc[i][0] / 1000), (unsigned long)(s_acc[i][1] / 1000));
            s_acc[i][0] = s_acc[i][1] = 0;
        }
        xhw_log(line);
        s_pair = (s_pair + 1) % NPAIRS;
        program(s_pair);
    }
    xgx_gpu_period_log();
    s_periods++;
    if (s_ablate && s_periods % 2 == 0) {
        static const char* const k_ab[XHW_AB_WINDOWS] = { "none", "ftz", "no shadow maps", "no reflection",
                                                          "no back end", "no dlist rechecks", "no audio",
                                                          "none (drift)", "no fill", "no EFB copies" };
        int was = s_window;
        s_at = (s_at + 1) % s_nlist;
        s_window = s_list[s_at];
        if (was == XHW_AB_FTZ || s_window == XHW_AB_FTZ) xhw_set_ftz(s_window == XHW_AB_FTZ);
        xhw_logf("[AB] %d %s", s_window, k_ab[s_window]);
    }
}

int xhw_ablate(int window) { return s_ablate && s_window == window; }
#else
void xhw_pmc_charge(int bucket) { (void)bucket; }
void xhw_pmc_period(void) {}
int xhw_ablate(int window) {
    (void)window;
    return 0;
}
#endif

/* MXCSR_MASK from an FXSAVE image (0: the CPU predates the field, 0xFFBF) */
static uint32_t mxcsr_mask(void) {
    static uint8_t area[512 + 16];
    static volatile uint32_t s_mask;   /* read once (xhw_cpu_probe, at boot) */
    uint8_t* fx = (uint8_t*)(((uintptr_t)area + 15) & ~(uintptr_t)15);
    uint32_t m;
    if (s_mask) return s_mask;
    memset(fx, 0, 512);
    __asm__ volatile("fxsave %0" : "=m"(*(uint8_t(*)[512])fx));
    memcpy(&m, fx + 28, 4);
    return s_mask = m ? m : 0xFFBFu;
}

/* Boot, test builds: the [CPU] block. MSRs only on the console. */
void xhw_cpu_probe(void) {
    uint32_t a, b, c, d, sig, feat;
    __asm__ volatile("cpuid" : "=a"(sig), "=b"(b), "=c"(c), "=d"(feat) : "a"(1));
    (void)sig;
#if XHW_TEST_BUILD
    {
    uint32_t cr0, cr3, cr4, mxcsr;
    uint16_t fcw;
    char line[400];
    int i, n;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    __asm__ volatile("fnstcw %0" : "=m"(fcw));
    xhw_logf("[CPU] cpuid 1: %08lx features %08lx | cr0 %08lx cr3 %08lx cr4 %08lx | mxcsr %08lx mask %08lx (DAZ %s) "
             "| fcw %04x%s",
             (unsigned long)sig, (unsigned long)feat, (unsigned long)cr0, (unsigned long)cr3, (unsigned long)cr4,
             (unsigned long)mxcsr, (unsigned long)mxcsr_mask(), (mxcsr_mask() & 0x40u) ? "yes" : "no", fcw,
             xhw_running_in_xemu() ? " | xemu: no MSRs" : "");
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(2));
    xhw_logf("[CPU] cpuid 2: %08lx %08lx %08lx %08lx", (unsigned long)a, (unsigned long)b, (unsigned long)c,
             (unsigned long)d);
    if (!xhw_running_in_xemu() && (feat & (1u << 5))) {   /* MSRs */
        if (feat & (1u << 12)) {                          /* MTRRs */
            uint64_t cap = rdmsr(0xFE);
            n = snprintf(line, sizeof line, "[CPU] mtrr cap %08lx def %08lx | var", (unsigned long)cap,
                         (unsigned long)rdmsr(0x2FF));
            for (i = 0; i < (int)(cap & 0xFF) && i < 8; i++) {
                uint64_t base = rdmsr(0x200 + 2 * i), mask = rdmsr(0x201 + 2 * i);
                if (mask & 0x800) n += snprintf(line + n, sizeof line - (size_t)n, " %08lx/%08lx:%u",
                                                (unsigned long)(base & ~0xFFFull), (unsigned long)(mask & ~0xFFFull),
                                                (unsigned)(base & 0xFF));
            }
            xhw_log(line);
            n = snprintf(line, sizeof line, "[CPU] mtrr fixed");
            {
                static const uint32_t k_fixed[] = { 0x250, 0x258, 0x259, 0x268, 0x269, 0x26A, 0x26B,
                                                    0x26C, 0x26D, 0x26E, 0x26F };
                for (i = 0; i < (int)(sizeof k_fixed / sizeof k_fixed[0]); i++) {
                    uint64_t v = rdmsr(k_fixed[i]);
                    n += snprintf(line + n, sizeof line - (size_t)n, " %08lx%08lx", (unsigned long)(v >> 32),
                                  (unsigned long)v);
                }
            }
            xhw_log(line);
        }
        if (feat & (1u << 16)) {   /* PAT */
            uint64_t pat = rdmsr(0x277);
            xhw_logf("[CPU] pat %08lx%08lx", (unsigned long)(pat >> 32), (unsigned long)pat);
        }
    }
    {   /* the page directory through the kernel's self-map */
        static const struct { uint32_t va, n; } k_ranges[] = { { 0x00000000u, 128 }, { 0x80000000u, 64 } };
        const uint32_t* pd = (const uint32_t*)0xC0300000u;
        int r;
        for (r = 0; r < 2; r++) {
            char map[160];
            uint32_t k;
            for (k = 0; k < k_ranges[r].n; k++) {
                const uint32_t* e = &pd[(k_ranges[r].va >> 22) + k];
                uint32_t v = MmIsAddressValid((PVOID)e) ? *e : 0;
                map[k] = !(v & 1) ? '.' : (v & 0x80) ? 'L' : 't';
            }
            map[k_ranges[r].n] = '\0';
            xhw_logf("[CPU] pde %08lx+4MB: %s (L: 4 MB page, t: page table, .: none)", (unsigned long)k_ranges[r].va,
                     map);
        }
    }
    }
#else
    (void)a;
    (void)c;
    (void)d;
#endif
#if XHW_PMC
    if (!xhw_running_in_xemu() && (feat & (1u << 5))) {
        s_on = 1;
        pref_umask();
        program(0);
        xhw_logf("[PMC] counters on: one pair per [PERF] period, %d pairs", NPAIRS);
    } else {
        xhw_logf("[PMC] off (xemu or no MSRs)");
    }
#endif
}
