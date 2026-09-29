/* ar.c - ARAM and the ARAM DMA queue.
 *
 * ARAM is a host buffer at a fixed VA; ARAM "addresses" are offsets into it,
 * which is what PC_IS_ARAM_ADDR (below 16 MB) relies on. It is committed on
 * demand (xhw_reserve_lazy): only the parts the game fills cost Xbox RAM. ARQ transfers are
 * plain copies done at post time; their callbacks are queued and delivered by
 * a worker thread holding the interrupt lock, the way the ARAM interrupt ran:
 * as soon as the poster re-enables interrupts, whatever the game thread is
 * doing. Loaders spin on flags those callbacks set (HSD_SynthSFXWaitForLoad-
 * Completion), so waiting for the game thread's next frame would deadlock. */
#include <dolphin/ar.h>
#include <dolphin/os.h>
#include <string.h>

#include "xhw.h"
#include "xsdk.h"

#ifndef XSDK_ARAM_VA
#define XSDK_ARAM_VA 0x12000000u
#endif
#define ARAM_SIZE (16u * 1024 * 1024)
#define ARAM_USER_BASE 0x4000u   /* the SDK keeps the first 16 KB for the DSP */

static u8* s_aram;
static u32 s_top = ARAM_USER_BASE;
static u32* s_stack;
static u32 s_stack_n, s_stack_max;
static ARCallback s_dma_cb;

static void aram_map(void) {
    if (s_aram) return;
    s_aram = (u8*)xhw_reserve_lazy(XSDK_ARAM_VA, ARAM_SIZE);
    if (!s_aram) xhw_fatal("Out of memory", "Could not reserve ARAM.");
}

u8* aurora_aram_base(void) {
    aram_map();
    return s_aram;
}
void* ARGetStorageAddress(void) { return aurora_aram_base(); }
void* xsdk_aram_base(void) { return aurora_aram_base(); }
u32 xsdk_aram_size(void) { return ARAM_SIZE; }

u32 ARInit(u32* stack_index_addr, u32 num_entries) {
    aram_map();
    s_stack = stack_index_addr;
    s_stack_max = num_entries;
    s_stack_n = 0;
    s_top = ARAM_USER_BASE;
    return ARAM_USER_BASE;
}

BOOL ARCheckInit(void) { return s_aram != NULL; }
void ARReset(void) {}
void ARSetSize(void) {}
u32 ARGetBaseAddress(void) { return ARAM_USER_BASE; }
u32 ARGetSize(void) { return ARAM_SIZE; }
u32 ARGetInternalSize(void) { return ARAM_SIZE; }
void ARClear(u32 flag) { (void)flag; }

u32 ARAlloc(u32 length) {
    u32 at = s_top;
    if (s_stack && s_stack_n < s_stack_max) s_stack[s_stack_n++] = length;
    s_top += length;
    return at;
}

u32 ARFree(u32* length) {
    u32 len = 0;
    if (s_stack && s_stack_n) len = s_stack[--s_stack_n];
    s_top -= len;
    if (length) *length = len;
    return s_top;
}

ARCallback ARRegisterDMACallback(ARCallback cb) {
    ARCallback old = s_dma_cb;
    s_dma_cb = cb;
    return old;
}

u32 ARGetDMAStatus(void) { return 0; }

/* type 0: main memory -> ARAM; 1: ARAM -> main memory */
static void copy(u32 type, uintptr_t mram, uintptr_t aram, u32 length) {
    aram_map();
    if (aram + length > ARAM_SIZE) {
        xhw_logf("[AR] DMA out of range: %08x + %u", (unsigned)aram, length);
        return;
    }
    xhw_commit(s_aram + aram, length);
    xhw_commit((const void*)mram, length);
    if (type == 0) memcpy(s_aram + aram, (const void*)mram, length);
    else memcpy((void*)mram, s_aram + aram, length);
}

void ARStartDMA(u32 type, u32 mainmem_addr, u32 aram_addr, u32 length) {
    copy(type, (uintptr_t)mainmem_addr, aram_addr, length);
    if (s_dma_cb) s_dma_cb();
}

u16 __ARGetInterruptStatus(void) { return 0; }
void __ARClearInterrupt(void) {}

/* ---- ARQ ---- */
#define QMAX 128
static ARQRequest* s_done[QMAX];
static int s_done_head, s_done_n;
static int s_arq_init;
static xhw_event* s_arq_event;

static void arq_worker(void* arg) {
    (void)arg;
    for (;;) {
        xhw_event_wait(s_arq_event, 100);
        xsdk_arq_deliver();
    }
}

void ARQInit(void) {
    if (!s_arq_event) {
        s_arq_event = xhw_event_create();
        xhw_thread_start(arq_worker, NULL, 1, 64 * 1024);
    }
    s_arq_init = 1;
}
void ARQReset(void) {}
BOOL ARQCheckInit(void) { return s_arq_init; }
void ARQSetChunkSize(u32 size) { (void)size; }
u32 ARQGetChunkSize(void) { return 4096; }
int aurora_arq_inflight(void) { return s_done_n; }

void ARQPostRequest(ARQRequest* r, uintptr_t owner, u32 type, u32 priority, uintptr_t source, uintptr_t dest,
                    u32 length, ARQCallback callback) {
    BOOL intr;
    r->next = NULL;
    r->owner = owner;
    r->type = type;
    r->priority = priority;
    r->source = source;
    r->dest = dest;
    r->length = length;
    r->callback = callback;
    if ((type == 0 ? dest : source) + length > ARAM_SIZE)
        xhw_logf("[AR] ARQ type %u %08x -> %08x + %u from %p", (unsigned)type, (unsigned)source, (unsigned)dest,
                 (unsigned)length, __builtin_return_address(0));
    if (type == 0) copy(0, source, dest, length);
    else copy(1, dest, source, length);
    if (!callback) return;
    intr = OSDisableInterrupts();
    if (s_done_n == QMAX) OSPanic(__FILE__, __LINE__, "ARQ completion queue overflow");
    s_done[(s_done_head + s_done_n++) % QMAX] = r;
    OSRestoreInterrupts(intr);
    if (s_arq_event) xhw_event_signal(s_arq_event);
}

void ARQRemoveRequest(ARQRequest* r) {
    int i;
    BOOL intr = OSDisableInterrupts();
    for (i = 0; i < s_done_n; i++)
        if (s_done[(s_done_head + i) % QMAX] == r) s_done[(s_done_head + i) % QMAX] = NULL;
    OSRestoreInterrupts(intr);
}

void ARQRemoveOwnerRequest(uintptr_t owner) {
    int i;
    BOOL intr = OSDisableInterrupts();
    for (i = 0; i < s_done_n; i++) {
        ARQRequest* r = s_done[(s_done_head + i) % QMAX];
        if (r && r->owner == owner) s_done[(s_done_head + i) % QMAX] = NULL;
    }
    OSRestoreInterrupts(intr);
}

void ARQFlushQueue(void) { xsdk_arq_deliver(); }

/* Callbacks run with interrupts disabled, as in the interrupt handler, so
 * the worker and the game thread's frame-boundary delivery never overlap. */
void xsdk_arq_deliver(void) {
    BOOL intr = OSDisableInterrupts();
    while (s_done_n > 0) {
        ARQRequest* r = s_done[s_done_head];
        s_done_head = (s_done_head + 1) % QMAX;
        s_done_n--;
        if (r && r->callback) r->callback(r);
    }
    OSRestoreInterrupts(intr);
}
