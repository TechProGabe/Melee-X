/* ar.c - ARAM and the ARAM DMA queue.
 *
 * ARAM is a host buffer at a fixed VA; ARAM "addresses" are offsets into it,
 * which is what PC_IS_ARAM_ADDR (below 16 MB) relies on. It is committed on
 * demand (xhw_reserve_lazy): only the parts the game fills cost Xbox RAM. ARQ transfers are
 * plain copies done at post time; their callbacks are queued and delivered by
 * a worker thread holding the interrupt lock, the way the ARAM interrupt ran:
 * as soon as the poster re-enables interrupts, whatever the game thread is
 * doing. Loaders spin on flags those callbacks set (HSD_SynthSFXWaitForLoad-
 * Completion), so waiting for the game thread's next frame would deadlock.
 *
 * Disc-backed pages. Melee fills ARAM with files it preloads, and with the
 * 24 MB MEM1 that is more than the Xbox has once the game has been running
 * a while. A copy into ARAM that the DVD worker just read from the disc
 * image (the devcom relay: disc -> relay buffer -> ARAM, posted from the
 * read's callback) only records, per 4 KB page, where on the image those
 * bytes are. A 64 KB chunk whose pages are all on the disc gives its memory
 * back; reading it back into MEM1 reads the image; a CPU touch (the audio
 * mixer reads samples in ARAM) faults, and the chunk is committed and
 * filled from the image (fill_chunk). Everything else is kept in memory as
 * before. */
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

#define PAGE 4096u
#define NPAGES (ARAM_SIZE / PAGE)
#define CHUNK XHW_LAZY_CHUNK
#define PER_CHUNK (CHUNK / PAGE)
#define NO_DISC 0xFFFFFFFFu

static u8* s_aram;
static u32 s_disc[NPAGES];   /* page -> disc image offset of its bytes, or NO_DISC */
static u32 s_disc_pages;
static xhw_mutex* s_lock;    /* s_disc and chunk states; recursive */
static u32 s_top = ARAM_USER_BASE;
static u32* s_stack;
static u32 s_stack_n, s_stack_max;
static ARCallback s_dma_cb;

static void fill_chunk(void* chunk);

static void aram_map(void) {
    u32 i;
    if (s_aram) return;
    for (i = 0; i < NPAGES; i++) s_disc[i] = NO_DISC;
    s_lock = xhw_mutex_create();
    s_aram = (u8*)xhw_reserve_lazy(XSDK_ARAM_VA, ARAM_SIZE);
    if (!s_aram) xhw_fatal("Out of memory", "Could not reserve ARAM.");
    xhw_lazy_set_fill(s_aram, fill_chunk);
}

u32 xsdk_aram_disc_kb(void) { return s_disc_pages * (PAGE / 1024); }

static void set_disc(u32 page, u32 off) {
    if ((s_disc[page] == NO_DISC) != (off == NO_DISC)) s_disc_pages += off == NO_DISC ? (u32)-1 : 1u;
    s_disc[page] = off;
}

/* pages [p, p + n) from the image into dst, runs of consecutive pages in
 * one read; pages not on the disc were never written: zero */
static void read_pages(u32 p, u32 n, u32 in, u8* dst, u32 len) {
    while (len) {
        u32 run = PAGE - in, k = 1;
        if (run > len) run = len;
        if (s_disc[p] == NO_DISC) {
            memset(dst, 0, run);
        } else {
            while (run < len && k < n && s_disc[p + k] == s_disc[p] + k * PAGE) {
                run += len - run < PAGE ? len - run : PAGE;
                k++;
            }
            if (!xsdk_dvd_image_read(s_disc[p] + in, dst, run)) {
                xhw_logf("[AR] image read failed: %08x + %u", (unsigned)(s_disc[p] + in), (unsigned)run);
                memset(dst, 0, run);
            }
        }
        dst += run;
        len -= run;
        p += k;
        n -= k;
        in = 0;
    }
}

/* xhw_lazy fill: the chunk was just committed (zeroed) */
static void fill_chunk(void* chunk) {
    u32 c = (u32)((u8*)chunk - s_aram) / CHUNK;
    xhw_mutex_lock(s_lock);
    if (!xhw_lazy_is_committed(chunk)) read_pages(c * PER_CHUNK, PER_CHUNK, 0, (u8*)chunk, CHUNK);
#ifdef XSDK_ARAM_VERIFY
    {
        static u32 nfill;
        if ((++nfill & 15) == 1) xhw_logf("[AR] verify: %u chunks filled, %u KB on disc", nfill, xsdk_aram_disc_kb());
    }
#endif
    xhw_mutex_unlock(s_lock);
}

static int chunk_on_disc(u32 c) {
    u32 p;
    for (p = c * PER_CHUNK; p < (c + 1) * PER_CHUNK; p++)
        if (s_disc[p] == NO_DISC) return 0;
    return 1;
}

/* main memory -> ARAM; disc: the image offset of src's bytes, or NO_DISC */
static void aram_write(u32 aram, const u8* src, u32 len, u32 disc) {
    u32 end = aram + len;
    while (aram < end) {
        u32 c = aram / CHUNK, cend = (c + 1) * CHUNK < end ? (c + 1) * CHUNK : end, a;
        u8* chunk = s_aram + c * CHUNK;
        for (a = aram; a < cend;) {
            u32 p = a / PAGE, in = a % PAGE, n = PAGE - in < cend - a ? PAGE - in : cend - a;
            if (disc != NO_DISC && n == PAGE) {
                set_disc(p, disc + (a - aram));
                if (xhw_lazy_is_committed(chunk)) memcpy(s_aram + a, src + (a - aram), n);
            } else {
                xhw_commit(s_aram + a, n);   /* filled first if it was on the disc */
                memcpy(s_aram + a, src + (a - aram), n);
                set_disc(p, NO_DISC);
            }
            a += n;
        }
        if (disc != NO_DISC && xhw_lazy_is_committed(chunk) && chunk_on_disc(c)) xhw_lazy_decommit(chunk);
        src += cend - aram;
        if (disc != NO_DISC) disc += cend - aram;
        aram = cend;
    }
}

/* ARAM -> main memory */
static void aram_read(u32 aram, u8* dst, u32 len) {
    u32 end = aram + len;
    while (aram < end) {
        u32 c = aram / CHUNK, cend = (c + 1) * CHUNK < end ? (c + 1) * CHUNK : end;
        if (xhw_lazy_is_committed(s_aram + c * CHUNK))
            memcpy(dst, s_aram + aram, cend - aram);
        else
            read_pages(aram / PAGE, (cend - 1) / PAGE - aram / PAGE + 1, aram % PAGE, dst, cend - aram);
        dst += cend - aram;
        aram = cend;
    }
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
    xhw_commit((const void*)mram, length);
    xhw_mutex_lock(s_lock);
    if (type == 0) {
        u32 disc;
        if (!xsdk_dvd_disc_source((const void*)mram, length, &disc)) disc = NO_DISC;
#ifdef XSDK_ARAM_VERIFY
        {
            static u8 tmp[0x80000];
            static u32 nchk, nbad;
            if (disc != NO_DISC && length <= sizeof tmp) {
                nchk++;
                if (!xsdk_dvd_image_read(disc, tmp, length) || memcmp(tmp, (const void*)mram, length)) nbad++;
                if ((nchk & 63) == 1 || nbad == 1) xhw_logf("[AR] verify: %u disc copies checked, %u differ", nchk, nbad);
            }
        }
#endif
        aram_write((u32)aram, (const u8*)mram, length, disc);
    } else {
        aram_read((u32)aram, (u8*)mram, length);
    }
    xhw_mutex_unlock(s_lock);
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
