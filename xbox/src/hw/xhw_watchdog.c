/* xhw_watchdog.c - hang dumper (from OpenCrossing-Xbox xbox_watchdog.c).
 *
 * A 1 Hz thread started first thing in main_body() watches the retrace
 * counter (VIWaitForRetrace calls, xsdk_frame_count). It fires once if there
 * is no frame XHW_WATCHDOG_BOOT_SECS after boot, or if frames stop for
 * XHW_WATCHDOG_SECS later. It also watches presents (GXCopyDisp): a game
 * loop that keeps calling VIWaitForRetrace but never draws (HSD's XFB waits)
 * freezes the picture while the retrace count runs on. After
 * XHW_WATCHDOG_PRESENT_SECS of that it reports to the logs only, and after
 * a minute onto the screen too. Every thread in the process is dumped:
 * state, wait reason, where the game thread was interrupted, and each stack
 * word that points into the XBE image (a heuristic backtrace; frame pointers
 * are not reliable under -O2). The report goes to COM1, boot.log,
 * E:\UDATA\4d580001\hang.log and the screen; this thread writes and
 * flushes them itself, waiting at most ~0.5 s for the log lock. Symbolize
 * with tools/xbox/sym.py. Each tick also flushes boot.log lines still
 * pending (xhw_log_sync), and every XHW_HEARTBEAT_SECS a [BEAT] line goes
 * to the log, so the last seconds before a freeze are on disk. A stall
 * that ends (a long load) is logged and the game gets the screen back.
 * Kill switch: -DXHW_WATCHDOG=0. */
#include <hal/debug.h>
#include <pbkit/pbkit.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "xgx.h"
#include "xhw.h"
#include "xhw_internal.h"

#ifndef XHW_WATCHDOG
#define XHW_WATCHDOG 1
#endif
#ifndef XHW_WATCHDOG_SECS
#define XHW_WATCHDOG_SECS 6
#endif
#ifndef XHW_WATCHDOG_BOOT_SECS
#define XHW_WATCHDOG_BOOT_SECS 45
#endif
#ifndef XHW_WATCHDOG_PRESENT_SECS
#define XHW_WATCHDOG_PRESENT_SECS 10
#endif
/* Every N seconds log retraces, presents and free RAM; 0 disables. */
#ifndef XHW_HEARTBEAT_SECS
#define XHW_HEARTBEAT_SECS 5
#endif

#define WD_MAX_THREADS 16
#define WD_MAX_WORDS 40

typedef struct {
    PKTHREAD t;
    UCHAR state, wait;
    SCHAR prio;
    int self, game, n;
    ULONG eip;
    ULONG words[WD_MAX_WORDS];
} Snap;

static Snap s_snap[WD_MAX_THREADS];

/* Kernel stacks are nonpaged and committed from KernelStack (the saved ESP of
 * a thread that is not running) up to StackBase, so the scan can't fault.
 * Only copying happens at DPC level; formatting and I/O come after. */
static void snap_thread(Snap* o, PKTHREAD t, int self) {
    ULONG* sp = (ULONG*)t->KernelStack;
    ULONG* top = (ULONG*)t->StackBase;
    o->t = t;
    o->state = t->State;
    o->wait = t->WaitReason;
    o->prio = t->Priority;
    o->self = self;
    o->game = t == (PKTHREAD)xhw_game_thread();
    o->eip = xhw_thread_eip(t, NULL);
    o->n = 0;
    if (self || !sp || !top || sp >= top || top - sp > 0x40000) return;
    for (; sp < top && o->n < WD_MAX_WORDS; sp++) {
        ULONG v = *sp;
        if (v >= xhw_image_base + 0x1000 && v < xhw_image_end) o->words[o->n++] = v;
    }
}

static char s_report[8192];
static int s_rlen;

static void rep(const char* fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(s_report + s_rlen, sizeof s_report - (size_t)s_rlen, fmt, ap);
    va_end(ap);
    if (n > 0) s_rlen += n;
    if (s_rlen > (int)sizeof s_report - 1) s_rlen = (int)sizeof s_report - 1;
}

/* the log tail from before the report went into the log */
static char s_hang_tail[4096];
static size_t s_hang_tail_len;

/* last `lines` lines of that tail, each cut to `cols` */
static void screen_tail(int lines, int cols) {
    static char tail[4096];
    char* p;
    char* start[64];
    int n = 0, i;
    memcpy(tail, s_hang_tail, s_hang_tail_len);
    tail[s_hang_tail_len < sizeof tail ? s_hang_tail_len : sizeof tail - 1] = '\0';
    for (p = tail; *p;) {
        if (n < 64) {
            start[n++] = p;
        } else {
            memmove(start, start + 1, sizeof start - sizeof start[0]);
            start[63] = p;
        }
        p = strchr(p, '\n');
        if (!p) break;
        *p++ = '\0';
    }
    for (i = n > lines ? n - lines : 0; i < n; i++) debugPrint("%.*s\n", cols, start[i]);
}

static void write_hang_log(void) {
    HANDLE h = CreateFileA(XHW_UDATA_DIR "hang.log", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(h, s_hang_tail, (DWORD)s_hang_tail_len, &w, NULL);
        WriteFile(h, s_report, (DWORD)s_rlen, &w, NULL);
        xhw_flush_handle(h);
        CloseHandle(h);
    }
}

static int s_screen;   /* a report is on the screen */

static void dump_all(const char* why, int screen) {
    PKTHREAD me = KeGetCurrentThread();
    PKPROCESS p = me->ApcState.Process;
    PLIST_ENTRY e;
    int i, j, n = 0;
    KIRQL old = KeRaiseIrqlToDpcLevel();   /* freeze the thread list while walking it */
    for (e = p->ThreadListHead.Flink; e != &p->ThreadListHead && n < WD_MAX_THREADS; e = e->Flink) {
        PKTHREAD t = CONTAINING_RECORD(e, KTHREAD, ThreadListEntry);
        snap_thread(&s_snap[n++], t, t == me);
    }
    KfLowerIrql(old);

    s_rlen = 0;
    rep("[WDOG] %s (retrace %u, presented %u), %d threads, free %u KB, MEM1+ARAM %u KB\n", why, xhw_frame_count(),
        xgx_present_count(), n, xhw_mem_free_kb(), xhw_lazy_committed_kb());
    for (i = 0; i < n; i++) {
        const Snap* o = &s_snap[i];
        rep("[WDOG] thread %p%s state %u wait %u prio %d eip %08lx\n[WDOG]  ", (void*)o->t,
            o->self ? " (watchdog)" : o->game ? " (game)" : "", (unsigned)o->state, (unsigned)o->wait, (int)o->prio,
            o->eip);
        for (j = 0; j < o->n; j++) rep(" %08lx", o->words[j]);
        rep("\n");
    }
    rep("[WDOG] end\n");

    /* COM1 first, lock-free: the log lock itself may be what is stuck */
    xhw_com1_raw(s_report, (size_t)s_rlen);

    /* boot.log next (without COM1 again): the report the user pulls */
    s_hang_tail_len = xhw_log_tail(s_hang_tail, sizeof s_hang_tail);
    xhw_log_try_file(s_report);

    if (!screen) {
        write_hang_log();
        return;
    }
    /* screen next: the file I/O below can block if the hang involves the disk */
    pb_show_debug_screen();
    s_screen = 1;
    debugClearScreen();
    debugPrint("Melee-X: %s at frame %u\n", why, xhw_frame_count());
    debugPrint("Log: " XHW_UDATA_DIR "hang.log + boot.log\n\n");
    screen_tail(14, 76);
    debugPrint("\n");
    for (i = 0; i < n; i++) {
        const Snap* o = &s_snap[i];
        if (o->self) continue;
        debugPrint("t%d%s s%u w%u eip %08lx:", i, o->game ? "*" : "", (unsigned)o->state, (unsigned)o->wait, o->eip);
        for (j = 0; j < o->n && j < 8; j++) debugPrint(" %08lx", o->words[j]);
        debugPrint("\n");
    }

    write_hang_log();
}

static volatile int s_disabled, s_busy;

/* A stall that ends (a long load) gives the picture back to the game. */
static void resumed(const char* what, unsigned secs) {
    char line[120];
    snprintf(line, sizeof line, "[WDOG] %s again after %u s", what, secs);
    xhw_log_try(line);
    if (s_screen && !s_disabled) pb_show_front_screen();
    s_screen = 0;
}

void xhw_watchdog_busy(int on) { s_busy = on; }

/* an error card owns the screen for good */
void xhw_watchdog_disable(void) { s_disabled = 1; }

static void watchdog_body(void* arg) {
    unsigned last = 0, still = 0, secs = 0, fired = 0;
    unsigned last_p = 0, pstill = 0, pfired = 0, last_pf = 0;
    (void)arg;
    for (;;) {
        unsigned f, p;
        Sleep(1000);
        secs++;
        if (s_disabled) continue;
        xhw_log_sync();
        f = xhw_frame_count();
        p = xgx_present_count();
        if (XHW_HEARTBEAT_SECS && secs % XHW_HEARTBEAT_SECS == 0 && !s_busy) {
            /* through the log (COM1 + boot.log) when its lock is free within
             * ~0.5 s, else written anyway: a thread stuck holding the lock
             * must not silence the heartbeat. Not during a screenshot: it
             * would land inside an [FBDUMP] line. */
            char line[200];
            snprintf(line, sizeof line, "[BEAT] %us: retrace %u, presented %u, free %u KB, MEM1+ARAM %u KB, tex pool %u KB free",
                     secs, f, p, xhw_mem_free_kb(), xhw_lazy_committed_kb(), xgx_tex_pool_free_kb());
            xhw_log_try(line);
        }
        if (f == 0) {
            if (secs >= XHW_WATCHDOG_BOOT_SECS && !fired) {
                dump_all("no first frame after boot", 1);
                fired = 1;
            }
            continue;
        }
        /* presents stopped while the retrace count runs on */
        if (p != last_p || s_busy || f == last_pf) {
            if (p != last_p && pfired) resumed("presents", pstill);
            if (p != last_p) pfired = 0;
            last_p = p;
            pstill = 0;
        } else {
            pstill++;
            if (pstill >= XHW_WATCHDOG_PRESENT_SECS && !pfired) {
                dump_all("presents stopped, retrace running", 0);
                pfired = 1;
            } else if (pstill >= 60 && pfired == 1) {
                dump_all("presents stopped for a minute, retrace running", 1);
                pfired = 2;
            }
        }
        last_pf = f;
        if (f != last || s_busy) {
            if (f != last && fired) resumed("frames", still ? still : secs);
            last = f;
            still = 0;
            fired = 0;
            continue;
        }
        if (++still >= XHW_WATCHDOG_SECS && !fired) {
            dump_all("frames stopped", 1);
            fired = 1;
        }
    }
}

static DWORD WINAPI watchdog(LPVOID arg) {
    xhw_crash_guard(watchdog_body, arg);
    return 0;
}

void xhw_watchdog_start(void) {
    HANDLE h;
    if (!XHW_WATCHDOG) return;
    /* 64 KB: the file writes run the kernel's file-system path on this stack */
    h = CreateThread(NULL, 64 * 1024, watchdog, NULL, 0, NULL);
    if (h) {
        SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);
        CloseHandle(h);
    }
}
