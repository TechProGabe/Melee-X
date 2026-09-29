/* xhw_watchdog.c - hang dumper (from OpenCrossing-Xbox xbox_watchdog.c).
 *
 * A 1 Hz thread started first thing in main_body() watches the retrace
 * counter (VIWaitForRetrace calls, xsdk_frame_count). It fires once if there
 * is no frame XHW_WATCHDOG_BOOT_SECS after boot, or if frames stop for
 * XHW_WATCHDOG_SECS later. Every thread in the process is dumped: state, wait
 * reason, and each stack word that points into the XBE image (a heuristic
 * backtrace; frame pointers are not reliable under -O2). The report goes to
 * the log (COM1 + boot.log), to E:\UDATA\4d580001\hang.log and onto the
 * screen. Symbolize with tools/xbox/sym.py. Costs nothing until it fires,
 * apart from a [BEAT] line every XHW_HEARTBEAT_SECS.
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
    int self, n;
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

/* last `lines` lines of the log, each cut to `cols` */
static void screen_tail(int lines, int cols) {
    static char tail[4096];
    char* p;
    char* start[64];
    int n = 0, i;
    xhw_log_tail(tail, sizeof tail);
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

static void dump_all(const char* why) {
    PKTHREAD me = KeGetCurrentThread();
    PKPROCESS p = me->ApcState.Process;
    PLIST_ENTRY e;
    int i, j, n = 0;
    HANDLE h;
    KIRQL old = KeRaiseIrqlToDpcLevel();   /* freeze the thread list while walking it */
    for (e = p->ThreadListHead.Flink; e != &p->ThreadListHead && n < WD_MAX_THREADS; e = e->Flink) {
        PKTHREAD t = CONTAINING_RECORD(e, KTHREAD, ThreadListEntry);
        snap_thread(&s_snap[n++], t, t == me);
    }
    KfLowerIrql(old);

    s_rlen = 0;
    rep("[WDOG] %s (frame %u), %d threads, free %u KB\n", why, xhw_frame_count(), n, xhw_mem_free_kb());
    for (i = 0; i < n; i++) {
        const Snap* o = &s_snap[i];
        rep("[WDOG] thread %p%s state %u wait %u prio %d\n[WDOG]  ", (void*)o->t, o->self ? " (watchdog)" : "",
            (unsigned)o->state, (unsigned)o->wait, (int)o->prio);
        for (j = 0; j < o->n; j++) rep(" %08lx", o->words[j]);
        rep("\n");
    }
    rep("[WDOG] end\n");

    /* COM1 first, lock-free: the log lock itself may be what is stuck */
    xhw_com1_raw(s_report, (size_t)s_rlen);

    /* screen next: the file I/O below can block if the hang involves the disk */
    pb_show_debug_screen();
    debugClearScreen();
    debugPrint("Melee-X: %s at frame %u\n", why, xhw_frame_count());
    debugPrint("Log: " XHW_UDATA_DIR "hang.log + boot.log\n\n");
    screen_tail(14, 76);
    debugPrint("\n");
    for (i = 0; i < n; i++) {
        const Snap* o = &s_snap[i];
        if (o->self) continue;
        debugPrint("t%d s%u w%u:", i, (unsigned)o->state, (unsigned)o->wait);
        for (j = 0; j < o->n && j < 8; j++) debugPrint(" %08lx", o->words[j]);
        debugPrint("\n");
    }

    h = CreateFileA(XHW_UDATA_DIR "hang.log", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        static char tail[4096];
        DWORD w;
        size_t tl = xhw_log_tail(tail, sizeof tail);
        WriteFile(h, tail, (DWORD)tl, &w, NULL);
        WriteFile(h, s_report, (DWORD)s_rlen, &w, NULL);
        xhw_flush_handle(h);
        CloseHandle(h);
    }
}

static volatile int s_disabled, s_busy;

void xhw_watchdog_busy(int on) { s_busy = on; }

/* an error card owns the screen for good */
void xhw_watchdog_disable(void) { s_disabled = 1; }

static void watchdog_body(void* arg) {
    unsigned last = 0, still = 0, secs = 0, fired = 0;
    (void)arg;
    for (;;) {
        unsigned f;
        Sleep(1000);
        secs++;
        if (s_disabled) continue;
        f = xhw_frame_count();
        if (XHW_HEARTBEAT_SECS && secs % XHW_HEARTBEAT_SECS == 0 && !s_busy) {
            /* COM1 directly: if a thread is stuck holding the log lock, the
             * heartbeat must still get out. Not during a screenshot: it
             * would land inside an [FBDUMP] line. */
            char line[160];
            int n = snprintf(line, sizeof line, "[BEAT] %us: retrace %u, presented %u, free %u KB, MEM1+ARAM %u KB\n",
                             secs, f, xgx_present_count(), xhw_mem_free_kb(), xhw_lazy_committed_kb());
            if (n > 0) xhw_com1_raw(line, (size_t)(n < (int)sizeof line ? n : (int)sizeof line - 1));
        }
        if (f == 0) {
            if (secs >= XHW_WATCHDOG_BOOT_SECS && !fired) {
                dump_all("no first frame after boot");
                fired = 1;
            }
            continue;
        }
        if (f != last || s_busy) {
            last = f;
            still = 0;
            fired = 0;
            continue;
        }
        if (++still >= XHW_WATCHDOG_SECS && !fired) {
            dump_all("frames stopped");
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
    h = CreateThread(NULL, 32 * 1024, watchdog, NULL, 0, NULL);
    if (h) {
        SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);
        CloseHandle(h);
    }
}
