/* nv2a.c - the NV2A back end of the GX front end (xgx.h).
 *
 * Per draw it turns the GX state into:
 *   - a vertex program (nv2a_vp.c) for the transform/lighting/texgen
 *     configuration, cached and kept resident in program memory,
 *   - vertex-program constants: projection with the viewport and the content
 *     rect folded in, the ten GX position/normal matrices (skinned through
 *     a0 = PNMTXIDX), lights, material colours, texgen matrices; only rows
 *     that changed are sent,
 *   - a register-combiner program (nv2a_rc.c) for the TEV configuration,
 *     cached, with its constants resolved from the konst colours and TEV
 *     registers,
 *   - up to four texture units, and the fixed-function pixel state,
 * and draws the vertices the front end decoded into a contiguous ring.
 *
 * Output (docs/architecture.md): the 640x480 logical EFB maps onto a content
 * rect of the framebuffer: the whole 1280x720 (16:9, 720p) or 640x480 frame,
 * pillarboxed when a scene asks for the GameCube's own 73:60 picture.
 * Hardware knowledge (register values that bit, pushbuffer and scanout
 * rules) is OpenCrossing-Xbox's; see its docs/traps.md. */
#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <pbkit/nv_regs.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "nv2a_fog.h"
#include "nv2a_rc.h"
#include "nv2a_vp.h"
#include "nv2a_vpmem.h"
#include "xgx.h"
#include "xhw.h"
#include "xhw_internal.h"

void xhw_video_fallback_480(void);
extern unsigned int pb_DepthFmt;   /* settable: tools/xbox/patch_pbkit.py */
void ocx_pb_retarget_back_buffer(void);   /* tools/xbox/patch_pbkit.py */
void ocx_pb_layout(unsigned* out);        /* tools/xbox/patch_pbkit.py */

/* GX values used here (dolphin headers are not on the hw include path) */
enum { GX_CULL_NONE, GX_CULL_FRONT, GX_CULL_BACK, GX_CULL_ALL };
enum { GX_BM_NONE, GX_BM_BLEND, GX_BM_LOGIC, GX_BM_SUBTRACT };
enum { GX_AOP_AND, GX_AOP_OR, GX_AOP_XOR, GX_AOP_XNOR };
enum { GX_NEVER, GX_LESS, GX_EQUAL, GX_LEQUAL, GX_GREATER, GX_NEQUAL, GX_GEQUAL, GX_ALWAYS };
enum { GX_CLAMP, GX_REPEAT, GX_MIRROR };
enum { GX_TG_MTX3x4 = 0, GX_TG_MTX2x4 = 1, GX_TG_BUMP0 = 2, GX_TG_BUMP7 = 9 };
enum { GX_TG_TEXCOORD0 = 12, GX_TG_TEXCOORD6 = 18 };
enum { GX_AF_SPEC = 0, GX_AF_SPOT = 1, GX_AF_NONE = 2 };
enum { GX_COLOR0A0 = 4, GX_COLOR1A1 = 5 };
enum { GX_ITF_8 = 0, GX_ITM_OFF = 0, GX_ITM_2 = 3, GX_ITW_OFF = 0, GX_ITBA_OFF = 0 };
#define GX_TEXMTX0 30
#define GX_IDENTITY 60
#define GX_PTTEXMTX0 64
#define GX_PTIDENTITY 125
#define GX_NULL 0xFF

/* ======================================================================
 * Output geometry
 * ====================================================================== */
static int s_fbw = 640, s_fbh = 480, s_bpp = 32;
static float s_zmax = 16777215.0f;
static uint32_t s_draw_force = XGX_DIRTY_ALL;   /* groups xgx_draw must rebuild regardless of dirty bits */
static float s_display_aspect = 4.0f / 3.0f;
static float s_content_aspect = 73.0f / 60.0f;
static int s_cx, s_cy, s_cw = 640, s_ch = 480;   /* content rect, framebuffer pixels */

static void update_content_rect(void) {
    float a = s_content_aspect < s_display_aspect ? s_content_aspect : s_display_aspect;
    s_ch = s_fbh;
    s_cw = (int)(s_fbw * a / s_display_aspect + 0.5f);
    s_cw &= ~1;
    s_cx = (s_fbw - s_cw) / 2;
    s_cy = 0;
}

void xgx_output_size(uint32_t* w, uint32_t* h) {
    *w = (uint32_t)s_fbw;
    *h = (uint32_t)s_fbh;
}

void xgx_set_content_aspect(float aspect) {
    if (aspect <= 0.0f || fabsf(aspect - s_content_aspect) < 1e-4f) return;
    s_content_aspect = aspect;
    update_content_rect();
    s_draw_force = XGX_DIRTY_ALL;
}

void xgx_content_size(uint32_t* w, uint32_t* h) {
    float a = s_content_aspect < s_display_aspect ? s_content_aspect : s_display_aspect;
    *h = (uint32_t)s_fbh;
    *w = (uint32_t)(s_fbh * a + 0.5f);
}

/* logical (EFB) -> framebuffer, edges rounded so abutting rects stay abutting */
static int map_x(float x) { return s_cx + (int)floorf(x * (float)s_cw / XGX_EFB_W + 0.5f); }
static int map_y(float y) { return s_cy + (int)floorf(y * (float)s_ch / XGX_EFB_H + 0.5f); }
/* the framebuffer pixel that holds an EFB position (map_x/map_y round an edge
 * to the nearest pixel boundary; a sample point takes the pixel it is in) */
static int pix_x(float x) { return s_cx + (int)floorf(x * (float)s_cw / XGX_EFB_W); }
static int pix_y(float y) { return s_cy + (int)floorf(y * (float)s_ch / XGX_EFB_H); }

/* ======================================================================
 * Contiguous memory: texture pool and vertex ring
 * ====================================================================== */
/* sized for 64 MB next to the game (docs/architecture.md "Memory"). 6 MB
 * ran out on the console (textures dropped, drawn black): the menus' and
 * the stage's working sets meet at the start of a match. xemu still has
 * ~14 MB free on the results screen; if the pool can't be had, init falls
 * back 1 MB at a time down to TEX_POOL_MIN. -DXGX_TEX_POOL_KB=<n> sets it. */
#ifdef XGX_TEX_POOL_KB
#define TEX_POOL_480 ((uint32_t)XGX_TEX_POOL_KB * 1024)
#define TEX_POOL_720 ((uint32_t)XGX_TEX_POOL_KB * 1024)
#else
#define TEX_POOL_480 (8u * 1024 * 1024)
#define TEX_POOL_720 (6u * 1024 * 1024)
#endif
#define TEX_POOL_MIN (4u * 1024 * 1024)
#define RING_BYTES (1536u * 1024)
#ifndef XGX_EFB_GPU_COPY
#define XGX_EFB_GPU_COPY 1   /* EFB -> texture copies drawn by the GPU (efb_copy_gpu); 0: CPU readback */
#endif
/* cached display lists (gx_vtx.c); the results screen (6500 draws) filled
 * 2 MB and rebuilt lists every frame. 720p has 4 MB too: its 3 MB was
 * full in the first match on the console (v38, 0 KB free) */
#define VB_POOL_480 (4096u * 1024)
#define VB_POOL_720 (4096u * 1024)
#define VB_POOL_MIN (2048u * 1024)
/* pbkit ignores a size that isn't a power of two and keeps its 512 KB: the
 * 1.5 MB asked for before left PB_GUARD past the real end, and Pokémon
 * Stadium frames (750 draws, six EFB copies) ran off it into whatever memory
 * follows; the GPU then fetched texture data as methods and stopped */
#define PB_BYTES (1024u * 1024)
_Static_assert((PB_BYTES & (PB_BYTES - 1)) == 0 && PB_BYTES >= 64 * 1024, "pb_size takes powers of two only");
#define POOL_ALIGN 128
#define POOL_BIG (256 * 1024)

/* Blocks in address order (next/prev), the free ones also on a list of
 * their own, the allocated ones in a hash by offset: with ~2000 cached lists
 * and textures, walking every block per allocation and twice per free took
 * ~5% of the console's CPU on Pokémon Stadium. Placement is unchanged: small
 * requests take the lowest free block that fits, big ones the top of the
 * highest (keeping the two apart against fragmentation). */
#define POOL_HASH 4096
typedef struct Blk { uint32_t off, size; int free; struct Blk *next, *prev, *fnext, *fprev, *hnext; } Blk;
typedef struct {
    uint8_t* base;
    uint32_t bytes, used;
    Blk* blocks;
    Blk* freelist;
    Blk* hash[POOL_HASH];
} Pool;
static Pool s_tp;   /* textures */
static Pool s_vb;   /* cached display-list vertices (gx_vtx.c) */
static Pool s_tp2;  /* textures past s_tp: only while a scene's working set outgrows it (xgx_tex_pool_grow) */

static void fl_add(Pool* pl, Blk* b) {
    b->fprev = NULL;
    b->fnext = pl->freelist;
    if (pl->freelist) pl->freelist->fprev = b;
    pl->freelist = b;
}

static void fl_del(Pool* pl, Blk* b) {
    if (b->fprev) b->fprev->fnext = b->fnext;
    else pl->freelist = b->fnext;
    if (b->fnext) b->fnext->fprev = b->fprev;
}

static unsigned hkey(uint32_t off) { return (off / POOL_ALIGN) & (POOL_HASH - 1); }

static void h_add(Pool* pl, Blk* b) {
    unsigned k = hkey(b->off);
    b->hnext = pl->hash[k];
    pl->hash[k] = b;
}

static Blk* h_take(Pool* pl, uint32_t off) {
    Blk** pp;
    for (pp = &pl->hash[hkey(off)]; *pp; pp = &(*pp)->hnext)
        if ((*pp)->off == off) {
            Blk* b = *pp;
            *pp = b->hnext;
            return b;
        }
    return NULL;
}

static int pool_init(Pool* pl, uint32_t bytes) {
    memset(pl, 0, sizeof *pl);
    pl->base = (uint8_t*)MmAllocateContiguousMemoryEx(bytes, 0, MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
    pl->blocks = pl->base ? (Blk*)calloc(1, sizeof(Blk)) : NULL;
    if (!pl->blocks) {
        if (pl->base) MmFreeContiguousMemory(pl->base);
        memset(pl, 0, sizeof *pl);
        return 0;
    }
    pl->bytes = bytes;
    pl->used = 0;
    pl->blocks->size = bytes;
    pl->blocks->free = 1;
    fl_add(pl, pl->blocks);
    return 1;
}

static void pool_release(Pool* pl) {
    Blk* b = pl->blocks;
    while (b) {
        Blk* n = b->next;
        free(b);
        b = n;
    }
    if (pl->base) MmFreeContiguousMemory(pl->base);
    memset(pl, 0, sizeof *pl);
}

static void blk_insert_after(Blk* b, Blk* n) {
    n->prev = b;
    n->next = b->next;
    if (b->next) b->next->prev = n;
    b->next = n;
}

static void* pool_alloc(Pool* pl, uint32_t size) {
    Blk *b, *pick = NULL;
    size = (size + POOL_ALIGN - 1) & ~(uint32_t)(POOL_ALIGN - 1);
    for (b = pl->freelist; b; b = b->fnext) {
        if (b->size < size) continue;
        if (!pick || (size < POOL_BIG ? b->off < pick->off : b->off > pick->off)) pick = b;
    }
    if (!pick) return NULL;
    b = pick;
    if (b->size > size) {
        Blk* n = (Blk*)calloc(1, sizeof(Blk));
        if (!n) return NULL;
        blk_insert_after(b, n);
        if (size < POOL_BIG) {
            n->off = b->off + size;
            n->size = b->size - size;
            n->free = 1;
            fl_add(pl, n);
            b->size = size;
        } else {
            n->off = b->off + b->size - size;
            n->size = size;
            b->size -= size;
            h_add(pl, n);
            pl->used += size;
            return pl->base + n->off;
        }
    }
    b->free = 0;
    fl_del(pl, b);
    h_add(pl, b);
    pl->used += size;
    return pl->base + b->off;
}

static int pool_owns(const Pool* pl, const void* p) {
    return pl->base && (const uint8_t*)p >= pl->base && (const uint8_t*)p < pl->base + pl->bytes;
}

static void pool_free(Pool* pl, void* p) {
    Blk *b, *n;
    if (!p) return;
    b = h_take(pl, (uint32_t)((uint8_t*)p - pl->base));
    if (!b) return;
    b->free = 1;
    pl->used -= b->size;
    if ((n = b->next) && n->free) {   /* merge the next block into this one */
        fl_del(pl, n);
        b->size += n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
        free(n);
    }
    if ((n = b->prev) && n->free) {   /* and this one into the one before */
        n->size += b->size;
        n->next = b->next;
        if (b->next) b->next->prev = n;
        free(b);
    } else {
        fl_add(pl, b);
    }
}

uint32_t xgx_tex_pool_free_kb(void) { return (s_tp.bytes - s_tp.used) / 1024; }
uint32_t xgx_tex_pool_largest_kb(void) {
    const Blk* b;
    uint32_t best = 0;
    for (b = s_tp.blocks; b; b = b->next)
        if (b->free && b->size > best) best = b->size;
    return best / 1024;
}
uint32_t xgx_tex_pool_kb(void) { return s_tp.bytes / 1024; }
uint32_t xgx_vbuf_pool_kb(void) { return s_vb.bytes / 1024; }
uint32_t xgx_vbuf_pool_free_kb(void) { return (s_vb.bytes - s_vb.used) / 1024; }

static uint8_t* s_ring;
static uint32_t s_ring_pos;
static const uint8_t* s_draw_base;   /* the next draw's vertices: ring or a cached buffer */

/* ======================================================================
 * Pushbuffer
 * ====================================================================== */
static uint32_t* P;
static int s_pb_open;
static uint32_t* s_pb_mark;
static uint32_t* s_pb_base;
/* words per kick: each one runs pbkit's pb_start, which flushes the NV2A's
 * write-combine cache and spins until it's done (pb_cache_flush, ~2% of the
 * console's CPU in a match at 4096). -DXGX_PB_KICK=4096 restores v25's. */
#ifndef XGX_PB_KICK
#define XGX_PB_KICK 8192
#endif
#define PB_KICK XGX_PB_KICK
/* -DXGX_VB_CACHE_BREAK=0 drops the vertex cache break at each batch start
 * (pb_open), -DXGX_VBUF_FREE_NOW=0 sends evicted display-list buffers
 * through the deferred free like any other (xgx_vbuf_free_now): with
 * XGX_PB_KICK they undo v26's GPU-side changes one at a time, to bisect the
 * console's GPU stalls (docs/roadmap.md "Next"). */
#ifndef XGX_VB_CACHE_BREAK
#define XGX_VB_CACHE_BREAK 1
#endif
#ifndef XGX_VBUF_FREE_NOW
#define XGX_VBUF_FREE_NOW 1
#endif
#define PB_GUARD (PB_BYTES - 192 * 1024)
/* -DXGX_OVERLAP=0: xgx_present waits for the GPU before the flip, as up to
 * v32, instead of the next frame's first GPU use (frame_open) */
#ifndef XGX_OVERLAP
#define XGX_OVERLAP 1
#endif
#define PCRTC_START_REG (*(volatile uint32_t*)0xFD600800)

static inline void put1(uint32_t m, uint32_t v) { P[0] = (1u << 18) | m; P[1] = v; P += 2; }
static inline void putf(uint32_t m, float v) { union { float f; uint32_t u; } c; c.f = v; put1(m, c.u); }

static void pb_open(void) {
    if (s_pb_open) return;
    P = pb_begin();
    s_pb_mark = P;
    s_pb_open = 1;
    /* The NV2A caches vertex data by address, and a draw's fetch reads
     * ahead of its last vertex. Once a batch is kicked the GPU can run it
     * before the CPU writes the next vertices into the ring right behind
     * it, and the next draw then takes its first vertices from the stale
     * read-ahead. HSD's shadow maps showed it on the console: each EFB copy
     * waits for idle, and the next map's background quad lost its first
     * triangle (black wedges above the diagonal, flashing black on the
     * surfaces near fighters). Every batch after a kick starts by dropping
     * that cache (xemu has none). */
    if (XGX_VB_CACHE_BREAK) put1(NV097_BREAK_VERTEX_BUFFER_CACHE, 0);
}

#ifdef XGX_DEBUG_PBCHECK
static uint32_t s_frame, s_draws;   /* defined with the frame state below */
/* -DXGX_DEBUG_PBCHECK: walk each segment before the kick and log the first
 * malformed headers. xemu forgives what the console's pusher faults on, so
 * this tells "the CPU wrote a bad stream" from "the GPU faulted on a good
 * one" without a console. */
static void pb_check(const uint32_t* p, const uint32_t* end) {
    static int logged;
    const uint32_t* start = p;
    while (p < end && logged < 16) {
        uint32_t h = *p, n = (h >> 18) & 0x7FF, m = h & 0x1FFC;
        if ((h & 0xA0000003u) || ((h >> 13) & 7) > 1 || !n || p + 1 + n > end) {
            logged++;
            xhw_logf("[PBCHECK] frame %u draw %u: bad header %08x at +%u of %u words (method %04x count %u)",
                     (unsigned)s_frame, (unsigned)s_draws, h, (unsigned)(p - start), (unsigned)(end - start), m, n);
            return;
        }
        p += 1 + n;
    }
}
#endif

static void pb_close(void) {
    if (!s_pb_open) return;
#ifdef XGX_DEBUG_PBCHECK
    pb_check(s_pb_mark, P);
#endif
    pb_end(P);
    s_pb_open = 0;
}

/* per-interval counters for the [NV2A] frame line */
static uint32_t s_st_waits, s_st_efb, s_st_tex_kb, s_st_verts, s_st_tex_fail, s_st_pb_peak, s_st_pb_resets;
static uint32_t s_st_draws, s_st_dirty_none, s_st_dirty_mtx, s_st_dirty[13];
static uint32_t s_st_prim[8];   /* by GX primitive, (prim >> 3) & 7 */
static uint32_t s_st_vp_sel, s_st_vp_loads, s_st_vp_ins;   /* program switches, loads, instructions loaded */

/* GPU faults, recorded by the patched pbkit (ocx_pb_gpu_fault below) */
static volatile uint32_t s_gf_count, s_gf_storms, s_gf_last[5];
/* The first fault, with where the pusher was and which draw of which frame
 * the CPU had reached: later ones are usually its consequences. */
static volatile uint32_t s_gf_first[9], s_gf_first_logged;
/* at the first fault: PGRAPH 0x400800-0x40080C (pbkit's "limit details" for
 * LIMIT_COLOR/LIMIT_ZETA), and the last EFB copy's target (offset, w, h, frame) */
static volatile uint32_t s_gf_limit[4];
static uint32_t s_last_copy[4];
static void log_first_fault(void);

/* A GPU that stops fetching (bad method or state) otherwise hangs the game
 * thread here with nothing in the log: after 2 s, report once where the
 * FIFO stopped. The push buffer is contiguous memory, mapped at
 * 0x80000000 | physical, and DMA_GET is its physical address. */
static uint32_t s_frame, s_draws;   /* defined with the frame state below */

static void report_gpu_stall(void) {
    uint32_t get = *(volatile uint32_t*)(0xFD000000u + 0x3244), put = *(volatile uint32_t*)(0xFD000000u + 0x3240);
    const uint32_t* w = (const uint32_t*)(0x80000000u | (get & 0x03FFFFFFu));
    /* fault: kind (1 PGRAPH: nsource, class, trapped method, data; 2 DMA pusher) */
    xhw_logf("[NV2A] GPU stalled: get %08x put %08x dma_state %08x pgraph %08x, faults %u (last kind %u %08x %08x "
             "%08x %08x)",
             get, put, *(volatile uint32_t*)(0xFD000000u + 0x3228), *(volatile uint32_t*)(0xFD000000u + 0x400700),
             (unsigned)s_gf_count, (unsigned)s_gf_last[0], (unsigned)s_gf_last[1], (unsigned)s_gf_last[2],
             (unsigned)s_gf_last[3], (unsigned)s_gf_last[4]);
    log_first_fault();
    /* GET can be anywhere once the pusher has run off into other data
     * (GitHub issue #6: 15000004), and an unmapped read here faults */
    xhw_logf("[NV2A]  pushbuffer at %08x", (uint32_t)s_pb_base & 0x03FFFFFFu);
    if (MmIsAddressValid((PVOID)(w - 8)) && MmIsAddressValid((PVOID)(w + 7))) {
        xhw_logf("[NV2A]  at get-32: %08x %08x %08x %08x %08x %08x %08x %08x", w[-8], w[-7], w[-6], w[-5], w[-4],
                 w[-3], w[-2], w[-1]);
        xhw_logf("[NV2A]  at get:    %08x %08x %08x %08x %08x %08x %08x %08x", w[0], w[1], w[2], w[3], w[4], w[5],
                 w[6], w[7]);
    } else {
        xhw_logf("[NV2A]  get is outside mapped memory");
    }
    {   /* PGRAPH: trap (what it was doing), surface, clip and raster state */
        volatile const uint32_t* g = (volatile const uint32_t*)0xFD400000u;
        xhw_logf("[NV2A]  pgraph intr %08x nsource %08x trapped %08x data %08x surface %08x | clear %08x %08x "
                 "window %08x %08x | raster %08x control0 %08x | frame %u draws %u efb %u",
                 g[0x100 / 4], g[0x108 / 4], g[0x704 / 4], g[0x708 / 4], g[0x710 / 4], g[0x1864 / 4], g[0x1868 / 4],
                 g[0x1A44 / 4], g[0x1A64 / 4], g[0x1990 / 4], g[0x194C / 4], s_frame, s_draws, s_st_efb);
    }
}

/* Set by the patched pbkit when an interrupt storm made it leave the GPU
 * interrupt masked. Vblank flips and pbkit's PB_SETOUTER calls need that
 * interrupt, so the next frame would hang for good: turn it back on (at
 * passive level, once the storm has had time to pass) and say so. */
volatile int ocx_pb_irq_off;

static void irq_recover(void) {
    if (!ocx_pb_irq_off) return;
    ocx_pb_irq_off = 0;
    s_gf_storms = 0;
    *(volatile uint32_t*)0xFD000140u = 1;   /* NV_PMC_INTR_EN_0 = INTA_HARDWARE */
    xhw_logf("[NV2A] GPU interrupt storm: interrupt re-enabled (faults %u)", (unsigned)s_gf_count);
}

/* pb_busy only compares the pusher's GET with PUT and reads PGRAPH's status:
 * methods already fetched into PFIFO's CACHE1 but not yet handed to PGRAPH
 * pass as idle whenever PGRAPH is between two of them. Callers free and
 * rewrite memory the GPU reads (deferred textures and vertex buffers, the
 * vertex ring at each frame) or writes (EFB copy targets) right after this,
 * so idle also means CACHE1 empty and the pusher stopped, seen twice. */
static int gpu_quiet(void) {
    volatile const uint32_t* r = (volatile const uint32_t*)0xFD000000u;
    return !pb_busy() && (r[0x3214 / 4] & 0x10) && !(r[0x3220 / 4] & 0x10) && !r[0x400700 / 4];
}

static int gpu_busy(void) { return !gpu_quiet() || !gpu_quiet(); }

static void wait_idle(void) {
    uint64_t t0 = 0;
    int reported = 0, pf = xhw_perf_enter(XHW_PERF_GPU);
    s_st_waits++;
    pb_close();
    while (gpu_busy()) {
        irq_recover();
        if (!t0) t0 = xhw_time_ns();
        else if (!reported && xhw_time_ns() - t0 > 2000000000ull) {
            report_gpu_stall();
            reported = 1;
        }
    }
    xhw_perf_leave(pf);
}

static uint32_t pb_used(void) {
    const uint32_t* p = s_pb_open ? P : pb_begin();
    return (uint32_t)((const uint8_t*)p - (const uint8_t*)s_pb_base);
}

/* GPU faults, recorded by the patched pbkit (tools/xbox/patch_pbkit.py) */
static uint32_t s_gf_logged;

static void log_first_fault(void) {
    if (!s_gf_count || s_gf_first_logged) return;
    xhw_logf("[NV2A] first GPU fault: kind %u %08x %08x %08x %08x, get %08x put %08x (pushbuffer %08x), frame %u draw %u",
             (unsigned)s_gf_first[0], (unsigned)s_gf_first[1], (unsigned)s_gf_first[2], (unsigned)s_gf_first[3],
             (unsigned)s_gf_first[4], (unsigned)s_gf_first[5], (unsigned)s_gf_first[6], (uint32_t)s_pb_base & 0x03FFFFFFu,
             (unsigned)s_gf_first[7], (unsigned)s_gf_first[8]);
    s_gf_first_logged = 1;
    xhw_logf("[NV2A]  first fault pgraph 400800: %08x %08x %08x %08x | last EFB copy to %08x %ux%u in frame %u",
             (unsigned)s_gf_limit[0], (unsigned)s_gf_limit[1], (unsigned)s_gf_limit[2], (unsigned)s_gf_limit[3],
             s_last_copy[0], s_last_copy[1], s_last_copy[2], s_last_copy[3]);
    {   /* what the pusher had just read: 64 words before GET, 32 from it */
        const uint32_t* w = (const uint32_t*)(0x80000000u | (s_gf_first[5] & 0x03FFFFFCu));
        int i;
        if (!MmIsAddressValid((PVOID)(w - 64)) || !MmIsAddressValid((PVOID)(w + 31))) return;
        for (i = -64; i < 32; i += 8)
            xhw_logf("[NV2A]  first fault get%+d: %08x %08x %08x %08x %08x %08x %08x %08x", i * 4, w[i], w[i + 1],
                     w[i + 2], w[i + 3], w[i + 4], w[i + 5], w[i + 6], w[i + 7]);
    }
}

void ocx_pb_gpu_fault(unsigned kind, unsigned a, unsigned b, unsigned c, unsigned d) {
    if (!s_gf_count) {
        s_gf_first[0] = kind;
        s_gf_first[1] = a;
        s_gf_first[2] = b;
        s_gf_first[3] = c;
        s_gf_first[4] = d;
        s_gf_first[5] = *(volatile uint32_t*)(0xFD000000u + 0x3244);   /* DMA GET */
        s_gf_first[6] = *(volatile uint32_t*)(0xFD000000u + 0x3240);   /* DMA PUT */
        s_gf_first[7] = s_frame;
        s_gf_first[8] = s_draws;
        s_gf_limit[0] = *(volatile uint32_t*)(0xFD400800u);
        s_gf_limit[1] = *(volatile uint32_t*)(0xFD400804u);
        s_gf_limit[2] = *(volatile uint32_t*)(0xFD400808u);
        s_gf_limit[3] = *(volatile uint32_t*)(0xFD40080Cu);
    }
    s_gf_last[0] = kind;
    s_gf_last[1] = a;
    s_gf_last[2] = b;
    s_gf_last[3] = c;
    s_gf_last[4] = d;
    s_gf_count++;
    if (kind == 3 && ++s_gf_storms >= 16) ocx_pb_irq_off = 1;
}

/* ======================================================================
 * Textures
 * ====================================================================== */
#define MAX_TEX 4096
typedef struct {
    int used;
    uint16_t w, h;          /* after POT resampling; a rect texture's own size */
    uint8_t levels;
    uint8_t rect;           /* linear (LU_IMAGE) non-power-of-two image, sampled in texels */
    uint16_t pitch;         /* rect: bytes per row */
    uint8_t nvfmt;          /* NV097_SET_TEXTURE_FORMAT_COLOR_* */
    uint32_t bytes;         /* in the pool */
    void* mem;              /* level 0 */
    void* base;             /* the allocation: the palette of a P8 texture, then its levels */
    uint32_t pal;           /* P8: SET_TEXTURE_PALETTE value; 0: none */
    uint32_t gen;           /* bumped per texture made: a new one at a freed one's address differs */
} Tex;
static Tex s_tex[MAX_TEX];
static int s_tex_next = 1;
static void* s_deferred[4096];
static int s_ndeferred;

static void release_deferred(void) {
    int i;
    for (i = 0; i < s_ndeferred; i++)
        pool_free(pool_owns(&s_vb, s_deferred[i]) ? &s_vb : pool_owns(&s_tp2, s_deferred[i]) ? &s_tp2 : &s_tp,
                  s_deferred[i]);
    s_ndeferred = 0;
}

static void defer_free(void* p) {
    if (s_ndeferred == (int)(sizeof s_deferred / sizeof s_deferred[0])) {
        wait_idle();
        release_deferred();
    }
    s_deferred[s_ndeferred++] = p;
}

static uint32_t swz_x[2048], swz_y[2048];
static int swz_w, swz_h;

static void swz_tables(int w, int h) {
    uint32_t xm = 0, ym = 0, bit = 1, mbit = 1;
    int done, i;
    if (w == swz_w && h == swz_h) return;
    do {
        done = 1;
        if (bit < (uint32_t)w) { xm |= mbit; mbit <<= 1; done = 0; }
        if (bit < (uint32_t)h) { ym |= mbit; mbit <<= 1; done = 0; }
        bit <<= 1;
    } while (!done);
    for (i = 0; i < w; i++) {
        uint32_t v = 0, m = 1, mask = xm;
        while (mask) { uint32_t low = mask & -mask; if ((uint32_t)i & m) v |= low; m <<= 1; mask &= mask - 1; }
        swz_x[i] = v;
    }
    for (i = 0; i < h; i++) {
        uint32_t v = 0, m = 1, mask = ym;
        while (mask) { uint32_t low = mask & -mask; if ((uint32_t)i & m) v |= low; m <<= 1; mask &= mask - 1; }
        swz_y[i] = v;
    }
    swz_w = w;
    swz_h = h;
}

static int pot(int v) { int p = 1; while (p < v) p <<= 1; return p; }
static int log2i(int v) { int l = 0; while ((1 << l) < v) l++; return l; }

static void write_level_direct(void* dstv, const void* srcv, int w, int h, int pw, int ph, int bpp);

/* One level into swizzled (Morton) order; NPOT images are resampled.
 * Texture memory is write-combined, and swizzled stores land all over it,
 * which defeats write combining: swizzle into a cached buffer first, then
 * copy it over in order. */
static void write_level(void* dstv, const void* srcv, int w, int h, int pw, int ph, int bpp) {
    static uint8_t* scratch;
    static uint32_t scratch_bytes;
    uint32_t bytes = (uint32_t)(pw * ph * bpp);
    if (bytes > scratch_bytes && bytes <= 1024u * 1024) {
        free(scratch);
        scratch = (uint8_t*)malloc(bytes);
        scratch_bytes = scratch ? bytes : 0;
    }
    if (bytes > scratch_bytes || bytes < 256) {
        write_level_direct(dstv, srcv, w, h, pw, ph, bpp);
        return;
    }
    write_level_direct(scratch, srcv, w, h, pw, ph, bpp);
    memcpy(dstv, scratch, bytes);
}

/* Bilinear resample of one level to pw x ph (NPOT -> POT, so every wrap
 * mode keeps working and texcoords need no rescale), per 8-bit channel:
 * A8R8G8B8, AY8, A8Y8. Fixed point with per-column tables: movie frames
 * (640x480 Y/U/V planes, a new one every frame) went through it as floats. */
static void resample_level(uint8_t* dst, const uint8_t* src, int w, int h, int pw, int ph, int bpp) {
    static uint16_t cx0[1024], cx1[1024];
    static uint8_t cfx[1024];
    int x, y, k;
    for (x = 0; x < pw; x++) {
        int fx = ((2 * x + 1) * w * 128) / pw - 128;   /* ((x + 0.5) * w / pw - 0.5) * 256 */
        if (fx < 0) fx = 0;
        cx0[x] = (uint16_t)(fx >> 8);
        cx1[x] = (uint16_t)((fx >> 8) + 1 < w ? (fx >> 8) + 1 : fx >> 8);
        cfx[x] = (uint8_t)(fx & 255);
    }
    for (y = 0; y < ph; y++) {
        int fy = ((2 * y + 1) * h * 128) / ph - 128, y0, y1;
        uint32_t ty, yo = swz_y[y];
        const uint8_t *r0, *r1;
        if (fy < 0) fy = 0;
        y0 = fy >> 8;
        y1 = y0 + 1 < h ? y0 + 1 : y0;
        ty = (uint32_t)(fy & 255);
        r0 = src + (size_t)y0 * (size_t)w * (size_t)bpp;
        r1 = src + (size_t)y1 * (size_t)w * (size_t)bpp;
        for (x = 0; x < pw; x++) {
            const uint8_t *a = r0 + cx0[x] * bpp, *b = r0 + cx1[x] * bpp, *c = r1 + cx0[x] * bpp,
                          *d = r1 + cx1[x] * bpp;
            uint32_t tx = cfx[x];
            uint8_t* o = dst + (yo | swz_x[x]) * (uint32_t)bpp;
            for (k = 0; k < bpp; k++) {
                uint32_t top = a[k] * (256 - tx) + b[k] * tx, bot = c[k] * (256 - tx) + d[k] * tx;
                o[k] = (uint8_t)((top * (256 - ty) + bot * ty + 32768) >> 16);
            }
        }
    }
}

static void write_level_direct(void* dstv, const void* srcv, int w, int h, int pw, int ph, int bpp) {
    int x, y;
    swz_tables(pw, ph);
    if (w != pw || h != ph) {
        resample_level((uint8_t*)dstv, (const uint8_t*)srcv, w, h, pw, ph, bpp);
        return;
    }
    for (y = 0; y < ph; y++) {
        uint32_t yo = swz_y[y];
        switch (bpp) {
            case 4: {
                const uint32_t* row = (const uint32_t*)srcv + y * w;
                uint32_t* dst = (uint32_t*)dstv;
                for (x = 0; x < pw; x++) dst[yo | swz_x[x]] = row[x];
                break;
            }
            case 2: {
                const uint16_t* row = (const uint16_t*)srcv + y * w;
                uint16_t* dst = (uint16_t*)dstv;
                for (x = 0; x < pw; x++) dst[yo | swz_x[x]] = row[x];
                break;
            }
            default: {
                const uint8_t* row = (const uint8_t*)srcv + y * w;
                uint8_t* dst = (uint8_t*)dstv;
                for (x = 0; x < pw; x++) dst[yo | swz_x[x]] = row[x];
                break;
            }
        }
    }
}

static int fmt_bpp(uint32_t fmt) {
    switch (fmt) {
        case XGX_TEX_RGB565: case XGX_TEX_A8Y8: return 2;
        case XGX_TEX_AY8: case XGX_TEX_P8: return 1;
        case XGX_TEX_DXT1: case XGX_TEX_DXT3: return 0;
        default: return 4;
    }
}

static uint8_t nv_format(uint32_t fmt) {
    switch (fmt) {
        case XGX_TEX_RGB565: return NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5;
        case XGX_TEX_AY8: return NV097_SET_TEXTURE_FORMAT_COLOR_SZ_AY8;
        case XGX_TEX_A8Y8: return NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8;
        case XGX_TEX_DXT1: return NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5;
        case XGX_TEX_DXT3: return NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8;
        case XGX_TEX_P8: return NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8;
        default: return NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8;
    }
}

static uint32_t level_size(uint32_t fmt, int w, int h) {
    if (fmt == XGX_TEX_DXT1) return (uint32_t)(((w + 3) / 4) * ((h + 3) / 4) * 8);
    if (fmt == XGX_TEX_DXT3) return (uint32_t)(((w + 3) / 4) * ((h + 3) / 4) * 16);
    return (uint32_t)(w * h * fmt_bpp(fmt));
}

static int alloc_handle(void) {
    int k;
    for (k = 0; k < MAX_TEX - 1; k++) {
        int c = s_tex_next + k;
        if (c >= MAX_TEX) c = 1 + (c % (MAX_TEX - 1));
        if (!s_tex[c].used) {
            s_tex_next = c + 1;
            return c;
        }
    }
    return 0;
}

static void* tex_pool_alloc(uint32_t bytes) {
    uint8_t* base = (uint8_t*)pool_alloc(&s_tp, bytes);
    if (!base && s_tp2.base) base = (uint8_t*)pool_alloc(&s_tp2, bytes);
    if (!base && s_ndeferred) {   /* nothing to gain from waiting when nothing is pending */
        wait_idle();
        release_deferred();
        base = (uint8_t*)pool_alloc(&s_tp, bytes);
        if (!base && s_tp2.base) base = (uint8_t*)pool_alloc(&s_tp2, bytes);
    }
    if (!base) s_st_tex_fail++;
    return base;
}

/* Non-power-of-two images as they are: a linear (LU_IMAGE) texture of the
 * image's own size, rows 64-byte aligned, sampled in texel coordinates (the
 * texgen rows of a unit that binds one are scaled by its size,
 * build_texgen). GX allows only clamping and no mipmaps for such sizes, which
 * is what linear textures do. Resampling to a power of two blurred them a
 * little and cost the movie ~18 ms a frame on the console (640x480 Y/U/V
 * planes, new every frame); now it is one row copy. */
static uint32_t tex_create_rect(uint32_t w, uint32_t h, uint32_t fmt, const uint8_t* src) {
    int id, bpp = fmt_bpp(fmt);
    uint32_t row = w * (uint32_t)bpp, pitch = (row + 63) & ~63u, bytes = pitch * h, y;
    uint8_t* base;
    id = alloc_handle();
    if (!id) return 0;
    s_st_tex_kb += bytes / 1024;
    base = (uint8_t*)tex_pool_alloc(bytes);
    if (!base) return 0;
    for (y = 0; y < h; y++) memcpy(base + y * pitch, src + y * row, row);   /* in order: write-combined */
    s_tex[id].used = 1;
    s_tex[id].rect = 1;
    s_tex[id].pitch = (uint16_t)pitch;
    s_tex[id].w = (uint16_t)w;
    s_tex[id].h = (uint16_t)h;
    s_tex[id].levels = 1;
    s_tex[id].nvfmt = fmt == XGX_TEX_AY8    ? NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_AY8
                      : fmt == XGX_TEX_A8Y8 ? NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8Y8
                                            : NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8;
    s_tex[id].bytes = bytes;
    s_tex[id].mem = base;
    s_tex[id].base = base;
    {
        static uint32_t s_rgen = 0x80000000u;   /* apart from xgx_tex_create's */
        s_tex[id].gen = ++s_rgen;
    }
    s_tex[id].pal = 0;
    return (uint32_t)id;
}

uint32_t xgx_tex_create(uint32_t w, uint32_t h, uint32_t levels, uint32_t fmt, const void* data) {
    int id, pw, ph, l, lw, lh, sw, sh;
    uint32_t bytes = 0, pal_bytes = fmt == XGX_TEX_P8 ? XGX_TEX_PALETTE_BYTES : 0;
    uint8_t *mem, *base;
    const uint8_t* src = (const uint8_t*)data;
    uint8_t* dst;
    if (!w || !h || w > 1024 || h > 1024 || !levels) return 0;
    pw = pot((int)w);
    ph = pot((int)h);
    if (fmt == XGX_TEX_P8 && !data) return 0;
    if ((pw != (int)w || ph != (int)h) && data &&
        (fmt == XGX_TEX_ARGB8 || fmt == XGX_TEX_AY8 || fmt == XGX_TEX_A8Y8))
        return tex_create_rect(w, h, fmt, (const uint8_t*)data);
    if (pw != (int)w || ph != (int)h) {
        /* resampled per 8-bit channel: palette indices and DXT1 blocks can't be */
        if (fmt != XGX_TEX_ARGB8 && fmt != XGX_TEX_AY8 && fmt != XGX_TEX_A8Y8) return 0;
        levels = 1;
    }
    if (!data) levels = 1;
    for (l = 0, lw = pw, lh = ph; l < (int)levels; l++) {
        bytes += level_size(fmt, lw, lh);
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }
    id = alloc_handle();
    if (!id) return 0;
    bytes += pal_bytes;   /* the palette first: its offset wants 64-byte alignment, the pool gives 128 */
    s_st_tex_kb += bytes / 1024;
    base = (uint8_t*)tex_pool_alloc(bytes);
    if (!base) return 0;
    mem = base + pal_bytes;
    dst = mem;
    sw = (int)w;
    sh = (int)h;
    lw = pw;
    lh = ph;
    for (l = 0; data && l < (int)levels; l++) {   /* no data: the GPU fills it (EFB copy) */
        if (fmt == XGX_TEX_DXT1 || fmt == XGX_TEX_DXT3) memcpy(dst, src, level_size(fmt, lw, lh));   /* block-linear, not swizzled */
        else write_level(dst, src, sw, sh, lw, lh, fmt_bpp(fmt));
        src += level_size(fmt, sw, sh);
        dst += level_size(fmt, lw, lh);
        sw = sw > 1 ? sw / 2 : 1;
        sh = sh > 1 ? sh / 2 : 1;
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }
    if (pal_bytes) memcpy(base, src, pal_bytes);   /* after the levels in `data` */
    s_tex[id].used = 1;
    s_tex[id].rect = 0;
    s_tex[id].pitch = 0;
    s_tex[id].w = (uint16_t)pw;
    s_tex[id].h = (uint16_t)ph;
    s_tex[id].levels = (uint8_t)levels;
    s_tex[id].nvfmt = nv_format(fmt);
    s_tex[id].bytes = bytes;
    s_tex[id].mem = mem;
    s_tex[id].base = base;
    {
        static uint32_t s_gen;
        s_tex[id].gen = ++s_gen;
    }
    s_tex[id].pal = pal_bytes ? ((uint32_t)base & 0x03FFFFC0) | NV097_SET_TEXTURE_PALETTE_LENGTH_256 << 2 : 0;
    return (uint32_t)id;
}

/* The Trophy Collection draws every trophy at once: more textures in one
 * frame than the pool holds, so each frame evicted and re-uploaded most of
 * them (1.4 fps on the console, 650 ms a frame converting textures). When
 * only textures of the current frame are left to evict, gx_tex.c asks for
 * an overflow pool from the RAM that is free right then, keeping
 * TEX_OVERFLOW_RESERVE for the game's own demand-committed memory; it is
 * given back at the next scene change (xgx_tex_pool_shrink). */
#define TEX_OVERFLOW_MAX (8u * 1024 * 1024)
#define TEX_OVERFLOW_MIN (2u * 1024 * 1024)
#define TEX_OVERFLOW_RESERVE (6u * 1024 * 1024)
int xgx_tex_pool_grow(void) {
    uint32_t free_bytes = xhw_mem_free_kb() * 1024u, bytes;
    if (s_tp2.base || free_bytes < TEX_OVERFLOW_RESERVE + TEX_OVERFLOW_MIN) return 0;
    bytes = (free_bytes - TEX_OVERFLOW_RESERVE) & ~(1024u * 1024 - 1);
    if (bytes > TEX_OVERFLOW_MAX) bytes = TEX_OVERFLOW_MAX;
    while (bytes >= TEX_OVERFLOW_MIN && !pool_init(&s_tp2, bytes)) bytes -= 1024u * 1024;
    xhw_logf("[TEX] overflow pool %u KB (free %u KB)", s_tp2.base ? bytes / 1024 : 0, free_bytes / 1024);
    return s_tp2.base != NULL;
}

int xgx_tex_in_overflow(uint32_t tex) {
    return tex && tex < MAX_TEX && s_tex[tex].used && pool_owns(&s_tp2, s_tex[tex].base);
}

static void ind_tex_flush(void);

void xgx_tex_pool_shrink(void) {
    if (!s_tp2.base) return;
    ind_tex_flush();   /* rebuilt on demand; one may sit in the overflow pool */
    wait_idle();   /* the last frame may still sample them */
    release_deferred();
    if (s_tp2.used) return;   /* something still lives there: next time */
    pool_release(&s_tp2);
    xhw_logf("[TEX] overflow pool released (free %u KB)", xhw_mem_free_kb());
}

uint32_t xgx_tex_bytes(uint32_t tex) {
    return tex && tex < MAX_TEX && s_tex[tex].used ? s_tex[tex].bytes : 0;
}

void xgx_tex_destroy(uint32_t tex) {
    if (!tex || tex >= MAX_TEX || !s_tex[tex].used) return;
    defer_free(s_tex[tex].base);
    s_tex[tex].used = 0;
    s_tex[tex].mem = s_tex[tex].base = NULL;
}

/* ======================================================================
 * Frame
 * ====================================================================== */
static int s_frame_open;
static uint32_t s_frame;
static uint32_t s_draws, s_approx, s_pf_verts;
static uint32_t s_st_swz, s_st_swz_stages;   /* draws whose combiners extract swizzled channels */
static uint32_t s_st_ind, s_st_ind_approx;   /* draws with indirect stages: offset / drawn direct */
static volatile int s_fbdump_once;

unsigned xgx_present_count(void) { return s_frame; }
static void frame_open(void);
static void vp_frame_end(void);

/* GXCopyDisp's clear comes right after the present: it waits (in order)
 * until the next frame opens, so it doesn't open the frame early */
#define PENDING_CLEARS 4
static struct {
    int x, y, w, h, color, depth;
    uint32_t argb, z24;
} s_pending_clear[PENDING_CLEARS];
static int s_npending_clear;

static void clear_fb(int x, int y, int w, int h, uint32_t argb, int color, int depth, uint32_t z24) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_fbw) w = s_fbw - x;
    if (y + h > s_fbh) h = s_fbh - y;
    if (w <= 0 || h <= 0) return;
    pb_close();
    /* A8R8G8B8: pb_fill converts to the surface's format itself (converted
     * here as well, 16-bit clears came out near black) */
    if (color) pb_fill(x, y, w, h, argb);
    if (depth) {
        uint32_t* p = pb_begin();
        uint32_t zv = pb_DepthFmt == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ? (z24 >> 8) : (z24 << 8);
        p = pb_push1(p, NV097_SET_CLEAR_RECT_HORIZONTAL, (uint32_t)((x + w - 1) << 16) | (uint32_t)x);
        p = pb_push1(p, NV097_SET_CLEAR_RECT_VERTICAL, (uint32_t)((y + h - 1) << 16) | (uint32_t)y);
        p = pb_push1(p, NV097_SET_ZSTENCIL_CLEAR_VALUE, zv);
        p = pb_push1(p, NV097_CLEAR_SURFACE, NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL);
        pb_end(p);
    }
    /* The next draws wait for the clear. HSD's shadow maps are drawn one
     * after another in one 256x256 corner: copy, clear, then the next
     * map's white background quad. On the console the clear still ran
     * while that quad's first triangle was drawn, and black cut into it
     * (the copies had black wedges above the quad's diagonal); the stage
     * surfaces that multiply the map in flashed black near the fighters.
     * xemu runs it in order. */
    {
        uint32_t* p = pb_begin();
        p = pb_push1(p, NV097_WAIT_FOR_IDLE, 0);
        pb_end(p);
    }
}

void xgx_clear(const int32_t r[4], const uint8_t rgba[4], uint32_t z24, int color, int alpha, int depth) {
    int x0, y0, x1, y1;
    uint32_t argb = (uint32_t)rgba[3] << 24 | (uint32_t)rgba[0] << 16 | (uint32_t)rgba[1] << 8 | rgba[2];
    (void)alpha;
    x0 = map_x((float)r[0]);
    y0 = map_y((float)r[1]);
    x1 = map_x((float)(r[0] + r[2]));
    y1 = map_y((float)(r[1] + r[3]));
    if (XGX_OVERLAP && !s_frame_open && s_npending_clear < PENDING_CLEARS) {
        s_pending_clear[s_npending_clear].x = x0;
        s_pending_clear[s_npending_clear].y = y0;
        s_pending_clear[s_npending_clear].w = x1 - x0;
        s_pending_clear[s_npending_clear].h = y1 - y0;
        s_pending_clear[s_npending_clear].argb = argb;
        s_pending_clear[s_npending_clear].color = color;
        s_pending_clear[s_npending_clear].depth = depth;
        s_pending_clear[s_npending_clear].z24 = z24;
        s_npending_clear++;
        return;
    }
    frame_open();
    clear_fb(x0, y0, x1 - x0, y1 - y0, argb, color, depth, z24);
}

static void state_reset_shadows(void);

/* pbkit's pb_target_back_buffer (set_draw_buffer) writes CONTROL0 =
 * 0x00110001, "We use W": Z_PERSPECTIVE_ENABLE, a w-buffer. The projection
 * here is built for a z-buffer (docs/renderer.md "Depth"); with w the depth
 * came from the interpolated eye distance instead, and Pokémon Stadium's
 * floor and the dark layer just under it took turns in black bands across
 * the arena. xemu (the build in use) ignores the bit. Set back after every
 * retarget. */
#define CONTROL0 NV097_SET_CONTROL0_TEXTURE_PERSPECTIVE_ENABLE

/* The first GPU use of a frame. With XGX_OVERLAP the previous frame's wait
 * for the GPU is here rather than in xgx_present: the GPU finishes drawing
 * it (and the queued flip) while the CPU runs the next simulation ticks.
 * Everything after this point may assume, as before, that the GPU is idle
 * at the start of the frame (the vertex ring and the pushbuffer restart,
 * deferred frees are released, xgx_vbuf_free_now). */
static void frame_open(void) {
    int i;
    if (s_frame_open) return;
    if (XGX_OVERLAP) {
        wait_idle();
        release_deferred();
    }
    pb_reset();
    s_pb_base = pb_begin();
    pb_target_back_buffer();
    {
        uint32_t* p = pb_begin();
        p = pb_push1(p, NV097_SET_CONTROL0, CONTROL0);
        pb_end(p);
    }
    s_ring_pos = 0;
    s_frame_open = 1;
    /* the bars outside the content rect, and a defined EFB. Not when the
     * first pending clear (GXCopyDisp's) writes colour and depth over the
     * whole framebuffer anyway, as it does without a pillarbox: there are
     * no bars, and the fill and the wait for it were spent for nothing. */
    if (!s_npending_clear || !s_pending_clear[0].color || !s_pending_clear[0].depth || s_pending_clear[0].x > 0 ||
        s_pending_clear[0].y > 0 || s_pending_clear[0].x + s_pending_clear[0].w < s_fbw ||
        s_pending_clear[0].y + s_pending_clear[0].h < s_fbh)
        clear_fb(0, 0, s_fbw, s_fbh, 0xFF000000u, 1, 1, 0xFFFFFF);
    for (i = 0; i < s_npending_clear; i++)
        clear_fb(s_pending_clear[i].x, s_pending_clear[i].y, s_pending_clear[i].w, s_pending_clear[i].h,
                 s_pending_clear[i].argb, s_pending_clear[i].color, s_pending_clear[i].depth, s_pending_clear[i].z24);
    s_npending_clear = 0;
}

/* restart at the pushbuffer head when a frame gets close to its end:
 * pbkit's pushbuffer has no overflow check (OpenCrossing traps.md) */
static void pb_budget(void) {
    uint32_t used = pb_used();
    if (used > s_st_pb_peak) s_st_pb_peak = used;
    if (used < PB_GUARD) return;
    s_st_pb_resets++;
    wait_idle();
    pb_reset();
    s_pb_base = pb_begin();
    s_ring_pos = 0;
}

#ifndef XGX_STATS_EVERY
#define XGX_STATS_EVERY 600   /* [NV2A] frame line every N presents */
#endif
#ifndef XHW_FBDUMP_EVERY
#define XHW_FBDUMP_EVERY 0   /* [FBDUMP] screenshot every N presents (xhw_fbdump.c) */
#endif
void xgx_fbdump_next(void) { s_fbdump_once = 1; }
/* BACK asks for a screenshot (s_shot_req, from the pad code at any point of
 * a frame); the next frame is the one taken (s_shot_once from its start), so
 * a -DXGX_DEBUG_TRACE trace and the EFB-copy dumps cover all of it. */
static volatile int s_shot_req;
static int s_shot_once;
void xgx_shot_next(void) { s_shot_req = 1; }

/* On-screen frame rate (settings.ini [video] fps): frames presented over
 * the last half second, drawn by the GPU after the frame (colour fills of
 * the lit runs of each font row), before the screenshot dumps (shots show
 * it). Yellow 5x7 digits at 2x on a black box, inside the TV-safe area. */
static int s_fps_on;
static uint32_t s_fps_val, s_fps_frames;
static uint64_t s_fps_t0;
void xgx_set_fps_overlay(int on) { s_fps_on = on; }

static void fps_overlay(void) {
    static const uint8_t font[10][7] = {
        { 14, 17, 19, 21, 25, 17, 14 }, { 4, 12, 4, 4, 4, 4, 14 },   { 14, 17, 1, 2, 4, 8, 31 },
        { 31, 2, 4, 2, 1, 17, 14 },     { 2, 6, 10, 18, 31, 2, 2 },  { 31, 16, 30, 1, 1, 17, 14 },
        { 6, 8, 16, 30, 17, 17, 14 },   { 31, 1, 2, 4, 8, 8, 8 },    { 14, 17, 17, 14, 17, 17, 14 },
        { 14, 17, 17, 15, 1, 2, 12 },
    };
    uint64_t now = xhw_time_ns();
    uint32_t v, digits[3], nd = 0, d, cy;
    int x0 = s_fbw / 16, y0 = s_fbh / 16, w, h;
    s_fps_frames++;
    if (!s_fps_t0 || now - s_fps_t0 > 2000000000ull) {
        s_fps_t0 = now;
        s_fps_frames = 0;
    } else if (now - s_fps_t0 >= 500000000ull) {
        s_fps_val = (uint32_t)((s_fps_frames * 1000000000ull + (now - s_fps_t0) / 2) / (now - s_fps_t0));
        s_fps_t0 = now;
        s_fps_frames = 0;
    }
    v = s_fps_val > 999 ? 999 : s_fps_val;
    do {
        digits[nd++] = v % 10;
        v /= 10;
    } while (v && nd < 3);
    w = (int)nd * 12 + 4;
    h = 18;
    pb_close();
    pb_fill(x0, y0, w, h, 0xFF000000u);   /* pb_fill converts to the surface's format */
    for (d = 0; d < nd; d++)
        for (cy = 0; cy < 7; cy++) {
            uint32_t bits = font[digits[nd - 1 - d]][cy], cx = 0;
            while (cx < 5) {   /* runs of lit cells, 2x2 pixels each */
                uint32_t run = 0;
                while (cx + run < 5 && (bits >> (4 - (cx + run)) & 1)) run++;
                if (run) pb_fill(x0 + 2 + (int)(d * 12 + cx * 2), y0 + 2 + (int)cy * 2, (int)run * 2, 2, 0xFFFFFF00u);
                cx += run ? run : 1;
            }
        }
}

/* The settings menu (xgx.h): the CPU writes it into the finished frame, so
 * a frame that shows it waits for the GPU first. Only the title screen
 * has it. */
static xgx_overlay s_ovl;
static int s_ovl_ttl;   /* presents left without a refresh */

void xgx_set_overlay(const xgx_overlay* o) {
    int r;
    if (!o || o->rows <= 0) {
        s_ovl_ttl = 0;
        return;
    }
    s_ovl = *o;
    if (s_ovl.rows > XGX_OVERLAY_ROWS) s_ovl.rows = XGX_OVERLAY_ROWS;
    if (s_ovl.cols < 0 || s_ovl.cols > XGX_OVERLAY_COLS) s_ovl.cols = XGX_OVERLAY_COLS;
    for (r = 0; r < s_ovl.rows; r++) s_ovl.text[r][XGX_OVERLAY_COLS - 1] = '\0';
    s_ovl_ttl = 2;   /* lingers at most one present after the last refresh */
}

void xgx_present(int black) {
    int ovl;
    frame_open();
    if (black) clear_fb(0, 0, s_fbw, s_fbh, 0xFF000000u, 1, 0, 0);
    pb_budget();   /* the frame's pushbuffer peak */
    if (s_fps_on && !black) fps_overlay();
    ovl = s_ovl_ttl > 0 && !black;
    /* the next frame_open waits (XGX_OVERLAP); a screenshot reads the frame now */
    if (!XGX_OVERLAP || ovl || s_fbdump_once || s_shot_once ||
        (XHW_FBDUMP_EVERY && (s_frame + 1) % XHW_FBDUMP_EVERY == 0))
        wait_idle();
    if (ovl) xhw_overlay_draw(pb_back_buffer(), s_fbw, s_fbh, s_bpp, (int)pb_back_buffer_pitch(), &s_ovl);
    if (s_ovl_ttl > 0) s_ovl_ttl--;
    if (s_fbdump_once || (XHW_FBDUMP_EVERY && (s_frame + 1) % XHW_FBDUMP_EVERY == 0)) {
        s_fbdump_once = 0;
        xhw_fbdump(pb_back_buffer(), s_fbw, s_fbh, s_bpp, (int)pb_back_buffer_pitch());
    }
    if (s_shot_once) {
        s_shot_once = 0;
        xhw_fbdump_file(pb_back_buffer(), s_fbw, s_fbh, s_bpp, (int)pb_back_buffer_pitch());
    }
    if (s_shot_req) {
        s_shot_req = 0;
        s_shot_once = 1;
    }
    if (!XGX_OVERLAP) release_deferred();
    log_first_fault();
    if (s_gf_count != s_gf_logged) {
        xhw_logf("[NV2A] GPU fault x%u: kind %u %08x %08x %08x %08x%s", (unsigned)s_gf_count, (unsigned)s_gf_last[0],
                 (unsigned)s_gf_last[1], (unsigned)s_gf_last[2], (unsigned)s_gf_last[3], (unsigned)s_gf_last[4],
                 ocx_pb_irq_off ? " (interrupt masked)" : "");
        s_gf_logged = s_gf_count;
    }
    s_gf_storms = 0;
    irq_recover();
    {
        /* never draw into the buffer being scanned out (OpenCrossing traps.md).
         * Both waits depend on pbkit's vblank DPC; if the GPU interrupt is
         * masked (an interrupt storm, ocx_pb_irq_off) they would never end
         * and nothing would say why, so they are timed and logged. */
        int guard = 4, pf = xhw_perf_enter(XHW_PERF_GPU), warned = 0;
        uint64_t t0 = 0;
        /* pb_finished pushes the flip at pbkit's pb_Put, where the last kick
         * ended: with this frame's tail still open (P past pb_Put, not
         * kicked) the flip overwrote its first words, and the kick that
         * followed sent the GPU into the middle of the tail (the v1 release
         * hang on the intro movie, GitHub #5/#6: the frame-rate counter,
         * on in test builds, closed it first in fps_overlay). */
        pb_close();
        while (pb_finished()) {
            irq_recover();
            if (!t0) t0 = xhw_time_ns();
            else if (!warned && xhw_time_ns() - t0 > 1000000000ull) {
                xhw_logf("[NV2A] flip stalled: no back buffer free for 1 s, vblank %u, faults %u%s",
                         (unsigned)pb_get_vbl_counter(), (unsigned)s_gf_count,
                         ocx_pb_irq_off ? " (GPU interrupt masked)" : "");
                warned = 1;
            }
        }
        while (guard-- && (PCRTC_START_REG & 0x03FFFFFF) == ((uint32_t)pb_back_buffer() & 0x03FFFFFF)) {
            DWORD vbl = pb_get_vbl_counter();
            int ms;
            for (ms = 0; ms < 50 && pb_get_vbl_counter() == vbl; ms++) xhw_sleep_ms(1);
        }
        xhw_perf_leave(pf);
    }
    xhw_perf_frame(s_draws, s_pf_verts);
    s_pf_verts = 0;
    vp_frame_end();
    s_frame++;
    if (s_frame % XGX_STATS_EVERY == 0) {
        /* draws/approximated: the last frame; the rest summed over the interval */
        xhw_logf("[NV2A] frame %u: %u draws (%u approximated), tex pool %u KB free (largest %u KB) | per %u: %u idle "
                 "waits, %u EFB copies, %u KB textures, %u pool allocations failed, %u verts, pushbuffer peak %u of %u KB "
                 "(%u restarts)",
                 s_frame, s_draws, s_approx, xgx_tex_pool_free_kb(), xgx_tex_pool_largest_kb(), XGX_STATS_EVERY,
                 s_st_waits, s_st_efb, s_st_tex_kb, s_st_tex_fail, s_st_verts, s_st_pb_peak / 1024, PB_BYTES / 1024,
                 s_st_pb_resets);
        xhw_logf("[NV2A] per %u draws by primitive: quads %u, triangles %u, strips %u, fans %u, lines %u, line strips "
                 "%u, points %u", XGX_STATS_EVERY, s_st_prim[0], s_st_prim[2], s_st_prim[3], s_st_prim[4], s_st_prim[5],
                 s_st_prim[6], s_st_prim[7]);
        memset(s_st_prim, 0, sizeof s_st_prim);
        if (s_st_swz || s_st_ind || s_st_ind_approx)
            xhw_logf("[NV2A] per %u frames: %u draws through swap tables (%u combiner stages added), %u indirect "
                     "(%u more drawn direct)", XGX_STATS_EVERY, s_st_swz, s_st_swz_stages, s_st_ind, s_st_ind_approx);
        s_st_swz = s_st_swz_stages = s_st_ind = s_st_ind_approx = 0;
        xhw_logf("[NV2A] per %u frames: %u vertex programs loaded (%u instructions), %u program switches",
                 XGX_STATS_EVERY, s_st_vp_loads, s_st_vp_ins, s_st_vp_sel);
        s_st_vp_loads = s_st_vp_ins = s_st_vp_sel = 0;
        xhw_logf("[NV2A] per %u draws: %u changed nothing, %u only a position matrix | proj %u view %u posmtx %u texmtx "
                 "%u lights %u chans %u texgen %u tev %u tevreg %u pixel %u fog %u maps %u scissor %u",
                 s_st_draws, s_st_dirty_none, s_st_dirty_mtx, s_st_dirty[0], s_st_dirty[1], s_st_dirty[2], s_st_dirty[3],
                 s_st_dirty[4], s_st_dirty[5], s_st_dirty[6], s_st_dirty[7], s_st_dirty[8], s_st_dirty[9],
                 s_st_dirty[10], s_st_dirty[11], s_st_dirty[12]);
        memset(s_st_dirty, 0, sizeof s_st_dirty);
        s_st_draws = s_st_dirty_none = s_st_dirty_mtx = 0;
        s_st_waits = s_st_efb = s_st_tex_kb = s_st_verts = s_st_tex_fail = s_st_pb_peak = s_st_pb_resets = 0;
    }
    s_draws = s_approx = 0;
    s_frame_open = 0;
    if (!XGX_OVERLAP) frame_open();
}

/* ======================================================================
 * Vertex programs: cache + resident program memory
 *
 * Generated programs are cached by key (VP_CACHE entries); which of them are
 * in the NV2A's 136 instructions of program memory, and where a program is
 * loaded when it isn't, is nv2a_vpmem.c's decision (Belady's rule with the
 * previous frame as the forecast). A load is a few pushbuffer bursts of 8
 * instructions; the GPU runs them in order with the draws, so any program
 * may be overwritten, including the one the last draw used.
 * ====================================================================== */
typedef struct {
    VpKey key;
    uint32_t hash;     /* vp_hash(&key): compared before the key */
    VpProgram prog;
    uint32_t used;     /* s_vp_now at its last select */
    int valid;
} VpEntry;

static uint32_t vp_hash(const VpKey* k) {
    const uint8_t* p = (const uint8_t*)k;
    uint32_t h = 2166136261u, i;
    for (i = 0; i + 4 <= sizeof *k; i += 4) {
        uint32_t w;
        memcpy(&w, p + i, 4);
        h = (h ^ w) * 16777619u;
    }
    for (; i < sizeof *k; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

#define VP_CACHE VPM_PROGS
static VpEntry* s_vp;
/* hash, valid flag and last use of each entry side by side: the lookup and
 * the LRU scan read these instead of a word per 2 KB entry (a cache miss
 * each, ~80 program switches a frame) */
static uint32_t s_vp_hash[VP_CACHE], s_vp_used[VP_CACHE];
static uint8_t s_vp_valid[VP_CACHE];
static int s_vp_cur = -1;
static uint32_t s_vp_now;
static VpMem s_vpm;

#ifdef XGX_DEBUG_VPTRACE
/* -DXGX_DEBUG_VPTRACE[=n]: the program selects of two consecutive frames
 * every n (default 600) as [VPT] lines, for tools/xbox/vp_policy.py:
 *   [VPT] frame F: S selects, L loads (I instructions), K keys
 *   [VPT] k ID HASH N KEYHEX     each program the frame selected (N instructions)
 *   [VPT] s ID[L] ...            the selects in order, L where it was loaded
 *   [VPT] end */
#if XGX_DEBUG_VPTRACE > 1
#define VPT_EVERY XGX_DEBUG_VPTRACE
#else
#define VPT_EVERY 600
#endif
#define VPT_SEL 2048
#define VPT_KEYS 128
static uint16_t s_vpt_sel[VPT_SEL];   /* key id | loaded << 15 */
static VpKey s_vpt_key[VPT_KEYS];
static uint32_t s_vpt_hash[VPT_KEYS], s_vpt_len[VPT_KEYS], s_vpt_loads, s_vpt_ins;
static int s_vpt_n, s_vpt_nkeys;

static int vpt_on(void) { return s_frame >= VPT_EVERY && s_frame % VPT_EVERY < 2; }

static void vpt_record(const VpEntry* e, int loaded) {
    int id;
    if (!vpt_on()) return;
    for (id = 0; id < s_vpt_nkeys; id++)
        if (s_vpt_hash[id] == e->hash && !memcmp(&s_vpt_key[id], &e->key, sizeof e->key)) break;
    if (id == s_vpt_nkeys && id < VPT_KEYS) {
        s_vpt_key[id] = e->key;
        s_vpt_hash[id] = e->hash;
        s_vpt_len[id] = e->prog.n;
        s_vpt_nkeys++;
    }
    if (id >= VPT_KEYS || s_vpt_n >= VPT_SEL) return;
    s_vpt_sel[s_vpt_n++] = (uint16_t)(id | loaded << 15);
    if (loaded) s_vpt_loads++, s_vpt_ins += e->prog.n;
}

static void vpt_dump(void) {
    char line[900];
    int i, j, at;
    if (!vpt_on()) return;
    xhw_logf("[VPT] frame %u: %d selects, %u loads (%u instructions), %d keys", s_frame, s_vpt_n, s_vpt_loads,
             s_vpt_ins, s_vpt_nkeys);
    for (i = 0; i < s_vpt_nkeys; i++) {
        at = 0;
        for (j = 0; j < (int)sizeof(VpKey); j++) at += snprintf(line + at, 3, "%02x", ((const uint8_t*)&s_vpt_key[i])[j]);
        xhw_logf("[VPT] k %d %08x %u %s", i, s_vpt_hash[i], s_vpt_len[i], line);
    }
    for (i = 0; i < s_vpt_n; i += 64) {
        at = 0;
        for (j = i; j < s_vpt_n && j < i + 64; j++)
            at += snprintf(line + at, sizeof line - (size_t)at, " %u%s", s_vpt_sel[j] & 0x7FFFu,
                           s_vpt_sel[j] >> 15 ? "L" : "");
        xhw_logf("[VPT] s%s", line);
    }
    xhw_log("[VPT] end");
    s_vpt_n = s_vpt_nkeys = 0;
    s_vpt_loads = s_vpt_ins = 0;
}
#endif

static void vp_upload(const VpEntry* e, int start) {
    uint32_t i;
    put1(NV097_SET_TRANSFORM_PROGRAM_LOAD, (uint32_t)start);
    for (i = 0; i < e->prog.n; i += 8) {
        uint32_t n = e->prog.n - i < 8 ? e->prog.n - i : 8;
        pb_push(P++, NV097_SET_TRANSFORM_PROGRAM, n * 4);
        memcpy(P, &e->prog.words[i * 4], n * 16);
        P += n * 4;
    }
    s_st_vp_loads++;
    s_st_vp_ins += e->prog.n;
}

static void vp_select(const VpKey* k) {
    int i, pick = -1, start, loaded;
    uint32_t h = vp_hash(k);
    VpEntry* e;
    if (s_vp_cur >= 0 && s_vp[s_vp_cur].hash == h && memcmp(&s_vp[s_vp_cur].key, k, sizeof *k) == 0) return;
    s_vp_now++;
    s_st_vp_sel++;
    for (i = 0; i < VP_CACHE; i++)
        if (s_vp_hash[i] == h && s_vp_valid[i] && memcmp(&s_vp[i].key, k, sizeof *k) == 0) { pick = i; break; }
    if (pick < 0) {
        /* a free entry, or the least recently used (its memory is freed) */
        for (i = 0; i < VP_CACHE; i++)
            if (!s_vp_valid[i] || pick < 0 || s_vp_used[i] < s_vp_used[pick]) {
                pick = i;
                if (!s_vp_valid[i]) break;
            }
        e = &s_vp[pick];
        if (e->valid) vpm_drop(&s_vpm, pick);
        e->key = *k;
        e->hash = s_vp_hash[pick] = h;
        e->valid = s_vp_valid[pick] = 1;
        vp_generate(k, &e->prog);
    }
    e = &s_vp[pick];
    loaded = vpm_select(&s_vpm, pick, (int)e->prog.n, &start);
    if (loaded) vp_upload(e, start);
#ifdef XGX_DEBUG_VPTRACE
    vpt_record(e, loaded);
#endif
    e->used = s_vp_used[pick] = s_vp_now;
    put1(NV097_SET_TRANSFORM_PROGRAM_START, (uint32_t)start);
    s_vp_cur = pick;
    if (e->prog.approximated) s_approx++;
}

static void vp_frame_end(void) {
#ifdef XGX_DEBUG_VPTRACE
    vpt_dump();
#endif
    vpm_frame(&s_vpm);
}

/* ======================================================================
 * Vertex-program constants
 * ====================================================================== */
static float s_vc[VPC_COUNT][4];
static float s_vc_shadow[VPC_COUNT][4];
static int s_vc_valid;
/* rows written since the last emit_vc: only these are compared and sent */
static uint32_t s_vc_dirty[(VPC_COUNT + 31) / 32];

static void vc_mark(int r, int n) {
    for (; n > 0; n--, r++) s_vc_dirty[r >> 5] |= 1u << (r & 31);
}

static void set_row(int r, float x, float y, float z, float w) {
    vc_mark(r, 1);
    s_vc[r][0] = x;
    s_vc[r][1] = y;
    s_vc[r][2] = z;
    s_vc[r][3] = w;
}

/* GX fog (nv2a_fog.h): what the fog rows, the vertex program's key and the
 * final combiner were built from. The rows depend on the projection too. */
static FogSetup s_fog;

static void build_fog(const XgxState* st) {
    fog_setup(st->fog_type, st->fog_start, st->fog_end, st->fog_near, st->fog_far, s_vc[VPC_PROJ + 2],
              s_vc[VPC_PROJ + 3], s_zmax, &s_fog);
    if (!s_fog.kind) return;
    set_row(VPC_FOG, s_fog.num[0], s_fog.num[1], s_fog.num[2], s_fog.num[3]);
    set_row(VPC_FOG + 1, s_fog.den[0], s_fog.den[1], s_fog.den[2], s_fog.den[3]);
    set_row(VPC_FOG + 2, s_fog.curve[0], s_fog.curve[1], s_fog.curve[2], s_fog.curve[3]);
}

static void build_proj(const XgxState* st) {
    const float(*p)[4] = st->proj;
    float vx = (float)map_x(st->viewport[0]) , vy = (float)map_y(st->viewport[1]);
    float vw = st->viewport[2] * (float)s_cw / XGX_EFB_W, vh = st->viewport[3] * (float)s_ch / XGX_EFB_H;
    float vn = st->viewport[4], vf = st->viewport[5];
    float sx = vw * 0.5f, ox = vx + vw * 0.5f, sy = -vh * 0.5f, oy = vy + vh * 0.5f;
    int c;
    /* GX clip z/w runs -1 (near) .. 0 (far); depth = z/w * (far - near) + far */
    vc_mark(VPC_PROJ, 4);
    for (c = 0; c < 4; c++) {
        s_vc[VPC_PROJ][c] = sx * p[0][c] + ox * p[3][c];
        s_vc[VPC_PROJ + 1][c] = sy * p[1][c] + oy * p[3][c];
        s_vc[VPC_PROJ + 2][c] = s_zmax * ((vf - vn) * p[2][c] + vf * p[3][c]);
        s_vc[VPC_PROJ + 3][c] = p[3][c];
    }
    if (st->fog_type & 7) build_fog(st);
}

/* only the matrices the front end loaded since the last draw (posmtx_mask) */
static void build_mtx(XgxState* st) {
    int k, r;
    for (k = 0; k < XGX_NUM_POSMTX; k++) {
        if (!(st->posmtx_mask & (1u << k))) continue;
        vc_mark(VPC_POS + k * 3, 3);
        for (r = 0; r < 3; r++) {
            memcpy(s_vc[VPC_POS + k * 3 + r], st->posmtx[k][r], 16);
            set_row(VPC_NRM + k * 3 + r, st->nrmmtx[k][r][0], st->nrmmtx[k][r][1], st->nrmmtx[k][r][2], 0);
        }
    }
    st->posmtx_mask = 0;
}

/* colours as 0..1: a multiply, not a divide per component (the result may
 * differ from x / 255 in the last bit, far below what the GPU resolves) */
#define INV255 (1.0f / 255.0f)

static void build_chans(const XgxState* st) {
    int c, k;
    vc_mark(VPC_CHAN, 4);
    for (c = 0; c < 2; c++)
        for (k = 0; k < 4; k++) {
            s_vc[VPC_CHAN + c * 2][k] = st->mat[c][k] * INV255;
            s_vc[VPC_CHAN + c * 2 + 1][k] = st->amb[c][k] * INV255;
        }
}

/* Only the lights an enabled channel uses: the program reads no others, and
 * a channel change (XGX_DIRTY_CHANS) rebuilds them, so a light that comes
 * into use later is written then. ~20% of a match's draws get here. */
static void build_lights(const XgxState* st, uint32_t spec_lights) {
    uint32_t used = 0;
    int i;
    for (i = 0; i < 4; i++)
        if (st->chan[i].enable) used |= st->chan[i].light_mask;
    for (i = 0; i < XGX_MAX_LIGHTS; i++) {
        const XgxLight* l = &st->light[i];
        int b = VPC_LIGHT + i * 5;
        if (!(used & (1u << i))) continue;
        if (spec_lights & (1u << i)) {
            float n = sqrtf(l->pos[0] * l->pos[0] + l->pos[1] * l->pos[1] + l->pos[2] * l->pos[2]);
            n = n > 1e-12f ? 1.0f / n : 0.0f;
            set_row(b, l->pos[0] * n, l->pos[1] * n, l->pos[2] * n, 1);
        } else {
            set_row(b, l->pos[0], l->pos[1], l->pos[2], 1);
        }
        set_row(b + 1, l->dir[0], l->dir[1], l->dir[2], 0);
        set_row(b + 2, l->color[0] * INV255, l->color[1] * INV255, l->color[2] * INV255, l->color[3] * INV255);
        set_row(b + 3, l->a[0], l->a[1], l->a[2], 0);
        set_row(b + 4, l->k[0], l->k[1], l->k[2], 0);
    }
}

static const float* texgen_src_mtx(const XgxState* st, uint32_t id, float out[3][4]) {
    if (id >= GX_TEXMTX0 && id < GX_IDENTITY) memcpy(out, st->texmtx[(id - GX_TEXMTX0) / 3], 48);
    else if (id < GX_TEXMTX0) memcpy(out, st->posmtx[id / 3], 48);
    else {
        memset(out, 0, 48);
        out[0][0] = out[1][1] = out[2][2] = 1;
    }
    return &out[0][0];
}

/* rows for unit u: texgen tg; the post matrix is folded in unless the
 * texgen normalizes first */
static int s_d_unit_map[4];   /* (defined with the other derived state below) */
enum { UNIT_PLAIN, UNIT_IND, UNIT_BUMP };
static int s_d_unit_kind[4];
static float s_d_unit_off[4][2];

static void build_texgen(const XgxState* st, int u, const XgxTexGen* tg) {
    float m[3][4], pt[3][4], out[3][4];
    int r, c;
    texgen_src_mtx(st, tg->mtx, m);
    if (tg->type == GX_TG_MTX2x4) {
        m[2][0] = m[2][1] = m[2][2] = 0;
        m[2][3] = 1;
    }
    if (tg->pt_mtx >= GX_PTTEXMTX0 && tg->pt_mtx < GX_PTIDENTITY) memcpy(pt, st->ptmtx[(tg->pt_mtx - GX_PTTEXMTX0) / 3], 48);
    else {
        memset(pt, 0, 48);
        pt[0][0] = pt[1][1] = pt[2][2] = 1;
    }
    {   /* a linear texture is sampled in texels: s and t times its size (tex_create_rect) */
        const XgxMap* mp = &st->map[s_d_unit_map[u]];
        const Tex* t = mp->tex && mp->tex < MAX_TEX && s_tex[mp->tex].used ? &s_tex[mp->tex] : NULL;
        if (t && t->rect)
            for (c = 0; c < 4; c++) {
                pt[0][c] *= (float)t->w;
                pt[1][c] *= (float)t->h;
            }
    }
    if (s_d_unit_kind[u] == UNIT_IND) {   /* GXSetIndTexCoordScale */
        for (c = 0; c < 4; c++) {
            pt[0][c] *= s_d_unit_off[u][0];
            pt[1][c] *= s_d_unit_off[u][1];
        }
    } else if (s_d_unit_kind[u] == UNIT_BUMP) {   /* the bias's constant offset: s + c q, t + c q */
        for (c = 0; c < 4; c++) {
            pt[0][c] += s_d_unit_off[u][0] * pt[2][c];
            pt[1][c] += s_d_unit_off[u][1] * pt[2][c];
        }
    }
    if (tg->normalize) {
        memcpy(out, m, 48);
        vc_mark(VPC_POSTMTX + u * 3, 3);
        for (r = 0; r < 3; r++) memcpy(s_vc[VPC_POSTMTX + u * 3 + r], pt[r], 16);
    } else {
        for (r = 0; r < 3; r++)
            for (c = 0; c < 4; c++)
                out[r][c] = pt[r][0] * m[0][c] + pt[r][1] * m[1][c] + pt[r][2] * m[2][c] + (c == 3 ? pt[r][3] : 0);
    }
    vc_mark(VPC_TEXGEN + u * 3, 3);
    for (r = 0; r < 3; r++) memcpy(s_vc[VPC_TEXGEN + u * 3 + r], out[r], 16);
}

static void push_vc_rows(int first, int n) {
    int i, words = n * 4;
    const uint32_t* w = (const uint32_t*)s_vc[first];
    put1(NV097_SET_TRANSFORM_CONSTANT_LOAD, (uint32_t)first);
    for (i = 0; i < words; i += 32) {
        int k = words - i < 32 ? words - i : 32;
        pb_push(P++, NV097_SET_TRANSFORM_CONSTANT, k);
        memcpy(P, w + i, (size_t)k * 4);
        P += k;
    }
}

/* Send the rows that changed. Only rows marked dirty are compared; runs a
 * few unchanged rows apart are merged to save method headers. */
static void emit_vc(void) {
    int w, run_start = -1, run_end = -1;
    if (!s_vc_valid) {
        push_vc_rows(0, VPC_COUNT);
        memcpy(s_vc_shadow, s_vc, sizeof s_vc);
        memset(s_vc_dirty, 0, sizeof s_vc_dirty);
        s_vc_valid = 1;
        return;
    }
    for (w = 0; w < (int)(sizeof s_vc_dirty / sizeof s_vc_dirty[0]); w++) {
        uint32_t bits = s_vc_dirty[w];
        s_vc_dirty[w] = 0;
        while (bits) {
            int r = w * 32 + __builtin_ctz(bits);
            bits &= bits - 1;
            if (r >= VPC_COUNT) continue;
            {   /* compared as words, inline: a 16-byte memcmp here was a call per row */
                uint32_t sh[4], v[4];
                memcpy(sh, s_vc_shadow[r], 16);
                memcpy(v, s_vc[r], 16);
                if (sh[0] == v[0] && sh[1] == v[1] && sh[2] == v[2] && sh[3] == v[3]) continue;
                memcpy(s_vc_shadow[r], v, 16);
            }
            if (run_start >= 0 && r - run_end <= 3) {
                run_end = r + 1;
            } else {
                if (run_start >= 0) push_vc_rows(run_start, run_end - run_start);
                run_start = r;
                run_end = r + 1;
            }
        }
    }
    if (run_start >= 0) push_vc_rows(run_start, run_end - run_start);
}

/* ======================================================================
 * Combiners
 * ====================================================================== */
typedef struct { uint32_t hash; RcCfg cfg; RcProg prog; } RcEntry;
#define RC_CACHE 256
static RcEntry* s_rc;
/* the entries' hashes side by side: a lookup scans these 1 KB instead of one
 * word per 624-byte entry (a cache miss each; ~0.7% of the console's CPU) */
static uint32_t s_rc_hash[RC_CACHE];
static int s_rc_count, s_rc_last = -1;
static uint32_t s_rc_gen;             /* bumped whenever a cache slot is (re)compiled */
static const RcProg* s_rc_sent;       /* the program on the GPU, valid while s_rc_sent_gen == s_rc_gen */
static uint32_t s_rc_sent_gen;
static int s_rc_valid;
static uint32_t s_rc_consts[RC_MAX_STAGES][2], s_rc_fconsts[2];
static uint32_t s_rc_cw0;             /* the final combiner's CW0 as sent, fog included */

/* the program's final combiner, blending toward the fog colour while fog is on */
static uint32_t final_cw0(const RcProg* rp) { return s_fog.kind ? fog_final_cw0(rp->cw0) : rp->cw0; }

/* FNV-1a over 32-bit words (the tail bytes one at a time) */
static uint32_t fnv(const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    uint32_t h = 2166136261u;
    for (; n >= 4; n -= 4, b += 4) {
        uint32_t w;
        memcpy(&w, b, 4);
        h = (h ^ w) * 16777619u;
    }
    while (n--) h = (h ^ *b++) * 16777619u;
    return h;
}

/* the bytes of a config that matter: the header and its nstages stages
 * (derive_units zeroes only those; the compiler reads no further) */
static size_t rc_used(const RcCfg* cfg) { return offsetof(RcCfg, st) + cfg->nstages * sizeof(RcStage); }

static const RcProg* rc_lookup(const RcCfg* cfg) {
    uint32_t h;
    size_t n = rc_used(cfg);
    int k;
    /* most draws that get here set up the same TEV as the draw before */
    if (s_rc_last >= 0 && memcmp(&s_rc[s_rc_last].cfg, cfg, n) == 0) return &s_rc[s_rc_last].prog;
    h = fnv(cfg, n);
    for (k = 0; k < s_rc_count; k++)
        if (s_rc_hash[k] == h && memcmp(&s_rc[k].cfg, cfg, n) == 0) {
            s_rc_last = k;
            return &s_rc[k].prog;
        }
    k = s_rc_count < RC_CACHE ? s_rc_count++ : (int)(s_draws % RC_CACHE);
    s_rc_last = k;
    s_rc_gen++;
    s_rc[k].hash = s_rc_hash[k] = h;
    s_rc[k].cfg = *cfg;
    rc_compile(cfg, &s_rc[k].prog);
    return &s_rc[k].prog;
}

static uint8_t clamp_s10(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* RREF_FIXED constants: rgb, a (nv2a_rc.c's movie YUV program, then the
 * unit vectors its swap-table dot products extract a channel with) */
static const uint8_t k_rc_fixed[8][4] = {
    { 90, 0, 0, 44 },     /* 0.351, 0, 0 | 0.1725 */
    { 0, 0, 113, 91 },    /* 0, 0, 0.443 | 0.357 */
    { 255, 0, 255, 0 },
    { 0, 255, 0, 0 },
    { 255, 0, 0, 0 },
    { 0, 255, 0, 0 },
    { 0, 0, 255, 0 },
    { 0, 0, 0, 0 },
};

static void ref_val(const XgxState* st, uint16_t ref, uint8_t rgb[3], uint8_t* a) {
    int t = ref >> 8, p = ref & 0xFF, k;
    switch (t) {
        case RREF_FIXED:
            for (k = 0; k < 3; k++) rgb[k] = k_rc_fixed[p & 7][k];
            *a = k_rc_fixed[p & 7][3];
            break;
        case RREF_TEVREG_RGB:
            for (k = 0; k < 3; k++) rgb[k] = clamp_s10(st->tevreg[p & 3][k]);
            break;
        case RREF_TEVREG_A: *a = clamp_s10(st->tevreg[p & 3][3]); break;
        case RREF_KONST_C:
            if (p <= 7) rgb[0] = rgb[1] = rgb[2] = (uint8_t)(255 * (8 - p) / 8);
            else if (p >= 0x0C && p <= 0x0F) for (k = 0; k < 3; k++) rgb[k] = st->konst[p - 0x0C][k];
            else if (p >= 0x10) rgb[0] = rgb[1] = rgb[2] = st->konst[(p - 0x10) & 3][((p - 0x10) >> 2) & 3];
            else rgb[0] = rgb[1] = rgb[2] = 0;
            break;
        case RREF_KONST_A:
            if (p <= 7) *a = (uint8_t)(255 * (8 - p) / 8);
            else if (p >= 0x10) *a = st->konst[(p - 0x10) & 3][((p - 0x10) >> 2) & 3];
            else *a = 0;
            break;
    }
}

static uint32_t pack_const(const XgxState* st, uint16_t rgb_ref, uint16_t a_ref) {
    uint8_t rgb[3] = { 0, 0, 0 }, a = 0;
    if (rgb_ref) ref_val(st, rgb_ref, rgb, &a);
    if (a_ref) ref_val(st, a_ref, rgb, &a);
    if (a_ref && !rgb_ref) rgb[0] = rgb[1] = rgb[2] = 0;
    if (rgb_ref && !a_ref) a = 0;
    return (uint32_t)a << 24 | (uint32_t)rgb[0] << 16 | (uint32_t)rgb[1] << 8 | rgb[2];
}

static void emit_combiners(const XgxState* st, const RcProg* rp) {
    int i;
    if (!s_rc_valid || rp != s_rc_sent || s_rc_sent_gen != s_rc_gen) {
        put1(NV097_SET_COMBINER_CONTROL, (uint32_t)rp->nstages | (1u << 12) | (1u << 16));
        for (i = 0; i < rp->nstages; i++) {
            put1(NV097_SET_COMBINER_COLOR_ICW + i * 4, rp->cicw[i]);
            put1(NV097_SET_COMBINER_COLOR_OCW + i * 4, rp->cocw[i]);
            put1(NV097_SET_COMBINER_ALPHA_ICW + i * 4, rp->aicw[i]);
            put1(NV097_SET_COMBINER_ALPHA_OCW + i * 4, rp->aocw[i]);
        }
        s_rc_cw0 = final_cw0(rp);
        put1(NV097_SET_COMBINER_SPECULAR_FOG_CW0, s_rc_cw0);
        put1(NV097_SET_COMBINER_SPECULAR_FOG_CW1, rp->cw1);
        s_rc_sent = rp;
        s_rc_sent_gen = s_rc_gen;
        s_rc_valid = 1;
        memset(s_rc_consts, 0xA5, sizeof s_rc_consts);
        memset(s_rc_fconsts, 0xA5, sizeof s_rc_fconsts);
    }
    for (i = 0; i < rp->nstages; i++) {
        uint32_t c0 = pack_const(st, rp->cref[i][0], rp->cref[i][1]);
        uint32_t c1 = pack_const(st, rp->cref[i][2], rp->cref[i][3]);
        if (s_rc_consts[i][0] != c0) { put1(NV097_SET_COMBINER_FACTOR0 + i * 4, c0); s_rc_consts[i][0] = c0; }
        if (s_rc_consts[i][1] != c1) { put1(NV097_SET_COMBINER_FACTOR1 + i * 4, c1); s_rc_consts[i][1] = c1; }
    }
    {
        uint32_t f0 = pack_const(st, rp->fref[0], rp->fref[1]), f1 = pack_const(st, rp->fref[2], rp->fref[3]);
        if (s_rc_fconsts[0] != f0) { put1(NV097_SET_SPECULAR_FOG_FACTOR, f0); s_rc_fconsts[0] = f0; }
        if (s_rc_fconsts[1] != f1) { put1(NV097_SET_SPECULAR_FOG_FACTOR + 4, f1); s_rc_fconsts[1] = f1; }
    }
}

/* ======================================================================
 * Texture units
 * ====================================================================== */
static uint32_t s_tex_shadow[4][9];
static uint32_t s_tex_prog = 0xFFFFFFFFu;   /* NV097_SET_SHADER_STAGE_PROGRAM sent last */
static uint32_t s_tex_in = 0xFFFFFFFFu;     /* NV097_SET_SHADER_OTHER_STAGE_INPUT sent last */
static uint32_t s_bump_shadow[4][4];        /* SET_TEXTURE_SET_BUMP_ENV_MAT per unit, as sent */
static uint32_t s_d_unit_tex[4];            /* derive_units: a unit's texture when not its map's (an indirect map) */
static int s_d_unit_in[4], s_d_nbump;       /* a bump unit's input unit; bump units this draw */
static float s_d_unit_bump[4][4];           /* a bump unit's matrix: 00 01 10 11 */

static uint32_t wrap_mode(uint32_t gx) {
    switch (gx) {
        case GX_MIRROR: return 2;
        case GX_CLAMP: return 3;
        default: return 1;
    }
}

static void emit_textures(const XgxState* st, const int unit_map[4], int nunits) {
    uint32_t prog = 0;
    int u;
    for (u = 0; u < 4; u++) {
        uint32_t v[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        const Tex* t = NULL;
        const XgxMap* m = NULL;
        if (u < nunits) {
            uint32_t id;
            m = &st->map[unit_map[u]];
            id = s_d_unit_tex[u] ? s_d_unit_tex[u] : m->tex;
            if (id && id < MAX_TEX && s_tex[id].used) t = &s_tex[id];
        }
        if (t) {
            uint32_t minf = m->min_filter + 1, magf = m->mag_filter == 0 ? 1 : 2;
            if (t->levels <= 1 && minf > 2) minf = minf == 3 || minf == 5 ? 1 : 2;   /* no mips: drop the mip part */
            v[0] = (uint32_t)t->mem & 0x03FFFFFF;
            if (t->rect) {   /* linear, texel coordinates: no size in the format, clamp only */
                v[1] = 1 | (1u << 3) | (2u << 4) | ((uint32_t)t->nvfmt << 8) | (1u << 16);
                v[2] = 3 | (3u << 8) | (3u << 16);
                v[7] = (uint32_t)t->pitch << 16;
                v[8] = (uint32_t)t->w << 16 | t->h;
            } else {
                v[1] = 1 | (1u << 3) | (2u << 4) | ((uint32_t)t->nvfmt << 8) | ((uint32_t)t->levels << 16) |
                       ((uint32_t)log2i(t->w) << 20) | ((uint32_t)log2i(t->h) << 24);
                v[2] = wrap_mode(m->wrap_s) | (wrap_mode(m->wrap_t) << 8) | (3u << 16);
            }
            v[3] = 0x4003FFC0u;
            v[4] = (minf << 16) | (magf << 24) | ((uint32_t)((int)(m->lod_bias * 256.0f)) & 0x1FFF) | 0x2000u;
            /* not sent: a texture made at a freed one's address, format and
             * size still re-sends the unit, so a P8 texture's palette is
             * loaded again (xemu reads palettes at each draw; the console
             * may keep the one it has) */
            v[5] = t->gen;
            v[6] = t->pal;   /* DMA A: bit 0 clear */
            prog |= (s_d_unit_kind[u] == UNIT_BUMP ? 6u : 1u) << (u * 5);   /* BUMPENVMAP, 2D_PROJECTIVE */
        }
        if (memcmp(v, s_tex_shadow[u], sizeof v) != 0) {
            uint32_t b = (uint32_t)u * 64;
            if (!t) {
                put1(NV097_SET_TEXTURE_CONTROL0 + b, 0);
            } else {
                put1(NV097_SET_TEXTURE_OFFSET + b, v[0]);
                put1(NV097_SET_TEXTURE_FORMAT + b, v[1]);
                put1(NV097_SET_TEXTURE_ADDRESS + b, v[2]);
                put1(NV097_SET_TEXTURE_CONTROL0 + b, v[3]);
                put1(NV097_SET_TEXTURE_FILTER + b, v[4]);
                if (v[6]) put1(NV097_SET_TEXTURE_PALETTE + b, v[6]);
                if (v[7]) {
                    put1(NV097_SET_TEXTURE_CONTROL1 + b, v[7]);
                    put1(NV097_SET_TEXTURE_IMAGE_RECT + b, v[8]);
                }
            }
            memcpy(s_tex_shadow[u], v, sizeof v);
        }
    }
    if (prog != s_tex_prog) {
        put1(NV097_SET_SHADER_STAGE_PROGRAM, prog);
        s_tex_prog = prog;
    }
    if (s_d_nbump) {   /* indirect: which earlier unit each bump unit reads, and its matrix */
        uint32_t in = 0;
        for (u = 1; u < nunits; u++) {
            uint32_t w[4];
            if (s_d_unit_kind[u] != UNIT_BUMP) continue;
            if (u >= 2) in |= (uint32_t)s_d_unit_in[u] << (16 + (u - 2) * 4);   /* unit 1 reads unit 0 */
            /* the method takes 00 01 11 10 (xemu's swizzle; 00 01 10 11 are s from du, t from du,
             * s from dv, t from dv) */
            memcpy(&w[0], &s_d_unit_bump[u][0], 8);
            memcpy(&w[2], &s_d_unit_bump[u][3], 4);
            memcpy(&w[3], &s_d_unit_bump[u][2], 4);
            if (memcmp(w, s_bump_shadow[u], sizeof w) != 0) {
                uint32_t k;
                for (k = 0; k < 4; k++) put1(NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + (uint32_t)u * 64 + k * 4, w[k]);
                memcpy(s_bump_shadow[u], w, sizeof w);
            }
        }
        if (in != s_tex_in) {
            put1(NV097_SET_SHADER_OTHER_STAGE_INPUT, in);
            s_tex_in = in;
        }
    }
}

/* ======================================================================
 * Fixed-function pixel state
 * ====================================================================== */
static int s_fixed[24];
static int s_fog_force = 1;   /* emit_fog regardless of XGX_DIRTY_FOG (GPU state reset) */
/* vertex attribute arrays and inline values last sent (emit_vertex_arrays) */
static uint32_t s_attr_shadow[16], s_attr_off_shadow[16];
static uint32_t s_default_mtx = 0xFFFFFFFFu;
static int s_inline_col[2];   /* the inline colour holds our opaque white */

static void state_reset_shadows(void) {
    s_draw_force = XGX_DIRTY_ALL;
    s_fog_force = 1;
    memset(s_fixed, 0xFF, sizeof s_fixed);
    memset(s_tex_shadow, 0xFF, sizeof s_tex_shadow);
    s_tex_prog = 0xFFFFFFFFu;
    s_tex_in = 0xFFFFFFFFu;
    memset(s_bump_shadow, 0xFF, sizeof s_bump_shadow);
    s_rc_valid = 0;
    s_vc_valid = 0;
    s_vp_cur = -1;
    s_inline_col[0] = s_inline_col[1] = 0;
    s_default_mtx = 0xFFFFFFFFu;
}

#define SETF(i, method, value)                                                                                      \
    do {                                                                                                            \
        int v_ = (int)(value);                                                                                      \
        if (s_fixed[i] != v_) { put1(method, (uint32_t)v_); s_fixed[i] = v_; }                                     \
    } while (0)

static uint32_t blend_factor(uint32_t gx, int is_src) {
    switch (gx) {
        case 0: return 0;           /* ZERO */
        case 1: return 1;           /* ONE */
        case 2: return is_src ? 0x306 : 0x300;   /* src: DSTCLR, dst: SRCCLR */
        case 3: return is_src ? 0x307 : 0x301;
        case 4: return 0x302;       /* SRCALPHA */
        case 5: return 0x303;
        case 6: return 0x304;       /* DSTALPHA */
        default: return 0x305;
    }
}

static void emit_fog(const XgxState* st);

static void emit_fixed(const XgxState* st) {
    int x0, y0, x1, y1;
#ifdef XGX_DEBUG_NOZ
    SETF(0, NV097_SET_DEPTH_TEST_ENABLE, 0);
#else
    SETF(0, NV097_SET_DEPTH_TEST_ENABLE, st->z_enable ? 1 : 0);
#endif
    SETF(1, NV097_SET_DEPTH_FUNC, 0x200 + (st->z_func & 7));
    SETF(2, NV097_SET_DEPTH_MASK, st->z_update ? 1 : 0);
    switch (st->blend_type) {
        case GX_BM_BLEND:
            SETF(3, NV097_SET_BLEND_ENABLE, 1);
            SETF(4, NV097_SET_BLEND_FUNC_SFACTOR, blend_factor(st->blend_src, 1));
            SETF(5, NV097_SET_BLEND_FUNC_DFACTOR, blend_factor(st->blend_dst, 0));
            SETF(6, NV097_SET_BLEND_EQUATION, NV097_SET_BLEND_EQUATION_V_FUNC_ADD);
            SETF(7, NV097_SET_LOGIC_OP_ENABLE, 0);
            break;
        case GX_BM_SUBTRACT:
            SETF(3, NV097_SET_BLEND_ENABLE, 1);
            SETF(4, NV097_SET_BLEND_FUNC_SFACTOR, 1);
            SETF(5, NV097_SET_BLEND_FUNC_DFACTOR, 1);
            SETF(6, NV097_SET_BLEND_EQUATION, NV097_SET_BLEND_EQUATION_V_FUNC_REVERSE_SUBTRACT);
            SETF(7, NV097_SET_LOGIC_OP_ENABLE, 0);
            break;
        case GX_BM_LOGIC:
            SETF(3, NV097_SET_BLEND_ENABLE, 0);
            SETF(7, NV097_SET_LOGIC_OP_ENABLE, 1);
            SETF(8, NV097_SET_LOGIC_OP, 0x1500 + (st->blend_logic & 15));
            break;
        default:
            SETF(3, NV097_SET_BLEND_ENABLE, 0);
            SETF(7, NV097_SET_LOGIC_OP_ENABLE, 0);
            break;
    }
#ifdef XGX_DEBUG_NOCULL
    SETF(9, NV097_SET_CULL_FACE_ENABLE, 0);
#else
    SETF(9, NV097_SET_CULL_FACE_ENABLE, st->cull != GX_CULL_NONE);
#endif
    if (st->cull != GX_CULL_NONE)
        SETF(10, NV097_SET_CULL_FACE, st->cull == GX_CULL_FRONT ? 0x404 : st->cull == GX_CULL_BACK ? 0x405 : 0x408);
    SETF(11, NV097_SET_COLOR_MASK,
         (st->color_update ? NV097_SET_COLOR_MASK_RED_WRITE_ENABLE | NV097_SET_COLOR_MASK_GREEN_WRITE_ENABLE |
                                 NV097_SET_COLOR_MASK_BLUE_WRITE_ENABLE
                           : 0) |
             (st->alpha_update ? NV097_SET_COLOR_MASK_ALPHA_WRITE_ENABLE : 0));
    SETF(12, NV097_SET_DITHER_ENABLE, s_bpp == 16 ? 1 : st->dither ? 1 : 0);
    /* scissor, in framebuffer pixels, inside the content rect */
    x0 = map_x((float)st->scissor[0]);
    y0 = map_y((float)st->scissor[1]);
    x1 = map_x((float)(st->scissor[0] + st->scissor[2]));
    y1 = map_y((float)(st->scissor[1] + st->scissor[3]));
    if (x0 < s_cx) x0 = s_cx;
    if (y0 < s_cy) y0 = s_cy;
    if (x1 > s_cx + s_cw) x1 = s_cx + s_cw;
    if (y1 > s_cy + s_ch) y1 = s_cy + s_ch;
    if (x1 <= x0 || y1 <= y0) { x0 = y0 = 0; x1 = y1 = 1; }
    /* the hardware's max is inclusive: GX's scissor ends before x1, y1 */
    SETF(13, NV097_SET_WINDOW_CLIP_HORIZONTAL, (uint32_t)x0 | ((uint32_t)(x1 - 1) << 16));
    SETF(14, NV097_SET_WINDOW_CLIP_VERTICAL, (uint32_t)y0 | ((uint32_t)(y1 - 1) << 16));
    /* alpha test: the two-reference GX compare, folded to one when possible */
    {
        uint32_t c0 = st->alpha_comp0, c1 = st->alpha_comp1, op = st->alpha_op;
        uint32_t r0 = st->alpha_ref0, r1 = st->alpha_ref1, fn = c0, ref = r0;
        int en = 1;
        if (c0 == GX_ALWAYS && c1 == GX_ALWAYS) en = 0;
        else if (op == GX_AOP_AND && c1 == GX_ALWAYS) { fn = c0; ref = r0; }
        else if (op == GX_AOP_AND && c0 == GX_ALWAYS) { fn = c1; ref = r1; }
        else if (op == GX_AOP_OR && c1 == GX_NEVER) { fn = c0; ref = r0; }
        else if (op == GX_AOP_OR && c0 == GX_NEVER) { fn = c1; ref = r1; }
        else if (op == GX_AOP_OR && (c0 == GX_ALWAYS || c1 == GX_ALWAYS)) en = 0;
        if (st->ztex && !en) {   /* the Z-texture mask (derive_units): drop alpha 0 */
            en = 1;
            fn = GX_GREATER;
            ref = 0;
        }
        SETF(15, NV097_SET_ALPHA_TEST_ENABLE, en);
        if (en) {
            SETF(16, NV097_SET_ALPHA_FUNC, 0x200 + (fn & 7));
            SETF(17, NV097_SET_ALPHA_REF, ref);
        }
    }
    /* the front end clears dirty after each draw: only a changed GXSetFog gets here */
    if ((st->dirty & XGX_DIRTY_FOG) || s_fog_force) emit_fog(st);
}

/* ======================================================================
 * Draw
 * ====================================================================== */
void* xgx_vtx_alloc(uint32_t count, uint32_t stride) {
    uint32_t bytes = count * stride;
    frame_open();
    if (!count || bytes > RING_BYTES / 2) return NULL;
    if (s_ring_pos + bytes > RING_BYTES) {
        wait_idle();   /* the GPU still reads the older part */
        pb_open();
        s_ring_pos = 0;
    }
    /* at a multiple of the stride from the ring's start: see xgx_draw */
    s_draw_base = s_ring + (s_ring_pos + stride - 1) / stride * stride;
    if (s_draw_base + bytes > s_ring + RING_BYTES) {
        wait_idle();
        pb_open();
        s_draw_base = s_ring;
    }
    s_ring_pos = (uint32_t)(s_draw_base - s_ring) + bytes;
    return (void*)s_draw_base;
}

uint32_t xgx_vbuf_offset(const void* p) { return (uint32_t)((const uint8_t*)p - s_vb.base); }

void* xgx_vbuf_alloc(uint32_t bytes) {
    void* p;
    if (!s_vb.base || bytes > s_vb.bytes) return NULL;
    p = pool_alloc(&s_vb, bytes);
    if (!p && s_ndeferred) {   /* buffers the cache evicted wait for the GPU */
        wait_idle();
        release_deferred();
        p = pool_alloc(&s_vb, bytes);
    }
    return p;
}

void xgx_vbuf_free(void* p) {
    if (p) defer_free(p);
}

/* A buffer no draw of the current frame used: xgx_present waited for the
 * GPU to go idle before the frame began, so nothing can still read it.
 * Freeing these at once spares the wait for idle that a full pool otherwise
 * costs (Pokémon Stadium: ~4 extra a frame, each one stopping the CPU until
 * the GPU has drawn everything queued). */
void xgx_vbuf_free_now(void* p) {
    /* before the frame's first GPU use the last frame may still be drawing
     * (XGX_OVERLAP): opening the frame waits for it */
    if (p && XGX_VBUF_FREE_NOW) frame_open();
    if (p && XGX_VBUF_FREE_NOW) pool_free(&s_vb, p);
    else if (p) defer_free(p);
}

void xgx_vtx_use(const void* verts) { s_draw_base = (const uint8_t*)verts; }

static uint32_t nv_prim(uint32_t gx) {
    switch (gx) {
        case XGX_QUADS: return NV097_SET_BEGIN_END_OP_QUADS;
        case XGX_TRIANGLES: return NV097_SET_BEGIN_END_OP_TRIANGLES;
        case XGX_TRISTRIP: return NV097_SET_BEGIN_END_OP_TRIANGLE_STRIP;
        case XGX_TRIFAN: return NV097_SET_BEGIN_END_OP_TRIANGLE_FAN;
        case XGX_LINES: return NV097_SET_BEGIN_END_OP_LINES;
        case XGX_LINESTRIP: return NV097_SET_BEGIN_END_OP_LINE_STRIP;
        default: return NV097_SET_BEGIN_END_OP_POINTS;
    }
}


static const uint8_t* s_attr_base;   /* what the array offsets point at (xgx_draw) */
#define VTX_WINDOW 0x8000u                 /* vertices per array-offset window (xgx_draw) */

static void attr(int slot, int off, uint32_t type, uint32_t size, uint32_t stride) {
    uint32_t fmt = off < 0 ? 2u : type | size << 4 | stride << 8;
    uint32_t addr = off < 0 ? 0 : (((uint32_t)s_attr_base & 0x03FFFFFF) + (uint32_t)off);
    if (s_attr_shadow[slot] != fmt) {
        put1(NV097_SET_VERTEX_DATA_ARRAY_FORMAT + slot * 4, fmt);
        s_attr_shadow[slot] = fmt;
    }
    if (off >= 0 && s_attr_off_shadow[slot] != addr) {
        put1(NV097_SET_VERTEX_DATA_ARRAY_OFFSET + slot * 4, addr);
        s_attr_off_shadow[slot] = addr;
    }
}

static void emit_vertex_arrays(const XgxLayout* l, const XgxState* st) {
    int n;
    uint32_t s = l->stride;
    attr(VPI_POS, l->off_pos, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3, s);
    attr(VPI_MTX, l->off_mtx, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 1, s);
    attr(VPI_NRM, l->off_nrm, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3, s);
    attr(VPI_COL0, l->off_col[0], NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL, 4, s);
    attr(VPI_COL1, l->off_col[1], NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL, 4, s);
    for (n = 0; n < 8; n++) attr(vpi_tex(n), l->off_tc[n], NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, s);
    /* inline values for the absent ones. An array draw leaves its last vertex
     * in the attribute's inline value (NV2A, and xemu models it), so after a
     * skinned draw the cached default matrix index is gone. */
    if (l->off_mtx >= 0) s_default_mtx = 0xFFFFFFFFu;
    if (l->off_mtx < 0 && s_default_mtx != st->cur_posmtx) {
        putf(NV097_SET_VERTEX_DATA4F_M + VPI_MTX * 16, (float)st->cur_posmtx);
        putf(NV097_SET_VERTEX_DATA4F_M + VPI_MTX * 16 + 4, 0);
        putf(NV097_SET_VERTEX_DATA4F_M + VPI_MTX * 16 + 8, 0);
        putf(NV097_SET_VERTEX_DATA4F_M + VPI_MTX * 16 + 12, 1);
        s_default_mtx = st->cur_posmtx;
    }
    for (n = 0; n < 2; n++) {
        if (l->off_col[n] >= 0) {
            s_inline_col[n] = 0;   /* the array's last vertex will be left there */
        } else if (!s_inline_col[n]) {
            put1(NV097_SET_VERTEX_DATA4UB + (n ? VPI_COL1 : VPI_COL0) * 4, 0xFFFFFFFFu);
            s_inline_col[n] = 1;
        }
    }
}

static uint8_t vp_attn(uint32_t attn_fn) {
    switch (attn_fn) {
        case GX_AF_SPEC: return VPL_SPEC;
        case GX_AF_SPOT: return VPL_SPOT;
        default: return VPL_DIFFUSE;
    }
}

#ifdef XGX_DEBUG_TRACE
static uint32_t rgb565_to_888(uint16_t v) {
    uint32_t r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
    return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}

/* autopad "env XGX_SKIP=<first>-<last>": the traced frame leaves those
 * draws out, so a shot shows which pixels they make */
static int trace_skip(uint32_t draw) {
    static int lo = -2, hi = -1;
    if (lo == -2) {
        const char* e = getenv("XGX_SKIP");
        lo = hi = -1;
        if (e) {
            lo = atoi(e);
            hi = strchr(e, '-') ? atoi(strchr(e, '-') + 1) : lo;
        }
    }
    return (int)draw >= lo && (int)draw <= hi;
}

/* -DXGX_DEBUG_TRACE: log every draw of the frame an autopad SHOT dumps */
static void trace_draw(uint32_t prim, uint32_t count, const XgxState* st, int approx, const XgxLayout* l) {
    uint32_t s, i;
    xhw_logf("[DRAW] #%u prim %u n %u tev %u ind %u texgen %u approx %d blend %u %u %u alpha %u/%u %u %u/%u z %u/%u/%u",
             s_draws, prim, count, st->ntev, st->nind, st->ntexgen, approx, st->blend_type, st->blend_src,
             st->blend_dst, st->alpha_comp0, st->alpha_ref0, st->alpha_op, st->alpha_comp1, st->alpha_ref1,
             st->z_enable, st->z_func, st->z_update);
    if (l->off_pos >= 0 && !st->proj_ortho) {
        /* screen box (EFB pixels) of the draw's vertices, to find a draw in a shot */
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        uint32_t v;
        for (v = 0; v < count; v++) {
            const uint8_t* vx = s_draw_base + v * l->stride;
            const float* p = (const float*)(vx + l->off_pos);
            uint32_t row = l->off_mtx >= 0 ? (uint32_t)*(const float*)(vx + l->off_mtx) : st->cur_posmtx;
            const float(*m)[4] = st->posmtx[(row / 3) % XGX_NUM_POSMTX];
            float e[4], c[4];
            int r;
            for (r = 0; r < 3; r++) e[r] = m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3];
            e[3] = 1;
            for (r = 0; r < 4; r++) c[r] = st->proj[r][0] * e[0] + st->proj[r][1] * e[1] + st->proj[r][2] * e[2] + st->proj[r][3] * e[3];
            if (c[3] <= 0.001f) continue;
            c[0] = st->viewport[0] + st->viewport[2] * 0.5f * (1 + c[0] / c[3]);
            c[1] = st->viewport[1] + st->viewport[3] * 0.5f * (1 - c[1] / c[3]);
            if (c[0] < x0) x0 = c[0];
            if (c[0] > x1) x1 = c[0];
            if (c[1] < y0) y0 = c[1];
            if (c[1] > y1) y1 = c[1];
        }
        if (x0 <= x1) xhw_logf("[DRAW]  screen %d,%d - %d,%d", (int)x0, (int)y0, (int)x1, (int)y1);
        xhw_logf("[DRAW]  scissor %d,%d %dx%d viewport %d,%d %dx%d", (int)st->scissor[0], (int)st->scissor[1],
                 (int)st->scissor[2], (int)st->scissor[3], (int)st->viewport[0], (int)st->viewport[1],
                 (int)st->viewport[2], (int)st->viewport[3]);
    }
    for (s = 0; s < st->ntev && s < XGX_MAX_TEV; s++) {
        const XgxTevStage* t = &st->tev[s];
        const XgxMap* m = t->texmap < XGX_MAX_MAPS ? &st->map[t->texmap] : NULL;
        const Tex* x = m && m->tex && m->tex < MAX_TEX && s_tex[m->tex].used ? &s_tex[m->tex] : NULL;
        xhw_logf("[DRAW]  s%u tc %u map %u ch %u c %u %u %u %u op %u a %u %u %u %u op %u k %u/%u out %u/%u ind %u/%u/%u/%u"
                 " tex %ux%u fmt %02x lv %u",
                 s, t->texcoord, t->texmap, t->chan, t->cin[0], t->cin[1], t->cin[2], t->cin[3], t->cop, t->ain[0],
                 t->ain[1], t->ain[2], t->ain[3], t->aop, t->kcsel, t->kasel, t->cout, t->aout, t->ind_stage,
                 t->ind_format, t->ind_mtx, t->ind_add_prev, x ? x->w : 0, x ? x->h : 0, x ? x->nvfmt : 0,
                 x ? x->levels : 0);
        if (x && x->mem && (x->nvfmt == nv_format(XGX_TEX_DXT1) || x->nvfmt == nv_format(XGX_TEX_DXT3))) {
            /* level 0's colour endpoints: average and the centre block's */
            uint32_t bs = x->nvfmt == nv_format(XGX_TEX_DXT1) ? 8 : 16, nb = ((x->w + 3) / 4) * ((x->h + 3) / 4), b,
                     sum[3] = { 0, 0, 0 }, mid = (x->h / 8) * ((x->w + 3) / 4) + x->w / 8;
            const uint8_t* p = (const uint8_t*)x->mem + (bs - 8);
            for (b = 0; b < nb; b++, p += bs) {
                uint32_t c = rgb565_to_888((uint16_t)(p[0] | p[1] << 8));
                sum[0] += c >> 16 & 255; sum[1] += c >> 8 & 255; sum[2] += c & 255;
            }
            p = (const uint8_t*)x->mem + mid * bs + (bs - 8);
            xhw_logf("[DRAW]  s%u texels: average c0 %02x%02x%02x, centre block c0 %06x c1 %06x idx %02x%02x%02x%02x", s,
                     sum[0] / nb, sum[1] / nb, sum[2] / nb, rgb565_to_888((uint16_t)(p[0] | p[1] << 8)),
                     rgb565_to_888((uint16_t)(p[2] | p[3] << 8)), p[4], p[5], p[6], p[7]);
        }
    }
    xhw_logf("[DRAW]  k %02x%02x%02x%02x %02x%02x%02x%02x swap %u%u%u%u %u%u%u%u stage swap %u/%u", st->konst[0][0],
             st->konst[0][1], st->konst[0][2], st->konst[0][3], st->konst[1][0], st->konst[1][1], st->konst[1][2],
             st->konst[1][3], st->swap[0][0], st->swap[0][1], st->swap[0][2], st->swap[0][3], st->swap[1][0],
             st->swap[1][1], st->swap[1][2], st->swap[1][3], st->tev[0].ras_swap, st->tev[0].tex_swap);
    for (i = 0; i < st->nchans * 2 && i < 4; i++)
        xhw_logf("[DRAW]  chan%u on %u amb %u mat %u lights %02x diff %u attn %u | amb %02x%02x%02x%02x mat %02x%02x%02x%02x",
                 i, st->chan[i].enable, st->chan[i].amb_src, st->chan[i].mat_src, st->chan[i].light_mask,
                 st->chan[i].diff_fn, st->chan[i].attn_fn, st->amb[i >> 1][0], st->amb[i >> 1][1],
                 st->amb[i >> 1][2], st->amb[i >> 1][3], st->mat[i >> 1][0], st->mat[i >> 1][1], st->mat[i >> 1][2],
                 st->mat[i >> 1][3]);
    for (i = 0; i < XGX_MAX_LIGHTS; i++)
        if (st->nchans && (st->chan[0].light_mask >> i & 1))
            xhw_logf("[DRAW]  light%u colour %02x%02x%02x%02x pos %d %d %d dir %d %d %d", i, st->light[i].color[0],
                     st->light[i].color[1], st->light[i].color[2], st->light[i].color[3], (int)st->light[i].pos[0],
                     (int)st->light[i].pos[1], (int)st->light[i].pos[2], (int)(st->light[i].dir[0] * 100),
                     (int)(st->light[i].dir[1] * 100), (int)(st->light[i].dir[2] * 100));
    for (i = 0; i < st->ntexgen && i < XGX_MAX_TEXGEN; i++)
        xhw_logf("[DRAW]  tg%u type %u src %u mtx %u norm %u pt %u", i, st->texgen[i].type, st->texgen[i].src,
                 st->texgen[i].mtx, st->texgen[i].normalize, st->texgen[i].pt_mtx);
    for (i = 0; i < st->ntexgen && i < XGX_MAX_TEXGEN; i++) {
        float m[3][4], pt[3][4];
        int r;
        if (st->texgen[i].mtx == GX_IDENTITY && st->texgen[i].pt_mtx == GX_PTIDENTITY) continue;
        texgen_src_mtx(st, st->texgen[i].mtx, m);
        if (st->texgen[i].pt_mtx >= GX_PTTEXMTX0 && st->texgen[i].pt_mtx < GX_PTIDENTITY)
            memcpy(pt, st->ptmtx[(st->texgen[i].pt_mtx - GX_PTTEXMTX0) / 3], 48);
        else memset(pt, 0, 48);
        for (r = 0; r < 3; r++)   /* x1000 */
            xhw_logf("[DRAW]  tg%u row%d mtx %d %d %d %d pt %d %d %d %d", i, r, (int)(m[r][0] * 1000),
                     (int)(m[r][1] * 1000), (int)(m[r][2] * 1000), (int)(m[r][3] * 1000), (int)(pt[r][0] * 1000),
                     (int)(pt[r][1] * 1000), (int)(pt[r][2] * 1000), (int)(pt[r][3] * 1000));
    }
    if (st->fog_type & 7)
        xhw_logf("[DRAW]  fog type %u start %d end %d near %d far %d colour %02x%02x%02x", st->fog_type,
                 (int)st->fog_start, (int)st->fog_end, (int)st->fog_near, (int)st->fog_far, st->fog_color[0],
                 st->fog_color[1], st->fog_color[2]);
}
#endif

/* State derived from the front end's state, rebuilt only when the dirty
 * groups it depends on changed (or s_draw_force says the GPU state was
 * reset). Most draws change only matrices and vertices, and rebuilding and
 * hashing the combiner setup, the vertex-program key and the texture units
 * for each of ~2000 draws a frame cost more than the game itself. */
static RcCfg s_d_rc;
static const RcProg* s_d_rp;
static int s_d_unit_map[4], s_d_unit_tc[4], s_d_nunits, s_d_unit_miss, s_d_tg_posmtx;
static VpKey s_d_vk;
static uint32_t s_d_spec_lights, s_d_layout = 0xFFFFFFFFu;

#define DIRTY_UNITS (XGX_DIRTY_TEV | XGX_DIRTY_MAPS)
#define DIRTY_VK (DIRTY_UNITS | XGX_DIRTY_CHANS | XGX_DIRTY_TEXGEN | XGX_DIRTY_LIGHTS)
#define DIRTY_TG (DIRTY_UNITS | XGX_DIRTY_TEXGEN | XGX_DIRTY_TEXMTX)
#define DIRTY_FIXED (XGX_DIRTY_PIXEL | XGX_DIRTY_SCISSOR | XGX_DIRTY_FOG)

/* ======================================================================
 * Indirect texturing (GXSetTevIndirect)
 * ====================================================================== */
#ifndef XGX_NO_INDIRECT
#define XGX_NO_INDIRECT 0   /* 1: indirect stages draw as direct ones (as up to v40) */
#endif

/* A GX indirect stage offsets a TEV stage's texture coordinate, in texels,
 * by a 2x3 matrix times (s, t, u) - bias, read from the indirect texture's
 * alpha, blue and green (bias -128 for GX_ITF_8). The NV2A's BUMPENVMAP unit
 * offsets its coordinate by a 2x2 matrix times (du, dv), read from an
 * earlier unit's blue and green as two's complement bytes / 127 (xemu's
 * sign3). So the indirect map is drawn from a copy with s / 2 in blue and
 * t / 2 in green (0..127): no texel crosses the two's complement wrap, so
 * filtering between texels stays linear as on GX, the bias becomes a
 * constant offset folded into the perturbed unit's texgen
 * (s_d_unit_off), and the lowest bit of s and t is lost (half a step,
 * offset by a quarter on average). The copies are small (Melee's are 32x32
 * IA8) and kept per source texture and generation. */
typedef struct { uint32_t src, gen, tex, used; } IndTex;
static IndTex s_ind_tex[4];

/* GX alpha (s) and blue (t) of texel i of a texture as the pool holds it */
static int ind_texel(const Tex* t, uint32_t i, uint8_t* a, uint8_t* b) {
    const uint8_t* m = (const uint8_t*)t->mem;
    uint32_t w;
    switch (t->nvfmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_AY8: *a = *b = m[i]; return 1;
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8:   /* luminance, alpha */
            w = ((const uint16_t*)m)[i];
            *b = (uint8_t)w;
            *a = (uint8_t)(w >> 8);
            return 1;
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5:
            w = ((const uint16_t*)m)[i] & 31u;
            *a = 255;
            *b = (uint8_t)(w << 3 | w >> 2);
            return 1;
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8:
            w = ((const uint32_t*)t->base)[m[i]];   /* the palette precedes the levels */
            *a = (uint8_t)(w >> 24);
            *b = (uint8_t)w;
            return 1;
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8:
            w = ((const uint32_t*)m)[i];
            *a = (uint8_t)(w >> 24);
            *b = (uint8_t)w;
            return 1;
        default: return 0;   /* DXT: not as an indirect map */
    }
}

static void ind_tex_flush(void) {
    int k;
    for (k = 0; k < 4; k++)
        if (s_ind_tex[k].tex) xgx_tex_destroy(s_ind_tex[k].tex);
    memset(s_ind_tex, 0, sizeof s_ind_tex);
    s_draw_force |= XGX_DIRTY_MAPS;   /* units bound to them are derived again */
}

/* the BUMPENVMAP input copy of texture src; 0: none (format, size or pool) */
static uint32_t ind_tex(uint32_t src) {
    const Tex* t;
    uint32_t i, n, id, *out;
    uint8_t a, b;
    int k, lru = 0;
    if (!src || src >= MAX_TEX || !s_tex[src].used) return 0;
    t = &s_tex[src];
    for (k = 0; k < 4; k++)
        if (s_ind_tex[k].tex && s_ind_tex[k].src == src && s_ind_tex[k].gen == t->gen) {
            s_ind_tex[k].used = s_frame;
            return s_ind_tex[k].tex;
        }
    n = (uint32_t)t->w * t->h;
    if (t->rect || n > 65536 || !ind_texel(t, 0, &a, &b)) return 0;
    for (k = 1; k < 4; k++)
        if (s_ind_tex[k].used < s_ind_tex[lru].used) lru = k;
    if (s_ind_tex[lru].tex) xgx_tex_destroy(s_ind_tex[lru].tex);
    memset(&s_ind_tex[lru], 0, sizeof s_ind_tex[lru]);
    id = xgx_tex_create(t->w, t->h, 1, XGX_TEX_ARGB8, NULL);   /* same size: same swizzled texel order */
    if (!id) return 0;
    out = (uint32_t*)s_tex[id].mem;
    for (i = 0; i < n; i++) {   /* reads the pool (uncached), writes in order (write-combined) */
        ind_texel(t, i, &a, &b);
        out[i] = (uint32_t)(b >> 1) << 8 | (uint32_t)(a >> 1);   /* green t, blue s */
    }
    s_ind_tex[lru].src = src;
    s_ind_tex[lru].gen = t->gen;
    s_ind_tex[lru].tex = id;
    s_ind_tex[lru].used = s_frame;
    return id;
}

/* the TEV stage reads an indirect offset at all / one the NV2A draws:
 * static matrix, no wrap, no add-previous, no bump alpha, 8-bit format */
static int ind_active(const XgxState* st, const XgxTevStage* t) {
    return t->ind_stage < st->nind && t->ind_stage < 4 &&
           (t->ind_mtx != GX_ITM_OFF || t->ind_wrap_s != GX_ITW_OFF || t->ind_wrap_t != GX_ITW_OFF || t->ind_add_prev);
}

static int ind_exact(const XgxTevStage* t) {
    return t->ind_mtx > GX_ITM_OFF && t->ind_mtx <= GX_ITM_2 && t->ind_wrap_s == GX_ITW_OFF &&
           t->ind_wrap_t == GX_ITW_OFF && !t->ind_add_prev && t->ind_alpha == GX_ITBA_OFF && t->ind_format == GX_ITF_8;
}

/* bump unit u for TEV stage t: the matrix and the texgen offset. With
 * du = (v >> 1) / 127 the GX value is v ~ 254 du + 0.5 (the dropped bit's
 * mean), and s = v - 128 when biased. The offset is in texels of the GX
 * texture: a swizzled texture is sampled over [0, 1], so divide by its
 * size; a linear one is sampled in texels. The u column has no NV2A
 * counterpart (approximated when used). */
static int bump_setup(const XgxState* st, const XgxTevStage* t, int u) {
    const float(*m)[3] = st->ind_mtx[t->ind_mtx - 1];
    const XgxMap* mp = &st->map[t->texmap];
    const Tex* tx = mp->tex < MAX_TEX && s_tex[mp->tex].used ? &s_tex[mp->tex] : NULL;
    float w = tx && tx->rect ? 1.0f : mp->w ? (float)mp->w : 1.0f;
    float h = tx && tx->rect ? 1.0f : mp->h ? (float)mp->h : 1.0f;
    float cs = 0.5f - ((t->ind_bias & 1) ? 128.0f : 0.0f), ct = 0.5f - ((t->ind_bias & 2) ? 128.0f : 0.0f);
    s_d_unit_bump[u][0] = 254.0f * m[0][0] / w;   /* s from du */
    s_d_unit_bump[u][1] = 254.0f * m[1][0] / h;   /* t from du */
    s_d_unit_bump[u][2] = 254.0f * m[0][1] / w;   /* s from dv */
    s_d_unit_bump[u][3] = 254.0f * m[1][1] / h;   /* t from dv */
    s_d_unit_off[u][0] = (m[0][0] * cs + m[0][1] * ct) / w;
    s_d_unit_off[u][1] = (m[1][0] * cs + m[1][1] * ct) / h;
    return m[0][2] == 0.0f && m[1][2] == 0.0f;
}

/* a swap table as RcStage's tex_swz/ras_swz: two bits per channel */
static uint8_t swap_bits(const uint8_t t[4]) {
    return (uint8_t)((t[0] & 3) | (t[1] & 3) << 2 | (t[2] & 3) << 4 | (t[3] & 3) << 6);
}

/* texture units (one per distinct texcoord/texmap the TEV samples) and the combiner setup */
static void derive_units(const XgxState* st) {
    RcCfg* rc = &s_d_rc;
    int s, i, nunits = 0, nstages = st->ntev > RC_MAX_TEV ? RC_MAX_TEV : st->ntev ? (int)st->ntev : 1;
    memset(rc, 0, offsetof(RcCfg, st) + (size_t)nstages * sizeof(RcStage));   /* rc_used() */
    uint8_t emboss = 0;
    int ind_unit[4] = { -1, -1, -1, -1 };   /* per indirect stage: the unit its map is drawn on */
    uint8_t ind_use = 0;                    /* TEV stages drawn with their indirect offset */
    s_d_unit_miss = 0;
    s_d_nbump = 0;
    memset(s_d_unit_kind, 0, sizeof s_d_unit_kind);
    memset(s_d_unit_tex, 0, sizeof s_d_unit_tex);
    rc->nstages = (uint8_t)nstages;
    /* HSD's emboss bump: "prev + height(tc) * ras" then "prev - height(bump
     * of tc) * ras". The bump texgen is drawn as tc itself (unit_texgen), so
     * the pair cancels; drop it rather than let the combiners clamp the sum
     * at 1 before the subtraction (an inverted crate front). */
    for (s = 0; s + 1 < rc->nstages; s++) {
        const XgxTevStage *a = &st->tev[s], *b = &st->tev[s + 1];
        const XgxTexGen* bg = b->texcoord < XGX_MAX_TEXGEN ? &st->texgen[b->texcoord] : NULL;
        if (a->cin[0] == 15 && a->cin[1] == 8 && a->cin[2] == 10 && a->cin[3] == 0 && a->cop == 0 &&
            b->cin[0] == 15 && b->cin[1] == 8 && b->cin[2] == 10 && b->cin[3] == 0 && b->cop == 1 &&
            a->texmap == b->texmap && bg && bg->type >= GX_TG_BUMP0 && bg->type <= GX_TG_BUMP7 &&
            bg->src == GX_TG_TEXCOORD0 + a->texcoord)
            emboss |= (uint8_t)(3u << s);
    }
    /* Indirect maps get the first units: a BUMPENVMAP unit reads an earlier
     * unit's texture (NV097_SET_SHADER_OTHER_STAGE_INPUT), and unit 0
     * can't be one. A stage the NV2A can't offset exactly draws direct. */
    if (st->nind && !XGX_NO_INDIRECT) {
        for (s = 0; s < rc->nstages; s++) {
            const XgxTevStage* t = &st->tev[s];
            uint32_t k = t->ind_stage, imap, itc;
            if (!ind_active(st, t)) continue;
            imap = st->ind_order[k & 3][1];
            itc = st->ind_order[k & 3][0];
            if (t->texmap >= XGX_MAX_MAPS || t->texcoord >= XGX_MAX_TEXGEN || !st->map[t->texmap].tex ||
                (emboss >> s & 1))
                continue;   /* no texture to offset */
            if (!ind_exact(t) || imap >= XGX_MAX_MAPS || itc >= XGX_MAX_TEXGEN) {
                s_d_unit_miss++;
                continue;
            }
            if (ind_unit[k] < 0) {
                uint32_t tex = nunits < 3 ? ind_tex(st->map[imap].tex) : 0;   /* and room for a bump unit */
                if (!tex) {
                    s_d_unit_miss++;
                    continue;
                }
                ind_unit[k] = nunits++;
                s_d_unit_map[ind_unit[k]] = (int)imap;
                s_d_unit_tc[ind_unit[k]] = (int)itc;
                s_d_unit_kind[ind_unit[k]] = UNIT_IND;
                s_d_unit_tex[ind_unit[k]] = tex;
                s_d_unit_off[ind_unit[k]][0] = 1.0f / (float)(1u << (st->ind_scale[k][0] & 15));
                s_d_unit_off[ind_unit[k]][1] = 1.0f / (float)(1u << (st->ind_scale[k][1] & 15));
            }
            ind_use |= (uint8_t)(1u << s);
        }
    }
    for (s = 0; s < rc->nstages; s++) {
        const XgxTevStage* t = &st->tev[s];
        RcStage* r = &rc->st[s];
        int u = -1;
        for (i = 0; i < 4; i++) {
            r->cin[i] = (uint8_t)t->cin[i];
            r->ain[i] = (uint8_t)t->ain[i];
        }
        if (emboss >> s & 1) {   /* colour: d = CPREV; the alpha half samples no texture either */
            r->cin[0] = r->cin[1] = r->cin[2] = 15;
            r->cin[3] = 0;
            r->unit = -1;
        }
        r->cop = (uint8_t)t->cop; r->aop = (uint8_t)t->aop;
        r->cbias = (uint8_t)t->cbias; r->cscale = (uint8_t)t->cscale;
        r->abias = (uint8_t)t->abias; r->ascale = (uint8_t)t->ascale;
        r->cclamp = (uint8_t)t->cclamp; r->aclamp = (uint8_t)t->aclamp;
        r->cout = (uint8_t)(t->cout & 3); r->aout = (uint8_t)(t->aout & 3);
        r->kcsel = (uint8_t)t->kcsel; r->kasel = (uint8_t)t->kasel;
        r->ras = t->chan == GX_COLOR0A0 ? 0 : t->chan == GX_COLOR1A1 ? 1 : 2;
        r->ras_swz = r->ras == 2 ? RC_SWZ_ID : swap_bits(st->swap[t->ras_swap & 3]);
        if (!(emboss >> s & 1) && t->texmap != GX_NULL && t->texmap < XGX_MAX_MAPS && t->texcoord != GX_NULL &&
            st->map[t->texmap].tex) {
            if ((ind_use >> s & 1) && nunits < 4) {   /* a unit of its own, offset by its indirect map's */
                u = nunits++;
                s_d_unit_map[u] = (int)t->texmap;
                s_d_unit_tc[u] = (int)t->texcoord;
                s_d_unit_kind[u] = UNIT_BUMP;
                s_d_unit_in[u] = ind_unit[t->ind_stage];
                if (!bump_setup(st, t, u)) s_d_unit_miss++;
                s_d_nbump++;
            } else if (ind_use >> s & 1) {
                s_d_unit_miss++;   /* no unit left: drawn direct */
            }
            for (i = 0; i < nunits && u < 0; i++)
                if (s_d_unit_kind[i] == UNIT_PLAIN && s_d_unit_map[i] == (int)t->texmap &&
                    s_d_unit_tc[i] == (int)t->texcoord) { u = i; break; }
            if (u < 0 && nunits < 4) {
                u = nunits++;
                s_d_unit_map[u] = (int)t->texmap;
                s_d_unit_tc[u] = (int)t->texcoord;
            }
            if (u < 0) s_d_unit_miss++;
        }
        r->unit = (int8_t)u;
        r->tex_swz = u < 0 ? RC_SWZ_ID : swap_bits(st->swap[t->tex_swap & 3]);
        if (u >= 0) rc->units_used |= (uint8_t)(1u << u);
        if (r->ras == 1) rc->v1_used = 1;
    }
    /* GXSetZTexture: the last stage's texture is the mask xgx_ztex_mask put
     * in the depth copy; one more stage multiplies the alpha by it
     * (APREV * TEXA, colour passed on), and the alpha test drops the 0s */
    if (st->ztex && rc->nstages < RC_MAX_STAGES && rc->st[rc->nstages - 1].unit >= 0) {
        const RcStage* last = &rc->st[rc->nstages - 1];
        RcStage* r = &rc->st[rc->nstages];
        memset(r, 0, sizeof *r);
        r->cin[0] = r->cin[1] = r->cin[2] = 15;   /* GX_CC_ZERO */
        r->cin[3] = 0;                            /* GX_CC_CPREV */
        r->ain[0] = 7;                            /* GX_CA_ZERO */
        r->ain[1] = 0;                            /* GX_CA_APREV */
        r->ain[2] = 4;                            /* GX_CA_TEXA */
        r->ain[3] = 7;
        r->cclamp = r->aclamp = 1;
        r->unit = last->unit;
        r->ras = 2;
        r->tex_swz = r->ras_swz = RC_SWZ_ID;
        rc->nstages++;
    }
    s_d_nunits = nunits;
    s_d_rp = rc_lookup(rc);
}

/* The texgen behind texture coordinate tc. A bump texgen (GX_TG_BUMPn, the
 * emboss half of HSD's bump mapping) is an earlier coordinate shifted
 * toward light n along the binormal and tangent; without those it is drawn
 * as that coordinate itself, so the "+ height(tc) - height(bump)" stage
 * pair cancels instead of subtracting one texel's height everywhere (the
 * crates' black stripes). */
static const XgxTexGen* unit_texgen(const XgxState* st, uint32_t tc) {
    const XgxTexGen* tg = &st->texgen[tc < XGX_MAX_TEXGEN ? tc : 0];
    if (tg->type >= GX_TG_BUMP0 && tg->type <= GX_TG_BUMP7 && tg->src >= GX_TG_TEXCOORD0 &&
        tg->src <= GX_TG_TEXCOORD6 && tg->src - GX_TG_TEXCOORD0 < tc) {
        const XgxTexGen* base = &st->texgen[tg->src - GX_TG_TEXCOORD0];
        if (base->type == GX_TG_MTX3x4 || base->type == GX_TG_MTX2x4) return base;
    }
    return tg;
}

static void derive_vk(const XgxState* st, const XgxLayout* layout) {
    VpKey* vk = &s_d_vk;
    int i;
    memset(vk, 0, sizeof *vk);
    s_d_spec_lights = 0;
    s_d_tg_posmtx = 0;
    vk->has_nrm = layout->off_nrm >= 0;
    vk->nchans = (uint8_t)(st->nchans > 2 ? 2 : st->nchans);
    for (i = 0; i < 4; i++) {
        const XgxChan* c = &st->chan[i];
        vk->chan[i].enable = (uint8_t)(c->enable != 0);
        vk->chan[i].amb_vtx = (uint8_t)(c->amb_src != 0 && layout->off_col[i / 2] >= 0);
        vk->chan[i].mat_vtx = (uint8_t)(c->mat_src != 0 && layout->off_col[i / 2] >= 0);
        vk->chan[i].diff_fn = (uint8_t)c->diff_fn;
        vk->chan[i].attn = vp_attn(c->attn_fn);
        vk->chan[i].light_mask = c->enable ? (uint8_t)c->light_mask : 0;
        if (c->enable && vk->chan[i].attn == VPL_SPEC) s_d_spec_lights |= c->light_mask;
        /* vertex colour sources without a vertex colour: GX reads zero */
        if (c->amb_src && layout->off_col[i / 2] < 0) vk->chan[i].amb_vtx = 1;
        if (c->mat_src && layout->off_col[i / 2] < 0) vk->chan[i].mat_vtx = 1;
    }
    vk->fog = fog_kind(st->fog_type);
    vk->ntex = (uint8_t)s_d_nunits;
    for (i = 0; i < s_d_nunits; i++) {
        const XgxTexGen* tg = unit_texgen(st, (uint32_t)s_d_unit_tc[i]);
        vk->tex[i].src = (uint8_t)tg->src;
        vk->tex[i].proj = tg->type == GX_TG_MTX3x4;
        if (vk->tex[i].proj && s_d_unit_kind[i] == UNIT_BUMP) vk->tex[i].proj = VP_PROJ_DIVIDE;
        vk->tex[i].normalize = (uint8_t)(tg->normalize != 0);
        if (tg->mtx < GX_TEXMTX0) s_d_tg_posmtx = 1;
    }
    /* attenuation rows (build_lights) that make a spot light's factor 1:
     * HSD's infinite lights (a and k (1, 0, 0)) and point lights (a) */
    for (i = 0; i < XGX_MAX_LIGHTS && i < 8; i++) {
        const XgxLight* l = &st->light[i];
        if (l->a[0] == 1.0f && l->a[1] == 0.0f && l->a[2] == 0.0f) vk->ang_one |= (uint8_t)(1u << i);
        if (l->k[0] == 1.0f && l->k[1] == 0.0f && l->k[2] == 0.0f) vk->dist_one |= (uint8_t)(1u << i);
    }
    vp_canon(vk);   /* configurations with the same program compare equal */
}

/* GX fog changed (or the GPU state was reset): the fog rows, the vertex
 * program's fog variant, the fog unit and the final combiner. The NV2A's fog
 * unit runs LINEAR with FOG_PARAMS (1, 1, 0) (the linear mode subtracts 1:
 * factor = p0 + p1 * oFog.x - 1), so its factor is the vertex program's F;
 * the gen mode doesn't matter with a vertex program (nxdk_pgraph_tests). */
static void emit_fog(const XgxState* st) {
    s_fog_force = 0;
    build_fog(st);
    emit_vc();   /* the fog rows: this draw's emit_vc has run */
    SETF(18, NV097_SET_FOG_ENABLE, s_fog.kind != VPF_OFF);
    if (s_fog.kind) {
        SETF(19, NV097_SET_FOG_COLOR, fog_color_abgr(st->fog_color));
        if (s_fixed[20] != 1) {
            put1(NV097_SET_FOG_MODE, NV097_SET_FOG_MODE_V_LINEAR);
            put1(NV097_SET_FOG_GEN_MODE, NV097_SET_FOG_GEN_MODE_V_SPEC_ALPHA);
            putf(NV097_SET_FOG_PARAMS, 1.0f);
            putf(NV097_SET_FOG_PARAMS + 4, 1.0f);
            putf(NV097_SET_FOG_PARAMS + 8, 0.0f);
            s_fixed[20] = 1;
        }
    }
    if (s_d_vk.fog != s_fog.kind) {   /* derive_vk runs only for its own groups */
        s_d_vk.fog = s_fog.kind;
        vp_select(&s_d_vk);
    }
    /* the combiner program on the GPU: a new one (DIRTY_UNITS) is sent after
     * this with final_cw0 anyway */
    if (s_rc_valid && final_cw0(s_rc_sent) != s_rc_cw0) {
        s_rc_cw0 = final_cw0(s_rc_sent);
        put1(NV097_SET_COMBINER_SPECULAR_FOG_CW0, s_rc_cw0);
    }
}

void xgx_draw(uint32_t prim, uint32_t count, const XgxLayout* layout, XgxState* st) {
    int i, pf;
    uint32_t first, d, lay;

    if (!count) return;
    pf = xhw_perf_enter(XHW_PERF_DRAW);
    s_st_verts += count;
    s_pf_verts += count;
    frame_open();
    pb_budget();
    pb_open();

    d = st->dirty | s_draw_force | (s_vc_valid ? 0 : XGX_DIRTY_ALL);
    {   /* what changed before each draw: what keeps draws from merging */
        uint32_t bits = d & 0x1FFFu;
        if (!d) s_st_dirty_none++;
        else if (d == XGX_DIRTY_POSMTX) s_st_dirty_mtx++;
        for (; bits; bits &= bits - 1) s_st_dirty[__builtin_ctz(bits)]++;
    }
    s_draw_force = 0;
    lay = (layout->off_nrm >= 0) | (layout->off_col[0] >= 0) << 1 | (layout->off_col[1] >= 0) << 2;
    if (d & DIRTY_UNITS) derive_units(st);
    if ((d & DIRTY_VK) || lay != s_d_layout) {
        VpKey old = s_d_vk;
        derive_vk(st, layout);
        s_d_layout = lay;
        if (memcmp(&old, &s_d_vk, sizeof old) != 0) vp_select(&s_d_vk);
    }
    if (s_vp_cur < 0) vp_select(&s_d_vk);
    s_approx += (uint32_t)s_d_unit_miss + (s_d_rp->approximated ? 1u : 0u);
    if (s_d_rp->swizzled) {
        s_st_swz++;
        s_st_swz_stages += (uint32_t)s_d_rp->swizzled;
    }
    if (s_d_nbump) s_st_ind++;
    else if (st->nind && s_d_unit_miss) s_st_ind_approx++;
#ifdef XGX_DEBUG_TRACE
    if (s_fbdump_once || s_shot_once) trace_draw(prim, count, st, s_d_rp->approximated, layout);   /* autopad SHOT or BACK */
#endif
    if ((d & DIRTY_TG) || (s_d_tg_posmtx && (d & XGX_DIRTY_POSMTX)))
        for (i = 0; i < s_d_nunits; i++)
            build_texgen(st, i, unit_texgen(st, (uint32_t)s_d_unit_tc[i]));

    /* constants */
    if (d & (XGX_DIRTY_PROJ | XGX_DIRTY_VIEWPORT)) build_proj(st);
    if (d & (XGX_DIRTY_POSMTX | XGX_DIRTY_TEXMTX)) build_mtx(st);
    if (d & XGX_DIRTY_CHANS) build_chans(st);
    if (d & (XGX_DIRTY_LIGHTS | XGX_DIRTY_CHANS)) build_lights(st, s_d_spec_lights);
    emit_vc();

    if (d & DIRTY_FIXED) emit_fixed(st);
    if (d & DIRTY_UNITS) emit_textures(st, s_d_unit_map, s_d_nunits);
    if (d & (DIRTY_UNITS | XGX_DIRTY_TEVREG)) emit_combiners(st, s_d_rp);

    /* The vertices are at s_draw_base (xgx_vtx_alloc or xgx_vtx_use). The
     * arrays point at the start of the ring or of the vertex pool, and the
     * draw starts at the vertex's index from there (both place vertices at
     * a multiple of the stride): consecutive draws of one layout then send
     * no array offsets in between. xemu joins such back-to-back
     * BEGIN/DRAW_ARRAYS/END runs into one draw, and each xemu draw costs a
     * geometry-shader pass that macOS's GL runs as a compute pass.
     * The console takes vertex indices up to 0xFFFF only (a DRAW_ARRAYS
     * start past that raised a PGRAPH data error per draw, ~460 a frame,
     * and froze it), so the arrays point at the start of the 32768-vertex
     * window the draw starts in. */
    s_attr_base = s_draw_base;
    first = 0;
    {
        const uint8_t* region = s_draw_base >= s_ring && s_draw_base < s_ring + RING_BYTES ? s_ring
                                : pool_owns(&s_vb, s_draw_base)                             ? s_vb.base
                                                                                            : NULL;
        uint32_t rel = region ? (uint32_t)(s_draw_base - region) : 0;
        if (region && rel % layout->stride == 0) {
            uint32_t idx = rel / layout->stride, win = idx & ~(VTX_WINDOW - 1);
            if (idx - win + count <= 0x10000u) {
                s_attr_base = region + win * layout->stride;
                first = idx - win;
            }
        }
    }
    emit_vertex_arrays(layout, st);
#ifdef XGX_DEBUG_TRACE
    if ((s_fbdump_once || s_shot_once) && trace_skip(s_draws)) count = 0;
    if (count)
#endif
    put1(NV097_SET_BEGIN_END, nv_prim(prim));
    while (count > 0) {
        uint32_t batch = count > 256 * 64 ? 256 * 64 : count, k, words = 0;
        uint32_t* hdr = P++;
        for (k = 0; k < batch; k += 256) {
            uint32_t n = batch - k > 256 ? 256 : batch - k;
            *P++ = ((n - 1) << 24) | (first + k);
            words++;
        }
        *hdr = words << 18 | NV2A_SUPPRESS_COMMAND_INCREMENT(NV097_DRAW_ARRAYS);
        first += batch;
        count -= batch;
    }
#ifdef XGX_DEBUG_TRACE
    if (!(s_fbdump_once || s_shot_once) || !trace_skip(s_draws))
#endif
    put1(NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    if (P - s_pb_mark >= PB_KICK) pb_close();
    s_draws++;
    s_st_draws++;
    s_st_prim[(prim >> 3) & 7]++;
    xhw_perf_leave(pf);
}

/* ======================================================================
 * EFB -> texture / memory
 * ====================================================================== */
static void read_rect_cpu(const int32_t src[4], uint32_t dw, uint32_t dh, uint32_t* argb) {
    static int32_t col[1024];
    const uint8_t* fb;
    uint32_t pitch, x, y;
    s_st_efb++;
    frame_open();
    wait_idle();
    pb_open();
    /* write-combined memory: every read here is an uncached bus cycle */
    fb = (const uint8_t*)pb_back_buffer();
    pitch = pb_back_buffer_pitch();
    if (dw > 1024) dw = 1024;
    /* the column map is the same for every row: once per copy, not per pixel */
    for (x = 0; x < dw; x++) {
        int fx = pix_x(src[0] + (x + 0.5f) * (float)src[2] / (float)dw);
        col[x] = fx >= s_fbw ? s_fbw - 1 : fx < 0 ? 0 : fx;
    }
    for (y = 0; y < dh; y++) {
        int fy = pix_y(src[1] + (y + 0.5f) * (float)src[3] / (float)dh);
        const uint8_t* row;
        uint32_t* out = argb + y * dw;
        if (fy >= s_fbh) fy = s_fbh - 1;
        if (fy < 0) fy = 0;
        row = fb + (size_t)fy * pitch;
        if (s_bpp == 16) {
            const uint16_t* r16 = (const uint16_t*)row;
            for (x = 0; x < dw; x++) {
                uint32_t v = r16[col[x]];
                uint32_t r = v >> 11, g = (v >> 5) & 63, b = v & 31;
                out[x] = 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
            }
        } else {
            const uint32_t* r32 = (const uint32_t*)row;
            for (x = 0; x < dw; x++) out[x] = r32[col[x]];
        }
    }
}

static void read_rect(const int32_t src[4], uint32_t dw, uint32_t dh, uint32_t* argb) {
    int pf = xhw_perf_enter(XHW_PERF_EFB);
    read_rect_cpu(src, dw, dh, argb);
    xhw_perf_leave(pf);
}

#if XGX_EFB_GPU_COPY
/* EFB -> texture on the GPU: the back buffer, bound as a linear texture, is
 * drawn with one quad into the swizzled texture as the render target. The
 * CPU readback cost ~8 ms per 256x256 shadow map on the console: the
 * framebuffer is write-combined, so every read is an uncached bus cycle,
 * and the copy waited for the GPU first. Here nothing waits; the GPU runs
 * the copy between the draws before it and the draws that sample it.
 *
 * The combiners keep the channel the copy format stores (XGX_COPY_*), as
 * the CPU path below does. Afterwards the back buffer is the target again
 * and xgx_draw re-sends every state group. The target has the back buffer's
 * format: A8R8G8B8 with Z24S8 surfaces, or R5G6B5 with Z16 at 16 bits
 * (720p), as the NV2A wants colour and depth surfaces of the same width
 * even with depth off. An R5G6B5 copy samples alpha 1: the I/R copies HSD
 * makes (shadow maps) are read for colour only, alpha comes from APREV
 * (tobj.c, TObjSetupTevModulateShadow). At 720p this was the CPU readback,
 * ~60 ms a match frame on the console (v38). */
enum { CR_ZERO = 0, CR_C0 = 1, CR_T0 = 8, CR_R0 = 12 };
#define CR_IN(reg, alpha, inv) ((uint32_t)(reg) | (uint32_t)(alpha) << 4 | (uint32_t)(inv) << 5)
#define CR_ONE CR_IN(CR_ZERO, 0, 1)
#define CR_AB_TO(reg) ((uint32_t)(reg) << 4)   /* output control word: AB -> reg */
#define CR_AB_DOT (1u << 13)

static void copy_combiners(int mode) {
    uint32_t cicw[2] = { 0, 0 }, cocw[2] = { 0, 0 }, aicw[2] = { 0, 0 }, aocw[2] = { 0, 0 }, k = 0;
    int n = 1, i;
    switch (mode) {
        case XGX_COPY_LUMA: case XGX_COPY_LUMA_ALPHA: k = 0x004D961Du; break;   /* 77, 150, 29 / 255 */
        case XGX_COPY_RED: case XGX_COPY_RED_ALPHA: k = 0x00FF0000u; break;
        case XGX_COPY_GREEN: k = 0x0000FF00u; break;
        case XGX_COPY_BLUE: k = 0x000000FFu; break;
        default: break;
    }
    if (k) {
        /* rgb = t0 . k in every channel */
        cicw[0] = CR_IN(CR_T0, 0, 0) << 24 | CR_IN(CR_C0, 0, 0) << 16;
        cocw[0] = CR_AB_TO(CR_R0) | CR_AB_DOT;
        if (mode == XGX_COPY_LUMA_ALPHA || mode == XGX_COPY_RED_ALPHA) {
            aicw[0] = CR_IN(CR_T0, 1, 0) << 24 | CR_ONE << 16;
            aocw[0] = CR_AB_TO(CR_R0);
        } else {
            /* alpha = the same value: R0's blue, in a second stage */
            aicw[1] = CR_IN(CR_R0, 0, 0) << 24 | CR_ONE << 16;
            aocw[1] = CR_AB_TO(CR_R0);
            n = 2;
        }
    } else {
        /* colour as is, or alpha in every channel */
        cicw[0] = CR_IN(CR_T0, mode == XGX_COPY_ALPHA, 0) << 24 | CR_ONE << 16;
        cocw[0] = CR_AB_TO(CR_R0);
        aicw[0] = CR_IN(CR_T0, 1, 0) << 24 | CR_ONE << 16;
        aocw[0] = CR_AB_TO(CR_R0);
    }
    put1(NV097_SET_COMBINER_CONTROL, (uint32_t)n | (1u << 12) | (1u << 16));
    for (i = 0; i < n; i++) {
        put1(NV097_SET_COMBINER_COLOR_ICW + i * 4, cicw[i]);
        put1(NV097_SET_COMBINER_COLOR_OCW + i * 4, cocw[i]);
        put1(NV097_SET_COMBINER_ALPHA_ICW + i * 4, aicw[i]);
        put1(NV097_SET_COMBINER_ALPHA_OCW + i * 4, aocw[i]);
    }
    put1(NV097_SET_COMBINER_FACTOR0, k);
    put1(NV097_SET_COMBINER_SPECULAR_FOG_CW0, (uint32_t)CR_R0 << 8);   /* out = R0 */
    put1(NV097_SET_COMBINER_SPECULAR_FOG_CW1, (uint32_t)(CR_R0 | 1 << 4) << 8 | 0x80);
}

static void efb_copy_gpu(const int32_t src[4], const Tex* t, int mode) {
    static const VpKey k_copy = { .copy = 1 };
    uint32_t pw = t->w, ph = t->h, fb = (uint32_t)pb_back_buffer() & 0x03FFFFFF, i, bytes = (uint32_t)s_bpp / 8;
    float u0 = s_cx + src[0] * (float)s_cw / XGX_EFB_W, u1 = u0 + src[2] * (float)s_cw / XGX_EFB_W;
    float v0 = s_cy + src[1] * (float)s_ch / XGX_EFB_H, v1 = v0 + src[3] * (float)s_ch / XGX_EFB_H;
    /* nearest, unless the copy is smaller than its source (copy_dim) */
    uint32_t filt = (float)pw < u1 - u0 - 0.5f || (float)ph < v1 - v0 - 0.5f ? 2 : 1;
    float* v;
    int pf = xhw_perf_enter(XHW_PERF_EFB);
    s_st_efb++;
    frame_open();
    pb_budget();
    s_last_copy[0] = (uint32_t)t->mem & 0x03FFFFFF;
    s_last_copy[1] = pw;
    s_last_copy[2] = ph;
    s_last_copy[3] = s_frame;
    v = (float*)xgx_vtx_alloc(4, 20);
    if (!v) {
        xhw_perf_leave(pf);
        return;
    }
    /* x y z u v: the texture's corners and the source rect's, in texels.
     * A target pixel's centre then samples the centre of the source pixel
     * under it. (Until v40 both corners had +0.5, and read_rect_cpu rounded
     * the same way: each texel read the next pixel along. The scissor's
     * old extra column covered that; with the scissor exact, the copy's
     * last row and column read black past the shadow map's background.) */
    {
        const float q[4][5] = { { 0, 0, 1, u0, v0 }, { (float)pw, 0, 1, u1, v0 },
                                { 0, (float)ph, 1, u0, v1 }, { (float)pw, (float)ph, 1, u1, v1 } };
        memcpy(v, q, sizeof q);
    }
    pb_open();
    put1(NV097_WAIT_FOR_IDLE, 0);   /* the source's pixels are in memory */

    /* target: the texture, through pbkit's DMA object over all of RAM (3) */
    put1(NV097_SET_CONTEXT_DMA_COLOR, 3);
    put1(NV097_SET_SURFACE_FORMAT, (s_bpp == 16 ? NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5 |
                                                      NV097_SET_SURFACE_FORMAT_ZETA_Z16 << 4
                                                : NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8 |
                                                      NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 << 4) |
                                       NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE << 8 |
                                       (uint32_t)log2i((int)pw) << 16 | (uint32_t)log2i((int)ph) << 24);
    put1(NV097_SET_SURFACE_PITCH, pw * bytes | (pw * bytes) << 16);
    put1(NV097_SET_SURFACE_COLOR_OFFSET, (uint32_t)t->mem & 0x03FFFFFF);
    put1(NV097_SET_SURFACE_CLIP_HORIZONTAL, pw << 16);
    put1(NV097_SET_SURFACE_CLIP_VERTICAL, ph << 16);
    /* the window clip's max is inclusive (xemu adds 1 too): pw would let
     * column pw through, past the end of a swizzled target */
    put1(NV097_SET_WINDOW_CLIP_HORIZONTAL, (pw - 1) << 16);
    put1(NV097_SET_WINDOW_CLIP_VERTICAL, (ph - 1) << 16);

    /* pixel state: write every channel, test nothing (no depth access) */
    put1(NV097_SET_DEPTH_TEST_ENABLE, 0);
    put1(NV097_SET_DEPTH_MASK, 0);
    put1(NV097_SET_STENCIL_TEST_ENABLE, 0);
    put1(NV097_SET_ALPHA_TEST_ENABLE, 0);
    put1(NV097_SET_BLEND_ENABLE, 0);
    put1(NV097_SET_LOGIC_OP_ENABLE, 0);
    put1(NV097_SET_CULL_FACE_ENABLE, 0);
    put1(NV097_SET_DITHER_ENABLE, 0);
    put1(NV097_SET_COLOR_MASK, NV097_SET_COLOR_MASK_RED_WRITE_ENABLE | NV097_SET_COLOR_MASK_GREEN_WRITE_ENABLE |
                                   NV097_SET_COLOR_MASK_BLUE_WRITE_ENABLE | NV097_SET_COLOR_MASK_ALPHA_WRITE_ENABLE);

    /* source: the back buffer as a linear texture, texel coordinates, filt */
    put1(NV097_SET_TEXTURE_OFFSET, fb);
    put1(NV097_SET_TEXTURE_FORMAT, 1 | (1u << 3) | (2u << 4) |
                                       (uint32_t)(s_bpp == 16 ? NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5
                                                              : NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8)
                                           << 8 |
                                       (1u << 16));
    put1(NV097_SET_TEXTURE_ADDRESS, 3 | (3u << 8) | (3u << 16));
    put1(NV097_SET_TEXTURE_CONTROL0, 0x4003FFC0u);
    put1(NV097_SET_TEXTURE_CONTROL1, pb_back_buffer_pitch() << 16);
    put1(NV097_SET_TEXTURE_FILTER, (filt << 16) | (filt << 24) | 0x2000u);
    put1(NV097_SET_TEXTURE_IMAGE_RECT, (uint32_t)s_fbw << 16 | (uint32_t)s_fbh);
    for (i = 1; i < 4; i++) put1(NV097_SET_TEXTURE_CONTROL0 + i * 64, 0);
    put1(NV097_SET_SHADER_STAGE_PROGRAM, 1);   /* unit 0: 2D_PROJECTIVE */
    copy_combiners(mode);
    vp_select(&k_copy);

    s_attr_base = s_draw_base;
    attr(VPI_POS, 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3, 20);
    attr(VPI_MTX, -1, 0, 0, 0);
    attr(VPI_NRM, -1, 0, 0, 0);
    attr(VPI_COL0, -1, 0, 0, 0);
    attr(VPI_COL1, -1, 0, 0, 0);
    for (i = 0; i < 8; i++) attr(vpi_tex((int)i), i ? -1 : 12, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, 20);
    /* pb_open only breaks the vertex cache at a batch's start, and this one
     * may already be open: a stale read of these four vertices puts the quad
     * anywhere, and a pixel outside the swizzled target faults (LIMIT_COLOR,
     * three console stalls on this quad's END) */
    if (XGX_VB_CACHE_BREAK) put1(NV097_BREAK_VERTEX_BUFFER_CACHE, 0);
    put1(NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_TRIANGLE_STRIP);   /* not a quad: see gx_vtx.c out_prim */
    *P++ = 1u << 18 | NV2A_SUPPRESS_COMMAND_INCREMENT(NV097_DRAW_ARRAYS);
    *P++ = 3u << 24;   /* 4 vertices from 0 */
    put1(NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    put1(NV097_WAIT_FOR_IDLE, 0);   /* the copy is in memory before anything samples it */

    /* back to the back buffer and the game's state */
    put1(NV097_SET_STENCIL_TEST_ENABLE, 1);   /* as pbkit leaves it */
    pb_close();
    ocx_pb_retarget_back_buffer();
    {   /* the back buffer is the target before the next clear or draw starts */
        uint32_t* p = pb_begin();
        p = pb_push1(p, NV097_WAIT_FOR_IDLE, 0);
        pb_end(p);
    }
    memset(s_fixed, 0xFF, sizeof s_fixed);
    memset(s_tex_shadow, 0xFF, sizeof s_tex_shadow);
    s_tex_prog = 0xFFFFFFFFu;
    s_rc_valid = 0;
    s_vp_cur = -1;
    s_draw_force = XGX_DIRTY_ALL;
    xhw_perf_leave(pf);
}
#endif

/* GXSetZTexture emulation, the copy side. A Z-texture draw replaces each
 * pixel's depth with the texel and compares it with the depth buffer: the
 * Classic team card copies each fighter's depth and draws the tiles back
 * against a plane primed in front of the cleared background, so only the
 * fighter shows. The NV2A can't replace depth from a texture, and its
 * depth buffer is compressed (pbkit's tile 1), so it can't be sampled
 * either. The depth test itself makes the mask instead: alpha 0 over the
 * rect, then a quad just in front of z24 that writes alpha 1 where the
 * stored depth is nearer still (GREATER). The copy that follows takes that
 * alpha (XGX_COPY_ALPHA); the draw multiplies its alpha by it and drops
 * the 0s (derive_units, emit_fixed). */
int xgx_ztex_mask(const int32_t src[4], uint32_t z24) {
#if XGX_EFB_GPU_COPY
    static const VpKey k_mask = { .copy = 1 };
    int x0, y0, x1, y1, i;
    float z, *v;
    if (s_bpp != 32) return 0;   /* R5G6B5 has no alpha */
    x0 = map_x((float)src[0]);
    y0 = map_y((float)src[1]);
    x1 = map_x((float)(src[0] + src[2]));
    y1 = map_y((float)(src[1] + src[3]));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s_fbw) x1 = s_fbw;
    if (y1 > s_fbh) y1 = s_fbh;
    if (x1 <= x0 || y1 <= y0) return 0;
    frame_open();
    pb_budget();
    v = (float*)xgx_vtx_alloc(4, 20);
    if (!v) return 0;
    /* a little in front of z24: the cleared background holds exactly z24,
     * and the quad's interpolated depth there must not round past it */
    z24 &= 0xFFFFFFu;
    z = (float)(z24 > 0x100u ? z24 - 0x100u : 0u) * (s_zmax / 16777215.0f);
    {
        const float q[4][5] = { { (float)x0, (float)y0, z, 0, 0 }, { (float)x1, (float)y0, z, 0, 0 },
                                { (float)x0, (float)y1, z, 0, 0 }, { (float)x1, (float)y1, z, 0, 0 } };
        memcpy(v, q, sizeof q);
    }
    pb_open();
    put1(NV097_SET_DEPTH_MASK, 0);
    put1(NV097_SET_STENCIL_TEST_ENABLE, 0);
    put1(NV097_SET_ALPHA_TEST_ENABLE, 0);
    put1(NV097_SET_BLEND_ENABLE, 0);
    put1(NV097_SET_LOGIC_OP_ENABLE, 0);
    put1(NV097_SET_CULL_FACE_ENABLE, 0);
    put1(NV097_SET_COLOR_MASK, NV097_SET_COLOR_MASK_ALPHA_WRITE_ENABLE);
    put1(NV097_SET_WINDOW_CLIP_HORIZONTAL, (uint32_t)(s_fbw - 1) << 16);   /* max inclusive */
    put1(NV097_SET_WINDOW_CLIP_VERTICAL, (uint32_t)(s_fbh - 1) << 16);
    for (i = 0; i < 4; i++) put1(NV097_SET_TEXTURE_CONTROL0 + i * 64, 0);
    put1(NV097_SET_SHADER_STAGE_PROGRAM, 0);
    put1(NV097_SET_COMBINER_CONTROL, 1u | (1u << 12) | (1u << 16));
    put1(NV097_SET_COMBINER_COLOR_ICW, CR_ONE << 24 | CR_ONE << 16);
    put1(NV097_SET_COMBINER_COLOR_OCW, CR_AB_TO(CR_R0));
    put1(NV097_SET_COMBINER_SPECULAR_FOG_CW0, (uint32_t)CR_R0 << 8);   /* out = R0 */
    put1(NV097_SET_COMBINER_SPECULAR_FOG_CW1, (uint32_t)(CR_R0 | 1 << 4) << 8 | 0x80);
    vp_select(&k_mask);
    s_attr_base = s_draw_base;
    attr(VPI_POS, 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3, 20);
    attr(VPI_MTX, -1, 0, 0, 0);
    attr(VPI_NRM, -1, 0, 0, 0);
    attr(VPI_COL0, -1, 0, 0, 0);
    attr(VPI_COL1, -1, 0, 0, 0);
    for (i = 0; i < 8; i++) attr(vpi_tex(i), i ? -1 : 12, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, 20);
    if (XGX_VB_CACHE_BREAK) put1(NV097_BREAK_VERTEX_BUFFER_CACHE, 0);   /* as efb_copy_gpu */
    /* three passes over the rect. 0: depth writes on but nothing passes
     * (NEVER), which changes nothing; xemu only binds its depth buffer for
     * a draw that may write depth, and the EFB copy just before (another
     * surface shape) had unbound it, so the GREATER below passed
     * everywhere. 1: alpha 0. 2: alpha 1 where the depth buffer is nearer
     * than z (GREATER: the quad is behind what was drawn). */
    for (i = 0; i < 3; i++) {
        put1(NV097_SET_DEPTH_TEST_ENABLE, i != 1);
        put1(NV097_SET_DEPTH_FUNC, i ? 0x204 : 0x200);
        put1(NV097_SET_DEPTH_MASK, i == 0);
        put1(NV097_SET_COMBINER_ALPHA_ICW, (i == 2 ? CR_ONE : CR_ZERO) << 24 | CR_ONE << 16);
        put1(NV097_SET_COMBINER_ALPHA_OCW, CR_AB_TO(CR_R0));
        put1(NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_TRIANGLE_STRIP);
        *P++ = 1u << 18 | NV2A_SUPPRESS_COMMAND_INCREMENT(NV097_DRAW_ARRAYS);
        *P++ = 3u << 24;   /* 4 vertices from 0 */
        put1(NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    }
    put1(NV097_WAIT_FOR_IDLE, 0);   /* the alpha is in the framebuffer before the copy reads it */
    put1(NV097_SET_STENCIL_TEST_ENABLE, 1);   /* as pbkit leaves it */

    /* the game's state goes back at the next draw */
    memset(s_fixed, 0xFF, sizeof s_fixed);
    memset(s_tex_shadow, 0xFF, sizeof s_tex_shadow);
    s_tex_prog = 0xFFFFFFFFu;
    s_rc_valid = 0;
    s_vp_cur = -1;
    s_draw_force = XGX_DIRTY_ALL;
    return 1;
#else
    (void)src;
    (void)z24;
    return 0;
#endif
}

/* An EFB copy's texture side: the nearest power of two, not the next one
 * (the copy is resampled to it and sampled with normalized coordinates, so
 * any size works). Pokémon Stadium's screen copies 640x406 every few frames:
 * rounded up that was 1024x512 ARGB8, 2 MB of the texture pool, and the
 * stage's working set no longer fit (uploads and evictions every frame). */
static uint32_t copy_dim(uint32_t v) {
    uint32_t p = (uint32_t)pot((int)v);
    return p > 1 && p - v > v - p / 2 ? p / 2 : p;
}

uint32_t xgx_tex_from_efb(const int32_t src[4], uint32_t dst_w, uint32_t dst_h, int mode, uint32_t reuse) {
    static uint32_t* buf;
    static uint32_t buf_texels;
    uint32_t pw, ph, n, i;
    int reusable;
    if (!dst_w || !dst_h || dst_w > 1024 || dst_h > 1024) return 0;
#ifdef XGX_DEBUG_NOEFB
    return 0;
#endif
    pw = copy_dim(dst_w);
    ph = copy_dim(dst_h);
#ifdef XGX_DEBUG_TRACE
    /* the frame a BACK screenshot captures: each EFB copy's source as a BMP
     * too (up to 8), logged between the draws that made it and use it */
    if (s_shot_once) {
        /* the copy's size, freed after: a kept 4 MB buffer took the console
         * down to 2 MB free for the rest of the session (v27) */
        uint32_t* dbg;
        static uint32_t dbg_frame = 0xFFFFFFFFu, dbg_n;
        if (dbg_frame != s_frame) dbg_frame = s_frame, dbg_n = 0;
        if (dbg_n++ < 8 && (dbg = (uint32_t*)malloc((size_t)pw * ph * 4)) != NULL) {
            xhw_logf("[DRAW] efb copy after #%u: src %d,%d %dx%d -> %ux%u mode %d", s_draws, (int)src[0], (int)src[1],
                     (int)src[2], (int)src[3], pw, ph, mode);
            read_rect(src, pw, ph, dbg);
            xhw_fbdump_file(dbg, (int)pw, (int)ph, 32, (int)pw * 4);
#ifdef XHW_AUTOPAD
            /* xemu: the HDD's files are out of reach, COM1 isn't */
            xhw_fbdump(dbg, (int)pw, (int)ph, 32, (int)pw * 4);
#endif
            free(dbg);
        }
    }
#endif
#if XGX_EFB_GPU_COPY
    {   /* the target has the back buffer's format (efb_copy_gpu) */
        uint32_t cfmt = s_bpp == 32 ? XGX_TEX_ARGB8 : XGX_TEX_RGB565, tex;
        reusable = reuse && reuse < MAX_TEX && s_tex[reuse].used && s_tex[reuse].w == pw && s_tex[reuse].h == ph &&
                   s_tex[reuse].levels == 1 && !s_tex[reuse].rect && s_tex[reuse].nvfmt == nv_format(cfmt);
        tex = reusable ? reuse : xgx_tex_create(pw, ph, 1, cfmt, NULL);
        if (tex) efb_copy_gpu(src, &s_tex[tex], mode);
        return tex;
    }
#endif
    reusable = reuse && reuse < MAX_TEX && s_tex[reuse].used && s_tex[reuse].w == pw && s_tex[reuse].h == ph &&
               s_tex[reuse].levels == 1 && !s_tex[reuse].rect && s_tex[reuse].nvfmt == nv_format(XGX_TEX_ARGB8);
    n = pw * ph;
    if (n > buf_texels) {
        free(buf);
        buf = (uint32_t*)malloc(n * 4);
        buf_texels = buf ? n : 0;
        if (!buf) return 0;
    }
    read_rect(src, pw, ph, buf);   /* waits for the GPU: nothing is reading `reuse` now */
    if (mode != XGX_COPY_COLOR)
        for (i = 0; i < n; i++) {
            uint32_t c = buf[i], a = c >> 24, r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF, v;
            switch (mode) {
                case XGX_COPY_LUMA: case XGX_COPY_LUMA_ALPHA: v = (r * 77 + g * 150 + b * 29) >> 8; break;
                case XGX_COPY_RED: case XGX_COPY_RED_ALPHA: v = r; break;
                case XGX_COPY_GREEN: v = g; break;
                case XGX_COPY_BLUE: v = b; break;
                default: v = a; break;
            }
            if (mode != XGX_COPY_LUMA_ALPHA && mode != XGX_COPY_RED_ALPHA) a = v;
            buf[i] = a << 24 | v << 16 | v << 8 | v;
        }
    if (reusable) {
        write_level(s_tex[reuse].mem, buf, (int)pw, (int)ph, (int)pw, (int)ph, 4);
        return reuse;
    }
    return xgx_tex_create(pw, ph, 1, XGX_TEX_ARGB8, buf);
}

void xgx_read_efb(const int32_t src[4], uint32_t dst_w, uint32_t dst_h, uint8_t* rgba) {
    uint32_t i, n = dst_w * dst_h;
    uint32_t* buf = (uint32_t*)malloc(n * 4);
    if (!buf) return;
    read_rect(src, dst_w, dst_h, buf);
    for (i = 0; i < n; i++) {
        rgba[i * 4] = (uint8_t)(buf[i] >> 16);
        rgba[i * 4 + 1] = (uint8_t)(buf[i] >> 8);
        rgba[i * 4 + 2] = (uint8_t)buf[i];
        rgba[i * 4 + 3] = (uint8_t)(buf[i] >> 24);
    }
    free(buf);
}

/* ======================================================================
 * Init
 * ====================================================================== */
static void setup_state(void) {
    uint32_t* p = pb_begin();
    p = pb_push1(p, NV097_SET_CONTROL0, CONTROL0);
    p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);
    /* oD1 carries colour channel 1; without SPECULAR_ENABLE the NV2A replaces
     * it with (0,0,0,1) (OpenCrossing traps.md) */
    p = pb_push1(p, NV097_SET_SPECULAR_ENABLE, 1);
    p = pb_push1(p, NV097_SET_LIGHT_CONTROL,
                 NV097_SET_LIGHT_CONTROL_V_SEPARATE_SPECULAR | NV097_SET_LIGHT_CONTROL_V_ALPHA_FROM_MATERIAL_SPECULAR);
    p = pb_push1(p, NV097_SET_FOG_ENABLE, 0);
    p = pb_push1(p, NV097_SET_SKIN_MODE, NV097_SET_SKIN_MODE_OFF);
    p = pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT, 0);
    /* CW: GX's front faces, as seen after the viewport y-flip this renderer
     * folds into the projection. (OpenCrossing's GL shim flips differently and
     * uses CCW.) Checked in xemu against Dolphin: with CCW the memory card
     * screen's panels, which are drawn with GX_CULL_BACK, vanish. */
    p = pb_push1(p, NV097_SET_FRONT_FACE, NV097_SET_FRONT_FACE_V_CW);
    p = pb_push1(p, NV097_SET_WINDOW_CLIP_TYPE, 0);
    /* Depth outside [CLIP_MIN, CLIP_MAX] is clamped, as the GameCube's 24-bit
     * depth is. Culling those pixels (CULL_NEAR_FAR) left black bands across
     * Pokémon Stadium's floor once the z-buffer was really in use (see
     * CONTROL0); -DXGX_DEPTH_CULL=1 brings culling back. Behind-the-eye
     * geometry is still clipped by w. */
#if defined(XGX_DEPTH_CULL) && XGX_DEPTH_CULL
    p = pb_push1(p, NV097_SET_ZMIN_MAX_CONTROL,
                 NV097_SET_ZMIN_MAX_CONTROL_CULL_NEAR_FAR | NV097_SET_ZMIN_MAX_CONTROL_ZCLAMP_CULL);
#else
    p = pb_push1(p, NV097_SET_ZMIN_MAX_CONTROL, NV097_SET_ZMIN_MAX_CONTROL_ZCLAMP_CLAMP);
#endif
    p = pb_push1(p, NV097_SET_SHADER_CLIP_PLANE_MODE, 0);
    p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
                 NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM |
                     (NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV << 2));
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);
    pb_end(p);
    p = pb_begin();
    p = pb_push1(p, NV097_SET_CLIP_MIN, 0);
    {
        union { float f; uint32_t u; } z;
        z.f = s_zmax;
        p = pb_push1(p, NV097_SET_CLIP_MAX, z.u);
    }
    if (s_bpp == 16) p = pb_push1(p, NV097_SET_DITHER_ENABLE, 1);
    pb_end(p);
}

int xgx_init(void) {
    const xhw_video_mode* vm = xhw_video();
    uint32_t tex_pool_bytes = TEX_POOL_480, vb_pool_bytes = VB_POOL_480;
    int err;
    static int done;
    if (done) return 1;
    done = 1;
    s_vp = (VpEntry*)calloc(VP_CACHE, sizeof(VpEntry));
    vpm_init(&s_vpm);
    s_rc = (RcEntry*)calloc(RC_CACHE, sizeof(RcEntry));
    for (;;) {
        vm = xhw_video();
        if (vm->bpp == 16) {
            pb_set_color_format(NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5, false);
            pb_DepthFmt = NV097_SET_SURFACE_FORMAT_ZETA_Z16;   /* NV2x: match colour and depth widths */
            s_zmax = 65535.0f;
            tex_pool_bytes = TEX_POOL_720;
            vb_pool_bytes = VB_POOL_720;
        } else {
            pb_set_color_format(NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8, false);
            pb_DepthFmt = NV097_SET_SURFACE_FORMAT_ZETA_Z24S8;
            s_zmax = 16777215.0f;
            tex_pool_bytes = TEX_POOL_480;
            vb_pool_bytes = VB_POOL_480;
        }
        pb_size(PB_BYTES);
        err = pb_init();
        if (!err) {
            s_ring = (uint8_t*)MmAllocateContiguousMemoryEx(RING_BYTES, 0, MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
            while (s_ring && !pool_init(&s_tp, tex_pool_bytes) && tex_pool_bytes > TEX_POOL_MIN)
                tex_pool_bytes -= 1024u * 1024;
            if (s_tp.base && s_ring) break;
            pool_release(&s_tp);
            if (s_ring) MmFreeContiguousMemory(s_ring);
            s_ring = NULL;
            pb_kill();
        }
        xhw_logf("[NV2A] %dx%d start failed (pb_init %d)", vm->width, vm->height, err);
        /* 480 is the fallback, also at 16 bits (-DXHW_VIDEO_480_BPP=16) */
        if (vm->height == 480) xhw_fatal("Graphics init failed", "The NV2A could not be started.");
        xhw_video_fallback_480();
    }
    /* optional: without it display lists are decoded every call */
    while (!pool_init(&s_vb, vb_pool_bytes) && vb_pool_bytes > VB_POOL_MIN) vb_pool_bytes -= 1024u * 1024;
    if (!s_vb.base) xhw_logf("[NV2A] no memory for the %u KB vertex cache", vb_pool_bytes / 1024);
    pb_show_front_screen();
    s_fbw = (int)pb_back_buffer_width();
    s_fbh = (int)pb_back_buffer_height();
    s_bpp = vm->bpp;
    s_display_aspect = vm->widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
    update_content_rect();
    state_reset_shadows();
    memset(s_attr_shadow, 0xFF, sizeof s_attr_shadow);
    memset(s_attr_off_shadow, 0xFF, sizeof s_attr_off_shadow);
    set_row(VPC_K, 0, 1, 0.5f, 2);
    frame_open();
    setup_state();
    xhw_logf("[NV2A] up: %dx%d %d-bit, %s, tex pool %u KB", s_fbw, s_fbh, s_bpp,
             vm->widescreen ? "16:9" : "4:3", s_tp.bytes / 1024);
    {   /* physical layout: a GPU or DMA write past a buffer lands in whatever
         * sits next to it (the 480p hang overwrote the pushbuffer) */
        unsigned l[8];
        const void* xfb = XVideoGetFB();
        ocx_pb_layout(l);
        xhw_logf("[NV2A] layout: pushbuffer %08x +%x, fb %08x %08x %08x +%x, depth %08x +%x, video fb %08x",
                 l[0] & 0x03FFFFFFu, l[1], l[2] & 0x03FFFFFFu, l[3] & 0x03FFFFFFu, l[4] & 0x03FFFFFFu, l[5],
                 l[6] & 0x03FFFFFFu, l[7], (unsigned)(uintptr_t)xfb & 0x03FFFFFFu);
        xhw_logf("[NV2A] layout: vertex ring %08x +%x, tex pool %08x +%x, vertex pool %08x +%x",
                 (unsigned)(uintptr_t)s_ring & 0x03FFFFFFu, RING_BYTES, (unsigned)(uintptr_t)s_tp.base & 0x03FFFFFFu,
                 s_tp.bytes, (unsigned)(uintptr_t)s_vb.base & 0x03FFFFFFu, s_vb.bytes);
    }
    xhw_mem_log("after nv2a");
    return 1;
}
