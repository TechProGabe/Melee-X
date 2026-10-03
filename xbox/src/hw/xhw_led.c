/* xhw_led.c - front LED effects (settings.ini [system] led, the settings menu).
 *
 * The SMC drives the front LED. Register 0x08 takes a custom sequence of
 * four steps that the SMC cycles through by itself, several steps a second;
 * register 0x07 = 1 switches the LED to it and 0x07 = 0 gives it back to the
 * SMC (solid green, or the SMC's own blinking). The sequence byte, as nxdk's
 * hal/led.c builds it (xboxdevwiki "LED"): bit 7-n is step n's red, bit 3-n
 * its green (n = 0..3, the high nibble red, the low nibble green, first step
 * in the top bit); red and green together are orange.
 *
 * Each write is an SMBus transaction through the kernel (HalWriteSMBusValue
 * waits for the SMC), so none happen on the game thread. The game posts
 * events (a KO, the timer's last seconds, the match's end, a scene change)
 * into a small ring: a few stores and a SetEvent, nothing at all with the
 * effects off. A worker thread at the lowest priority, which runs while the
 * game waits for the vertical blank, turns them into a pattern and writes it
 * only when it changes, at most every LED_GAP_MS. The SMC does the blinking:
 * the worker only times when an effect ends.
 *
 * Effects, the first that applies wins:
 *   GAME!/TIME!  3 s red-orange-green-orange sweep (also the menu's preview)
 *   KO           1.5 s blinking in the port's colour: P1 red, P2 red/green,
 *                P3 orange (yellow), P4 green
 *   timer        last 10 s an orange blip, last 5 s orange blinking, last
 *                2 s orange/red
 *   last stock   a stock match with someone on their last stock: green with
 *                a red tick
 *   otherwise    the SMC's (register 0x07 = 0)
 * A scene change ends everything but a running sweep, so the LED is the
 * SMC's in the menus. Leaving the XBE (xhw_quit_to_dashboard,
 * xhw_reboot_self: the in-game reset, the game's own restart, Save and
 * restart, fatal errors) hands the LED back and waits for the write; a crash
 * hands it back for good from its handler, a hang report until the next
 * event. A failed write is logged once and the LED left alone from then on. */
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <stdint.h>

#include "xhw.h"
#include "xhw_internal.h"

#define SMC_ADDR 0x20
#define SMC_REG_LEDMODE 0x07   /* 0: the SMC's, 1: the sequence in 0x08 */
#define SMC_REG_LEDSEQ 0x08

/* one step: R red, G green, O orange, X off */
enum { X = 0x00, G = 0x01, R = 0x10, O = 0x11 };
#define SEQ(a, b, c, d) (((a) << 3) | ((b) << 2) | ((c) << 1) | (d))
#define LED_AUTO 0x100u        /* not a sequence: register 0x07 = 0 */

#define LED_GAP_MS 80          /* between two pattern writes */
#define LED_KO_MS 1500
#define LED_SWEEP_MS 3000
#define LED_PREVIEW_MS 1500
#define LED_LOG_MAX 300        /* [LED] lines a boot */

static const uint32_t k_port[4] = { SEQ(R, X, R, X), SEQ(R, G, R, G), SEQ(O, X, O, X), SEQ(G, X, G, X) };
static const uint32_t k_sweep = SEQ(R, O, G, O);
static const uint32_t k_last_stock = SEQ(G, G, G, R);

enum { EV_SCENE, EV_KO, EV_TIMER, EV_END, EV_PREVIEW };
#define RING 16                /* power of two */
static uint32_t s_ring[RING];
static volatile uint32_t s_head, s_tail;   /* head: the game thread's, tail: the worker's */

static HANDLE s_wake, s_done, s_thread;
static volatile int s_enabled, s_quit, s_reset;
static volatile int s_custom;  /* register 0x07 may be 1 */
static int s_dead;             /* a write failed: hands off */

/* ---- game thread ---- */

static void post(int kind, int a, int b) {
    uint32_t h = s_head;
    if (!s_enabled) return;
    if (h - __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE) >= RING) return;   /* full: dropped */
    s_ring[h % RING] = (uint32_t)kind | ((uint32_t)(a & 0xFF) << 8) | ((uint32_t)(b & 0xFF) << 16);
    __atomic_store_n(&s_head, h + 1, __ATOMIC_RELEASE);
    SetEvent(s_wake);
}

void xhw_led_scene(void) { post(EV_SCENE, 0, 0); }
void xhw_led_ko(int port, int stocks_left) {
    if (port >= 0 && port < 4) post(EV_KO, port, stocks_left);
}
void xhw_led_timer(int seconds_left) {
    /* 0 is TIME!: the pattern stays up until the match's end, a frame later */
    if (seconds_left >= 1 && seconds_left <= 10) post(EV_TIMER, seconds_left, 0);
}
void xhw_led_match_end(int outcome) {
    xhw_prof_match_end();   /* the game's only match-end hook: the whole-match profile goes out too */
    post(EV_END, outcome, 0);
}
void xhw_led_preview(void) { post(EV_PREVIEW, 0, 0); }

/* ---- worker ---- */

static struct {
    DWORD sweep_end, ko_end;   /* GetTickCount; 0: not running */
    uint32_t sweep_pat, ko_pat;
    int timer;                 /* seconds left, 1..10; 0: none */
    unsigned last_stock;       /* ports on their last stock */
    int over;                  /* the match ended: nothing but the sweep */
    const char* why;           /* what the KO/sweep/timer layer is, for the log */
} st;
static uint32_t s_cur = LED_AUTO;
static DWORD s_last_write;
static int s_logged;

static int smc_write(int reg, uint32_t val) {
    NTSTATUS r = HalWriteSMBusValue(SMC_ADDR, (UCHAR)reg, FALSE, val);
    if (NT_SUCCESS(r)) return 1;
    if (!s_dead && !s_quit)
        xhw_logf("[LED] SMBus write %02x = %02x failed (%08lx): LED effects off", reg, (unsigned)val,
                 (unsigned long)r);
    s_dead = 1;
    return 0;
}

static void apply(uint32_t want) {
    if (want == LED_AUTO) {
        if (smc_write(SMC_REG_LEDMODE, 0)) s_custom = 0;
    } else {
        s_custom = 1;
        if (smc_write(SMC_REG_LEDSEQ, want)) {
            /* a quit since the pattern was chosen: back to the SMC instead */
            smc_write(SMC_REG_LEDMODE, s_quit ? 0 : 1);
        }
        if (s_dead) HalWriteSMBusValue(SMC_ADDR, SMC_REG_LEDMODE, FALSE, 0);   /* one try at handing it back */
    }
    s_cur = want;
    s_last_write = GetTickCount();
}

static int running(DWORD end, DWORD now) { return end && (LONG)(end - now) > 0; }

static void take(uint32_t ev, DWORD now) {
    int a = (int8_t)(ev >> 8), b = (int8_t)(ev >> 16);
    switch (ev & 0xFF) {
        case EV_SCENE:
            /* a sweep runs on into the next scene (GAME! then the results) */
            st.ko_end = 0;
            st.timer = 0;
            st.last_stock = 0;
            st.over = 0;
            break;
        case EV_KO:
            if (st.over) break;
            st.ko_pat = k_port[a & 3];
            st.ko_end = now + LED_KO_MS;
            if (b == 1) st.last_stock |= 1u << a;
            else st.last_stock &= ~(1u << a);
            break;
        case EV_TIMER: st.timer = a; break;
        case EV_END:
            st.over = 1;
            st.ko_end = 0;
            st.timer = 0;
            st.last_stock = 0;
            if (a != 7) {   /* no contest: nothing to celebrate */
                st.sweep_pat = k_sweep;
                st.sweep_end = now + LED_SWEEP_MS;
            }
            break;
        case EV_PREVIEW:
            st.sweep_pat = k_sweep;
            st.sweep_end = now + LED_PREVIEW_MS;
            break;
    }
}

/* the pattern now; *wait_ms shrinks to when it next changes by itself */
static uint32_t want(DWORD now, DWORD* wait_ms) {
    if (running(st.sweep_end, now)) {
        DWORD left = st.sweep_end - now;
        if (left < *wait_ms) *wait_ms = left;
        st.why = "sweep";
        return st.sweep_pat;
    }
    st.sweep_end = 0;
    if (st.over) return LED_AUTO;
    if (running(st.ko_end, now)) {
        DWORD left = st.ko_end - now;
        if (left < *wait_ms) *wait_ms = left;
        st.why = "KO";
        return st.ko_pat;
    }
    st.ko_end = 0;
    if (st.timer) {
        st.why = "timer";
        return st.timer > 5 ? SEQ(O, X, X, X) : st.timer > 2 ? SEQ(O, X, O, X) : SEQ(O, R, O, R);
    }
    if (st.last_stock) {
        st.why = "last stock";
        return k_last_stock;
    }
    return LED_AUTO;
}

static void log_write(uint32_t p) {
    static const char* const k_step[4] = { "-", "G", "R", "O" };
    int i;
    char s[8];
    if (s_logged > LED_LOG_MAX) return;
    if (++s_logged > LED_LOG_MAX) {
        xhw_logf("[LED] more writes, not logged");
        return;
    }
    if (p == LED_AUTO) {
        xhw_logf("[LED] SMC (retrace %u)", xhw_frame_count());
        return;
    }
    for (i = 0; i < 4; i++) s[i] = k_step[((p >> (7 - i)) & 1) << 1 | ((p >> (3 - i)) & 1)][0];
    s[4] = '\0';
    xhw_logf("[LED] %s %s (retrace %u)", s, st.why, xhw_frame_count());
}

static void worker(void* arg) {
    (void)arg;
    for (;;) {
        DWORD now = GetTickCount(), wait = INFINITE;
        uint32_t t = s_tail, p;
        int reset;
        while (t != __atomic_load_n(&s_head, __ATOMIC_ACQUIRE)) {
            take(s_ring[t % RING], now);
            __atomic_store_n(&s_tail, ++t, __ATOMIC_RELEASE);
        }
        if (s_quit) {
            if (s_custom && !s_dead) apply(LED_AUTO);
            SetEvent(s_done);
            for (;;) WaitForSingleObject(s_wake, INFINITE);
        }
        reset = __atomic_exchange_n(&s_reset, 0, __ATOMIC_ACQ_REL);
        if (reset) {
            take(EV_SCENE, now);
            st.sweep_end = 0;
        }
        p = s_enabled ? want(now, &wait) : LED_AUTO;
        if (p != s_cur && !s_dead) {
            DWORD since = now - s_last_write;
            if (s_last_write && since < LED_GAP_MS && !reset) {
                if (LED_GAP_MS - since < wait) wait = LED_GAP_MS - since;
            } else {
                apply(p);
                if (!s_dead && !reset) log_write(p);   /* a hung thread may hold the log lock */
            }
        }
        if (reset) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);   /* xhw_led_release raised it */
        WaitForSingleObject(s_wake, wait);
    }
}

static DWORD WINAPI worker_entry(LPVOID arg) {
    xhw_crash_guard(worker, arg);
    return 0;
}

void xhw_led_enable(int on) {
    if (on && !s_thread && !s_dead) {
        s_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        s_done = CreateEventA(NULL, TRUE, FALSE, NULL);
        /* 32 KB: the SMBus call and the [LED] lines, whose log flush runs
         * the file system on this stack */
        if (s_wake && s_done) s_thread = CreateThread(NULL, 32 * 1024, worker_entry, NULL, 0, NULL);
        if (!s_thread) {
            xhw_logf("[LED] no worker thread: LED effects off");
            s_dead = 1;
            return;
        }
        SetThreadPriority(s_thread, THREAD_PRIORITY_LOWEST);
    }
    if (!s_thread) return;
    s_enabled = on && !s_dead;
    if (on) post(EV_SCENE, 0, 0);   /* nothing stale from before it was off */
    else SetEvent(s_wake);          /* the worker goes back to the SMC */
}

/* any thread: back to the SMC without waiting. A crash (for_good) ends the
 * effects; a hang report only resets them. A spinning game thread would
 * starve the worker at its lowest priority, so it gets the highest for this
 * and drops back once done. */
void xhw_led_release(int for_good) {
    if (!s_thread) return;
    if (for_good) s_quit = 1;
    else if (!s_custom) return;
    else s_reset = 1;
    SetThreadPriority(s_thread, THREAD_PRIORITY_HIGHEST);
    SetEvent(s_wake);
}

/* leaving the XBE: the worker writes register 0x07 = 0 and says so; if it
 * doesn't within 300 ms the write is done here */
void xhw_led_shutdown(void) {
    if (!s_thread) return;
    s_quit = 1;
    SetThreadPriority(s_thread, THREAD_PRIORITY_HIGHEST);
    SetEvent(s_wake);
    if (WaitForSingleObject(s_done, 300) != WAIT_OBJECT_0 && s_custom && !s_dead) {
        HalWriteSMBusValue(SMC_ADDR, SMC_REG_LEDMODE, FALSE, 0);
        xhw_logf("[LED] worker didn't answer: handed back from here");
    }
}
