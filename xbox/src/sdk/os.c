/* os.c - Dolphin OS on the Xbox: MEM1 and the arena, OSAlloc heaps,
 * interrupts, alarms, time, reset and reports.
 *
 * The interrupt/alarm model is melee-pc's (src/pc/os.c there): the game is
 * single-threaded, OSDisableInterrupts is a recursive lock shared with the
 * worker threads (audio mixer, DVD), and alarms and completions are delivered
 * on the game thread when it re-enables interrupts and at every frame
 * boundary, where GameCube interrupt handlers used to run. */
#include <dolphin/card.h>
#include <dolphin/os.h>
#include <dolphin/os/OSAlarm.h>
#include <dolphin/os/OSError.h>
#include <dolphin/os/OSReset.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc/disc.h"
#include "xhw.h"
#include "xsdk.h"

/* ======================================================================
 * MEM1
 * ====================================================================== */
/* The whole GameCube main RAM is 24 MB, including the DOL; the game's own
 * code lives in the XBE here, so the arena gets MEM1 minus the low-memory
 * globals. It sits at a fixed VA above 16 MB: PC_IS_ARAM_ADDR treats
 * anything lower as an ARAM offset (docs/architecture.md). */
#ifndef XSDK_MEM1_VA
#define XSDK_MEM1_VA 0x10000000u
#endif
#ifndef XSDK_MEM1_SIZE
#define XSDK_MEM1_SIZE (24u * 1024 * 1024)
#endif
#define ARENA_START_OFFSET 0x4000u   /* low-memory globals (boot info, clocks) */

uintptr_t OSBaseAddress;
static u8* s_mem1;
static void* s_arena_lo;
static void* s_arena_hi;
static OSTime s_time_base;       /* OSGetTime at boot, from the wall clock */
static uint64_t s_ns_base;

#define ROUNDUP(n, a) (((uintptr_t)(n) + (a) - 1) & ~(uintptr_t)((a) - 1))
#define TRUNC(n, a) ((uintptr_t)(n) & ~(uintptr_t)((a) - 1))

static void wr32(u32 off, u32 v) { *(u32*)(s_mem1 + off) = v; }

void xsdk_fill_disc_id(const void* header32) {
    if (s_mem1) memcpy(s_mem1, header32, 0x20);
}

static uint32_t s_intr_tls;   /* per thread: OSDisableInterrupts depth */
static uint32_t s_game_tls;   /* set on the game thread only */

void OSInit(void) {
    static int done;
    uint32_t ns;
    int64_t secs;
    if (done) return;
    done = 1;
    s_intr_tls = xhw_tls_alloc();
    s_game_tls = xhw_tls_alloc();
    xhw_tls_set(s_game_tls, (void*)1);   /* melee_main calls OSInit first */
    s_mem1 = (u8*)xhw_reserve_lazy(XSDK_MEM1_VA, XSDK_MEM1_SIZE);   /* committed on demand */
    if (!s_mem1) xhw_fatal("Out of memory", "Could not reserve the game's main memory.");
    memset(s_mem1, 0, ARENA_START_OFFSET);
    OSBaseAddress = (uintptr_t)s_mem1;
    /* OSBootInfo: the disc ID is filled in by DVD (xsdk_fill_disc_id) */
    wr32(0x20, 0x0D15EA5E);
    wr32(0x24, 1);
    wr32(0x28, XSDK_MEM1_SIZE);
    wr32(0x2C, OS_CONSOLE_RETAIL1);
    wr32(0xF0, XSDK_MEM1_SIZE);
    wr32(0xF8, OS_BUS_CLOCK);
    wr32(0xFC, 486000000u);
    s_arena_lo = s_mem1 + ARENA_START_OFFSET;
    s_arena_hi = s_mem1 + XSDK_MEM1_SIZE;
    secs = xhw_wallclock_2000(&ns);
    s_time_base = (OSTime)secs * OS_TIMER_CLOCK + (OSTime)ns * (OS_TIMER_CLOCK / 1000000) / 1000;
    s_ns_base = xhw_time_ns();
    xhw_logf("[OS] MEM1 %p, %u KB arena", s_mem1, (unsigned)(XSDK_MEM1_SIZE - ARENA_START_OFFSET) / 1024);
}

void* OSGetArenaHi(void) { return s_arena_hi; }
void* OSGetArenaLo(void) { return s_arena_lo; }
void OSSetArenaHi(void* hi) { s_arena_hi = hi; }
void OSSetArenaLo(void* lo) { s_arena_lo = lo; }

void* OSAllocFromArenaLo(u32 size, u32 align) {
    uintptr_t p = ROUNDUP(s_arena_lo, align);
    s_arena_lo = (void*)ROUNDUP(p + size, align);
    return (void*)p;
}

void* OSAllocFromArenaHi(u32 size, u32 align) {
    uintptr_t p = TRUNC((uintptr_t)s_arena_hi, align) - size;
    p = TRUNC(p, align);
    s_arena_hi = (void*)p;
    return (void*)p;
}

u32 OSGetPhysicalMemSize(void) { return XSDK_MEM1_SIZE; }
u32 OSGetConsoleSimulatedMemSize(void) { return XSDK_MEM1_SIZE; }
u32 OSGetConsoleType(void) { return OS_CONSOLE_RETAIL1; }

/* ======================================================================
 * OSAlloc: the Dolphin SDK heap (first fit, 32-byte cells, address-ordered
 * free list with coalescing). Heap behaviour matters: the game sizes its
 * heaps to what it measured on the GameCube.
 * ====================================================================== */
typedef struct Cell {
    struct Cell* prev;
    struct Cell* next;
    long size;   /* including the header */
} Cell;

typedef struct HeapDesc {
    long size;   /* -1: unused */
    Cell* free;
    Cell* allocated;
} HeapDesc;

#define HEADERSIZE 32
#define MINOBJSIZE 64

volatile OSHeapHandle __OSCurrHeap = -1;
static HeapDesc* s_heaps;
static int s_num_heaps;
static void* s_alloc_start;
static void* s_alloc_end;

static Cell* dl_add_front(Cell* list, Cell* cell) {
    cell->next = list;
    cell->prev = NULL;
    if (list) list->prev = cell;
    return cell;
}

static Cell* dl_extract(Cell* list, Cell* cell) {
    if (cell->next) cell->next->prev = cell->prev;
    if (!cell->prev) return cell->next;
    cell->prev->next = cell->next;
    return list;
}

/* insert in address order, merging with neighbours */
static Cell* dl_insert(Cell* list, Cell* cell) {
    Cell *prev = NULL, *next = list;
    while (next && next <= cell) {
        prev = next;
        next = next->next;
    }
    cell->next = next;
    cell->prev = prev;
    if (next) {
        next->prev = cell;
        if ((u8*)cell + cell->size == (u8*)next) {
            cell->size += next->size;
            cell->next = next->next;
            if (cell->next) cell->next->prev = cell;
        }
    }
    if (prev) {
        prev->next = cell;
        if ((u8*)prev + prev->size == (u8*)cell) {
            prev->size += cell->size;
            prev->next = cell->next;
            if (prev->next) prev->next->prev = prev;
        }
        return list;
    }
    return cell;
}

void* OSInitAlloc(void* start, void* end, int max_heaps) {
    int i;
    u32 bytes = (u32)(max_heaps * (int)sizeof(HeapDesc));
    s_heaps = (HeapDesc*)start;
    s_num_heaps = max_heaps;
    for (i = 0; i < max_heaps; i++) {
        s_heaps[i].size = -1;
        s_heaps[i].free = s_heaps[i].allocated = NULL;
    }
    __OSCurrHeap = -1;
    start = (void*)ROUNDUP((u8*)s_heaps + bytes, 32);
    s_alloc_start = start;
    s_alloc_end = (void*)TRUNC(end, 32);
    return start;
}

OSHeapHandle OSCreateHeap(void* start, void* end) {
    int i;
    Cell* cell = (Cell*)ROUNDUP(start, 32);
    end = (void*)TRUNC(end, 32);
    for (i = 0; i < s_num_heaps; i++) {
        HeapDesc* hd = &s_heaps[i];
        if (hd->size >= 0) continue;
        hd->size = (long)((u8*)end - (u8*)cell);
        cell->prev = cell->next = NULL;
        cell->size = hd->size;
        hd->free = cell;
        hd->allocated = NULL;
        return i;
    }
    return -1;
}

void OSDestroyHeap(OSHeapHandle heap) {
    if (heap >= 0 && heap < s_num_heaps) s_heaps[heap].size = -1;
}

void OSAddToHeap(OSHeapHandle heap, void* start, void* end) {
    HeapDesc* hd = &s_heaps[heap];
    Cell* cell = (Cell*)ROUNDUP(start, 32);
    end = (void*)TRUNC(end, 32);
    cell->size = (long)((u8*)end - (u8*)cell);
    hd->size += cell->size;
    hd->free = dl_insert(hd->free, cell);
}

OSHeapHandle OSSetCurrentHeap(OSHeapHandle heap) {
    OSHeapHandle old = __OSCurrHeap;
    __OSCurrHeap = heap;
    return old;
}

void* OSAllocFromHeap(OSHeapHandle heap, u32 size) {
    HeapDesc* hd;
    Cell* cell;
    long need, left;
    if (heap < 0 || heap >= s_num_heaps || s_heaps[heap].size < 0) return NULL;
    hd = &s_heaps[heap];
    need = (long)ROUNDUP(size + HEADERSIZE, 32);
    for (cell = hd->free; cell; cell = cell->next)
        if (cell->size >= need) break;
    if (!cell) {
        xhw_logf("[OS] OSAllocFromHeap(%d, %u) failed", heap, size);
        return NULL;
    }
    left = cell->size - need;
    if (left < MINOBJSIZE) {
        hd->free = dl_extract(hd->free, cell);
    } else {
        Cell* rest = (Cell*)((u8*)cell + need);
        cell->size = need;
        rest->size = left;
        rest->prev = cell->prev;
        rest->next = cell->next;
        if (rest->next) rest->next->prev = rest;
        if (rest->prev) rest->prev->next = rest;
        else hd->free = rest;
    }
    hd->allocated = dl_add_front(hd->allocated, cell);
    return (u8*)cell + HEADERSIZE;
}

void OSFreeToHeap(OSHeapHandle heap, void* ptr) {
    HeapDesc* hd;
    Cell* cell;
    if (!ptr || heap < 0 || heap >= s_num_heaps || s_heaps[heap].size < 0) return;
    hd = &s_heaps[heap];
    cell = (Cell*)((u8*)ptr - HEADERSIZE);
    hd->allocated = dl_extract(hd->allocated, cell);
    hd->free = dl_insert(hd->free, cell);
}

void* OSAllocFixed(void* rstart, void* rend) {
    (void)rstart;
    (void)rend;
    return NULL;
}

s32 OSCheckHeap(OSHeapHandle heap) {
    long total = 0;
    Cell* c;
    if (heap < 0 || heap >= s_num_heaps || s_heaps[heap].size < 0) return -1;
    for (c = s_heaps[heap].free; c; c = c->next) total += c->size - HEADERSIZE;
    return (s32)total;
}

u32 OSReferentSize(void* ptr) { return (u32)(((Cell*)((u8*)ptr - HEADERSIZE))->size - HEADERSIZE); }

void OSDumpHeap(OSHeapHandle heap) { xhw_logf("[OS] heap %d: %d bytes free", heap, (int)OSCheckHeap(heap)); }

void OSVisitAllocated(void (*visitor)(void*, u32)) {
    int i;
    Cell* c;
    for (i = 0; i < s_num_heaps; i++)
        if (s_heaps[i].size >= 0)
            for (c = s_heaps[i].allocated; c; c = c->next) visitor((u8*)c + HEADERSIZE, (u32)c->size);
}

bool aurora_heap_extent(OSHeapHandle heap, void** lo, void** hi) {
    (void)heap;
    *lo = s_alloc_start;
    *hi = s_alloc_end;
    return heap >= 0 && heap < s_num_heaps && s_heaps[heap].size >= 0;
}

void aurora_heap_descs(void** lo, size_t* len) {
    *lo = s_heaps;
    *len = (size_t)s_num_heaps * sizeof(HeapDesc);
}

bool aurora_heap_desc(OSHeapHandle heap, void** lo, size_t* len) {
    if (heap < 0 || heap >= s_num_heaps || s_heaps[heap].size < 0) return false;
    *lo = &s_heaps[heap];
    *len = sizeof(HeapDesc);
    return true;
}

/* ======================================================================
 * Interrupts: a recursive lock, depth kept per thread in a TLS slot.
 * ====================================================================== */
static xhw_mutex* s_intr;
static int s_in_delivery;

static int intr_depth(void) { return (int)(intptr_t)xhw_tls_get(s_intr_tls); }
static void set_intr_depth(int d) { xhw_tls_set(s_intr_tls, (void*)(intptr_t)d); }

int xsdk_is_game_thread(void) { return xhw_tls_get(s_game_tls) != NULL; }

BOOL OSDisableInterrupts(void) {
    int d;
    if (!s_intr) s_intr = xhw_mutex_create();
    xhw_mutex_lock(s_intr);
    d = intr_depth();
    set_intr_depth(d + 1);
    return d == 0;
}

/* HSD re-enables interrupts thousands of times a frame (allocations, object
 * lists, audio); alarms are milliseconds apart. Checking at most every
 * ~0.3 ms (rdtsc, 733 MHz) keeps the time conversions and the alarm walk out
 * of those; the frame boundary still runs them every frame. */
#define DELIVERY_TSC 250000u

static void deliver_pending(void) {
    static uint64_t last;
    uint64_t now = xhw_perf_now();
    if (now - last < DELIVERY_TSC) return;
    if (xsdk_is_game_thread() && !s_in_delivery) {
        last = now;
        s_in_delivery = 1;
        xsdk_run_alarms();
        s_in_delivery = 0;
    }
}

BOOL OSEnableInterrupts(void) {
    int d = intr_depth();
    BOOL was = d == 0;
    while (d > 0) {
        set_intr_depth(--d);
        xhw_mutex_unlock(s_intr);
    }
    deliver_pending();
    return was;
}

BOOL OSRestoreInterrupts(BOOL level) {
    int d = intr_depth();
    BOOL was = d == 0;
    if (level) {
        OSEnableInterrupts();
    } else if (d > 0) {
        set_intr_depth(d - 1);
        xhw_mutex_unlock(s_intr);
    }
    return was;
}

/* ======================================================================
 * Alarms (run on the game thread)
 * ====================================================================== */
static OSAlarm* s_alarms;

void OSInitAlarm(void) {}

void OSCreateAlarm(OSAlarm* alarm) {
    alarm->handler = NULL;
    alarm->prev = alarm->next = NULL;
    alarm->period = 0;
    alarm->tag = 0;
}

static void insert_alarm(OSAlarm* alarm, OSTime fire, OSAlarmHandler handler) {
    BOOL intr = OSDisableInterrupts();
    if (alarm->handler) OSCancelAlarm(alarm);
    alarm->handler = handler;
    alarm->fire = fire;
    alarm->prev = NULL;
    alarm->next = s_alarms;
    if (s_alarms) s_alarms->prev = alarm;
    s_alarms = alarm;
    OSRestoreInterrupts(intr);
}

void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler) {
    alarm->period = 0;
    insert_alarm(alarm, OSGetTime() + tick, handler);
}

void OSSetAbsAlarm(OSAlarm* alarm, OSTime time, OSAlarmHandler handler) {
    alarm->period = 0;
    insert_alarm(alarm, time, handler);
}

void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler) {
    alarm->period = period;
    alarm->start = start;
    insert_alarm(alarm, OSGetTime() + start, handler);
}

void OSCancelAlarm(OSAlarm* alarm) {
    BOOL intr = OSDisableInterrupts();
    if (alarm->handler) {
        if (alarm->prev) alarm->prev->next = alarm->next;
        else if (s_alarms == alarm) s_alarms = alarm->next;
        if (alarm->next) alarm->next->prev = alarm->prev;
        alarm->handler = NULL;
        alarm->prev = alarm->next = NULL;
    }
    OSRestoreInterrupts(intr);
}

void OSSetAlarmTag(OSAlarm* alarm, u32 tag) { alarm->tag = tag; }

void OSCancelAlarms(u32 tag) {
    BOOL intr = OSDisableInterrupts();
    OSAlarm* a = s_alarms;
    while (a) {
        OSAlarm* next = a->next;
        if (a->tag == tag) OSCancelAlarm(a);
        a = next;
    }
    OSRestoreInterrupts(intr);
}

BOOL OSCheckAlarmQueue(void) { return s_alarms != NULL; }

static void card_deliver(void);

void xsdk_run_alarms(void) {
    OSTime now = OSGetTime();
    BOOL intr = OSDisableInterrupts();
    OSAlarm* a = s_alarms;
    while (a) {
        OSAlarm* next = a->next;
        if (a->fire <= now) {
            OSAlarmHandler handler = a->handler;
            if (a->period > 0) {
                if (a->period <= OSMillisecondsToTicks(10)) {
                    /* the 3 ms pad-poll alarm only needs "fired since last frame" */
                    a->fire += a->period;
                    if (a->fire <= now) a->fire = now + a->period;
                    handler(a, NULL);
                } else {
                    int catchup = 10;   /* timekeeping alarms (movie player) catch up */
                    while (a->fire <= now && catchup-- > 0 && a->handler == handler) {
                        a->fire += a->period;
                        handler(a, NULL);
                    }
                    if (a->fire <= now) a->fire = now + a->period;
                }
            } else {
                OSCancelAlarm(a);
                handler(a, NULL);
            }
        }
        a = next;
    }
    OSRestoreInterrupts(intr);
    card_deliver();
    xsdk_dvd_deliver();
    xsdk_arq_deliver();
}

/* The frame loop spins until the pad alarm has queued a sample; sleep toward
 * the earliest alarm instead of burning the CPU the GPU driver needs. */
void pc_os_wait_alarm(void) {
    BOOL intr;
    if (xsdk_lockstep()) {   /* the clock stands still between frames: move it */
        xsdk_lockstep_advance(1000000);
        xsdk_run_alarms();
        return;
    }
    intr = OSDisableInterrupts();
    OSTime next = 0, wait;
    OSAlarm* a;
    for (a = s_alarms; a; a = a->next)
        if (next == 0 || a->fire < next) next = a->fire;
    OSRestoreInterrupts(intr);
    wait = next - OSGetTime();
    if (next != 0 && wait > 0) {
        /* pacing, not simulation: [PERF] charges it to vsync (under xemu's
         * -icount the sleep is idle guest time, not instructions) */
        int pf = xhw_perf_enter(XHW_PERF_VSYNC);
        if (wait >= OSMillisecondsToTicks(1)) xhw_sleep_ms(1);
        else xhw_yield();
        xhw_perf_leave(pf);
    }
    xsdk_dvd_deliver();
}

void pc_os_yield(void) {
    xhw_sleep_ms(1);
    xsdk_dvd_deliver();
}

/* ======================================================================
 * Memory card completions (card.c finishes calls inline; the game arms its
 * "pending" state after the call returns, so they are queued and delivered
 * with the alarms, as the CARD interrupt handler did).
 * ====================================================================== */
#define CARD_QUEUE 16
static struct {
    CARDCallback callback;
    s32 chan;
    s32 result;
} s_card_queue[CARD_QUEUE];
static int s_card_head, s_card_count;

void xsdk_card_dispatch(CARDCallback callback, s32 chan, s32 result) {
    BOOL intr = OSDisableInterrupts();
    int slot;
    if (s_card_count == CARD_QUEUE) OSPanic(__FILE__, __LINE__, "card completion queue overflow");
    slot = (s_card_head + s_card_count++) % CARD_QUEUE;
    s_card_queue[slot].callback = callback;
    s_card_queue[slot].chan = chan;
    s_card_queue[slot].result = result;
    OSRestoreInterrupts(intr);
}

static void card_deliver(void) {
    while (s_card_count > 0) {
        BOOL intr = OSDisableInterrupts();
        CARDCallback cb = s_card_queue[s_card_head].callback;
        s32 chan = s_card_queue[s_card_head].chan;
        s32 result = s_card_queue[s_card_head].result;
        s_card_head = (s_card_head + 1) % CARD_QUEUE;
        s_card_count--;
        OSRestoreInterrupts(intr);
        if (cb) cb(chan, result);
    }
}

/* ======================================================================
 * Time: 40.5 MHz ticks since 2000-01-01 (local time, as the GameCube RTC)
 * ====================================================================== */
/* env MX_LOCKSTEP=1 (test builds): the game's clock advances 1/60 s per
 * frame boundary and stands still in between, so the pad alarm queues one
 * sample a frame and every rendered frame is one simulation tick: frame N
 * shows tick N in any build, however fast (xemu screenshots compare across
 * builds). The frame loop's wait for a pad sample (pc_os_wait_alarm) moves
 * it 1 ms at a time instead of sleeping. Pacing is off; [PERF] keeps the
 * real clock. */
static int s_lockstep = -1;
static uint64_t s_lockstep_ns;

int xsdk_lockstep(void) {
    if (s_lockstep < 0) {
        const char* e = getenv("MX_LOCKSTEP");
        s_lockstep = e && atoi(e);
        if (s_lockstep) xhw_logf("[OS] lockstep: one simulation tick per frame");
    }
    return s_lockstep;
}

void xsdk_lockstep_advance(u32 ns) { s_lockstep_ns += ns; }

/* env MX_JITTER=<seed> (test builds, docs/lan-plan.md phase 0A): a seeded
 * delay of 0-2 ms at the frame boundary, in the mixer loop and in the DVD
 * worker before a completion, each site with its own xorshift sequence. It
 * moves what the console's timing moves (when a voice ends, when a read
 * completes, how many ticks a render gets), so a real-time xemu run can show
 * what only the console showed (roadmap item 10). */
void xsdk_jitter(int site) {
#if XHW_TEST_BUILD
    static int seed = -1;
    static uint32_t state[XSDK_JITTER_SITES];
    uint32_t x, us;
    uint64_t end;
    if (seed <= 0) {
        if (seed == 0) return;
        {
            const char* e = getenv("MX_JITTER");   /* read at boot: the script is loaded before any thread */
            int s = e ? atoi(e) : 0;
            if (s > 0) xhw_logf("[OS] jitter: seed %d, 0-2 ms at the frame boundary, mixer and DVD completions", s);
            seed = s > 0 ? s : 0;
        }
        if (!seed) return;
    }
    if (site < 0 || site >= XSDK_JITTER_SITES) return;
    x = state[site];
    if (!x) x = (uint32_t)seed * 2654435761u ^ ((uint32_t)site + 1) * 40503u;
    if (!x) x = 1;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state[site] = x;
    us = x % 2001;
    if (us >= 1000) xhw_sleep_ms(us / 1000);
    end = xhw_time_ns() + (uint64_t)(us % 1000) * 1000;
    while (xhw_time_ns() < end) {}
#else
    (void)site;
#endif
}

/* [NETM] (test builds; simhash.c calls it at a match's ticks 600 and 3600):
 * what a snapshot of the game's heaps would have to copy (the cells in use
 * of each OSAlloc heap, headers included), and 1 MB copied eight times
 * between two cold buffers (xhw_sys.c): rollback's numbers,
 * docs/lan-plan.md F5. Logging only; the copy costs a few tens of ms. */
void xsdk_netm(unsigned tick) {
#if XHW_TEST_BUILD
    char line[600];
    int len, i;
    uint32_t live = 0, cells = 0, size = 0, mn, mean, mx;
    BOOL intr = OSDisableInterrupts();
    len = snprintf(line, sizeof line, "[NETM] tick %u: heap live/size KB (cells):", tick);
    for (i = 0; i < s_num_heaps && len < (int)sizeof line - 40; i++) {
        uint32_t b = 0, n = 0;
        Cell* c;
        if (s_heaps[i].size < 0) continue;
        for (c = s_heaps[i].allocated; c; c = c->next) {
            b += (uint32_t)c->size;
            n++;
        }
        len += snprintf(line + len, sizeof line - len, " %d: %u/%u (%u)", i, (unsigned)(b / 1024),
                        (unsigned)(s_heaps[i].size / 1024), (unsigned)n);
        live += b;
        cells += n;
        size += (uint32_t)s_heaps[i].size;
    }
    OSRestoreInterrupts(intr);
    snprintf(line + len, sizeof line - len, "; all %u of %u KB in %u cells", (unsigned)(live / 1024),
             (unsigned)(size / 1024), (unsigned)cells);
    xhw_log(line);
    if (xhw_mem_copy_probe(1u << 20, 8, &mn, &mean, &mx))
        xhw_logf("[NETM] tick %u: 1 MB copied 8 times between cold buffers: min %u us, mean %u us, max %u us "
                 "(%u MB/s), free %u KB",
                 tick, (unsigned)mn, (unsigned)mean, (unsigned)mx, mean ? (unsigned)(1000000u / mean) : 0u,
                 (unsigned)xhw_mem_free_kb());
    else
        xhw_logf("[NETM] tick %u: no 2 MB for the copy test, free %u KB", tick, (unsigned)xhw_mem_free_kb());
#else
    (void)tick;
#endif
}

OSTime OSGetTime(void) {
    uint64_t ns = xsdk_lockstep() ? s_lockstep_ns : xhw_time_ns() - s_ns_base;
    return s_time_base + (OSTime)(ns / 1000000000ull) * OS_TIMER_CLOCK +
           (OSTime)(ns % 1000000000ull) * (OS_TIMER_CLOCK / 1000000) / 1000;
}

OSTick OSGetTick(void) { return (OSTick)OSGetTime(); }
OSTime OSGetSystemTime(void) { return OSGetTime(); }
OSTime OSGetNativeTime(void) { return OSGetTime(); }
OSTime __OSGetSystemTime(void) { return OSGetTime(); }

uint64_t pc_monotonic_ns(void) { return xhw_time_ns(); }

static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td) {
    static const int k_mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    s64 secs = ticks / OS_TIMER_CLOCK, days;
    s64 sub = ticks % OS_TIMER_CLOCK;
    int y = 2000, m;
    if (sub < 0) {
        sub += OS_TIMER_CLOCK;
        secs--;
    }
    td->msec = (int)(sub / (OS_TIMER_CLOCK / 1000));
    td->usec = (int)(sub % (OS_TIMER_CLOCK / 1000) * 1000 / (OS_TIMER_CLOCK / 1000));
    days = secs / 86400;
    secs %= 86400;
    if (secs < 0) {
        secs += 86400;
        days--;
    }
    td->hour = (int)(secs / 3600);
    td->min = (int)(secs / 60 % 60);
    td->sec = (int)(secs % 60);
    td->wday = (int)((days + 6) % 7);   /* 2000-01-01 was a Saturday */
    if (td->wday < 0) td->wday += 7;
    while (days < 0) days += is_leap(--y) ? 366 : 365;
    for (;;) {
        int len = is_leap(y) ? 366 : 365;
        if (days < len) break;
        days -= len;
        y++;
    }
    td->year = y;
    td->yday = (int)days;
    for (m = 0; m < 12; m++) {
        int len = k_mdays[m] + (m == 1 && is_leap(y));
        if (days < len) break;
        days -= len;
    }
    td->mon = m;
    td->mday = (int)days + 1;
}

OSTime OSCalendarTimeToTicks(OSCalendarTime* td) {
    static const int k_mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    s64 days = 0;
    int y, m;
    for (y = 2000; y < td->year; y++) days += is_leap(y) ? 366 : 365;
    for (m = 0; m < td->mon && m < 12; m++) days += k_mdays[m] + (m == 1 && is_leap(td->year));
    days += td->mday - 1;
    return ((((days * 24 + td->hour) * 60 + td->min) * 60 + td->sec) * OS_TIMER_CLOCK) +
           (OSTime)td->msec * (OS_TIMER_CLOCK / 1000) + (OSTime)td->usec * (OS_TIMER_CLOCK / 1000000);
}

/* ======================================================================
 * Reset, modes, misc
 * ====================================================================== */
static u32 s_progressive;
static u32 s_eurgb60;
static u32 s_reset_code;

void OSResetSystem(int reset, u32 code, BOOL force_menu) {
    xhw_logf("[OS] OSResetSystem(%d, %08x, %d)", reset, (unsigned)code, force_menu);
    if (reset == OS_RESET_HOTRESET || reset == OS_RESET_RESTART) {
        /* "Reset" from the game (e.g. the A+B+X+Y+Start reset during a match
         * is handled by the game itself; this is a real restart) */
        xhw_reboot_self();
    }
    xhw_quit_to_dashboard();
}

u32 OSGetResetCode(void) { return s_reset_code; }
u32 OSGetProgressiveMode(void) { return s_progressive; }
void OSSetProgressiveMode(u32 on) { s_progressive = on; }
u32 OSGetEuRgb60Mode(void) { return s_eurgb60; }
void OSSetEuRgb60Mode(u32 on) { s_eurgb60 = on; }
BOOL OSCheckActiveThreads(void) { return 1; }

OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler) {
    (void)error;
    (void)handler;
    return NULL;
}
BOOL DBIsDebuggerPresent(void) { return 0; }

void DCFlushRange(void* addr, u32 n) { (void)addr; (void)n; }
void DCFlushRangeNoSync(void* addr, u32 n) { (void)addr; (void)n; }
void DCStoreRange(void* addr, u32 n) { (void)addr; (void)n; }
void DCStoreRangeNoSync(void* addr, u32 n) { (void)addr; (void)n; }
void DCInvalidateRange(void* addr, u32 n) { (void)addr; (void)n; }
void ICInvalidateRange(void* addr, u32 n) { (void)addr; (void)n; }
void PPCSync(void) {}

void OSReport(const char* msg, ...) {
    va_list ap;
    va_start(ap, msg);
    OSVReport(msg, ap);
    va_end(ap);
}

void OSVReport(const char* msg, va_list ap) {
    char buf[512];
    xsdk_vsnprintf(buf, sizeof buf, msg, ap);
    xsdk_log_raw(buf);
}

void OSPanic(const char* file, int line, const char* msg, ...) {
    char buf[512];
    char where[640];
    va_list ap;
    va_start(ap, msg);
    xsdk_vsnprintf(buf, sizeof buf, msg, ap);
    va_end(ap);
    snprintf(where, sizeof where, "%s:%d: %s", file, line, buf);
    xhw_fatal("OSPanic", where);
}

void OSFatal(GXColor fg, GXColor bg, const char* msg) {
    (void)fg;
    (void)bg;
    xhw_fatal("OSFatal", msg);
}

void pc_log_line(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    xsdk_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    xhw_log(buf);
}
