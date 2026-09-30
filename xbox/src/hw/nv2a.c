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

#include "nv2a_rc.h"
#include "nv2a_vp.h"
#include "xgx.h"
#include "xhw.h"
#include "xhw_internal.h"

void xhw_video_fallback_480(void);
extern unsigned int pb_DepthFmt;   /* settable: tools/xbox/patch_pbkit.py */
void ocx_pb_retarget_back_buffer(void);   /* tools/xbox/patch_pbkit.py */

/* GX values used here (dolphin headers are not on the hw include path) */
enum { GX_CULL_NONE, GX_CULL_FRONT, GX_CULL_BACK, GX_CULL_ALL };
enum { GX_BM_NONE, GX_BM_BLEND, GX_BM_LOGIC, GX_BM_SUBTRACT };
enum { GX_AOP_AND, GX_AOP_OR, GX_AOP_XOR, GX_AOP_XNOR };
enum { GX_NEVER, GX_LESS, GX_EQUAL, GX_LEQUAL, GX_GREATER, GX_NEQUAL, GX_GEQUAL, GX_ALWAYS };
enum { GX_CLAMP, GX_REPEAT, GX_MIRROR };
enum { GX_TG_MTX3x4 = 0, GX_TG_MTX2x4 = 1 };
enum { GX_AF_SPEC = 0, GX_AF_SPOT = 1, GX_AF_NONE = 2 };
enum { GX_COLOR0A0 = 4, GX_COLOR1A1 = 5 };
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
 * 2 MB and rebuilt lists every frame */
#define VB_POOL_480 (4096u * 1024)
#define VB_POOL_720 (3072u * 1024)
#define VB_POOL_MIN (2048u * 1024)
/* pbkit ignores a size that isn't a power of two and keeps its 512 KB: the
 * 1.5 MB asked for before left PB_GUARD past the real end, and Pokémon
 * Stadium frames (750 draws, six EFB copies) ran off it into whatever memory
 * follows; the GPU then fetched texture data as methods and stopped */
#define PB_BYTES (1024u * 1024)
_Static_assert((PB_BYTES & (PB_BYTES - 1)) == 0 && PB_BYTES >= 64 * 1024, "pb_size takes powers of two only");
#define POOL_ALIGN 128
#define POOL_BIG (256 * 1024)

typedef struct Blk { uint32_t off, size; int free; struct Blk* next; } Blk;
typedef struct {
    uint8_t* base;
    uint32_t bytes, used;
    Blk* blocks;
} Pool;
static Pool s_tp;   /* textures */
static Pool s_vb;   /* cached display-list vertices (gx_vtx.c) */

static int pool_init(Pool* pl, uint32_t bytes) {
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

static void* pool_alloc(Pool* pl, uint32_t size) {
    Blk *b, *pick = NULL;
    size = (size + POOL_ALIGN - 1) & ~(uint32_t)(POOL_ALIGN - 1);
    for (b = pl->blocks; b; b = b->next) {
        if (!b->free || b->size < size) continue;
        pick = b;
        if (size < POOL_BIG) break;
    }
    if (!pick) return NULL;
    b = pick;
    if (b->size > size) {
        Blk* n = (Blk*)calloc(1, sizeof(Blk));
        if (!n) return NULL;
        n->free = 1;
        n->next = b->next;
        b->next = n;
        if (size < POOL_BIG) {
            n->off = b->off + size;
            n->size = b->size - size;
            b->size = size;
        } else {
            n->off = b->off + b->size - size;
            n->size = size;
            b->size -= size;
            n->free = 0;
            pl->used += size;
            return pl->base + n->off;
        }
    }
    b->free = 0;
    pl->used += size;
    return pl->base + b->off;
}

static int pool_owns(const Pool* pl, const void* p) {
    return pl->base && (const uint8_t*)p >= pl->base && (const uint8_t*)p < pl->base + pl->bytes;
}

static void pool_free(Pool* pl, void* p) {
    Blk* b;
    uint32_t off;
    if (!p) return;
    off = (uint32_t)((uint8_t*)p - pl->base);
    for (b = pl->blocks; b; b = b->next)
        if (b->off == off && !b->free) {
            b->free = 1;
            pl->used -= b->size;
            break;
        }
    for (b = pl->blocks; b && b->next;) {
        if (b->free && b->next->free) {
            Blk* n = b->next;
            b->size += n->size;
            b->next = n->next;
            free(n);
        } else {
            b = b->next;
        }
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
#define PB_KICK 4096
#define PB_GUARD (PB_BYTES - 192 * 1024)
#define PCRTC_START_REG (*(volatile uint32_t*)0xFD600800)

static inline void put1(uint32_t m, uint32_t v) { P[0] = (1u << 18) | m; P[1] = v; P += 2; }
static inline void putf(uint32_t m, float v) { union { float f; uint32_t u; } c; c.f = v; put1(m, c.u); }

static void pb_open(void) {
    if (s_pb_open) return;
    P = pb_begin();
    s_pb_mark = P;
    s_pb_open = 1;
}

static void pb_close(void) {
    if (!s_pb_open) return;
    pb_end(P);
    s_pb_open = 0;
}

/* per-interval counters for the [NV2A] frame line */
static uint32_t s_st_waits, s_st_efb, s_st_tex_kb, s_st_verts, s_st_tex_fail, s_st_pb_peak, s_st_pb_resets;
static uint32_t s_st_draws, s_st_dirty_none, s_st_dirty_mtx, s_st_dirty[13];
static uint32_t s_st_prim[8];   /* by GX primitive, (prim >> 3) & 7 */

/* GPU faults, recorded by the patched pbkit (ocx_pb_gpu_fault below) */
static volatile uint32_t s_gf_count, s_gf_storms, s_gf_last[5];

/* A GPU that stops fetching (bad method or state) otherwise hangs the game
 * thread here with nothing in the log: after 2 s, report once where the
 * FIFO stopped. The push buffer is contiguous memory, mapped at
 * 0x80000000 | physical, and DMA_GET is its physical address. */
static void report_gpu_stall(void) {
    uint32_t get = *(volatile uint32_t*)(0xFD000000u + 0x3244), put = *(volatile uint32_t*)(0xFD000000u + 0x3240);
    const uint32_t* w = (const uint32_t*)(0x80000000u | (get & 0x03FFFFFFu));
    xhw_logf("[NV2A] GPU stalled: get %08x put %08x dma_state %08x pgraph %08x, faults %u (last kind %u %08x %08x)",
             get, put, *(volatile uint32_t*)(0xFD000000u + 0x3228), *(volatile uint32_t*)(0xFD000000u + 0x400700),
             (unsigned)s_gf_count, (unsigned)s_gf_last[0], (unsigned)s_gf_last[1], (unsigned)s_gf_last[2]);
    xhw_logf("[NV2A]  at get-32: %08x %08x %08x %08x %08x %08x %08x %08x", w[-8], w[-7], w[-6], w[-5], w[-4], w[-3],
             w[-2], w[-1]);
    xhw_logf("[NV2A]  at get:    %08x %08x %08x %08x %08x %08x %08x %08x", w[0], w[1], w[2], w[3], w[4], w[5], w[6],
             w[7]);
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

void ocx_pb_gpu_fault(unsigned kind, unsigned a, unsigned b, unsigned c, unsigned d) {
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
    uint16_t w, h;          /* after POT resampling */
    uint8_t levels;
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
    for (i = 0; i < s_ndeferred; i++) pool_free(pool_owns(&s_vb, s_deferred[i]) ? &s_vb : &s_tp, s_deferred[i]);
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

/* bilinear resample of one ARGB level to pw x ph (NPOT -> POT, so every
 * wrap mode keeps working and texcoords need no rescale) */
static uint32_t sample_bilinear(const uint32_t* src, int w, int h, float fx, float fy) {
    int x0 = (int)fx, y0 = (int)fy, x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
    float tx = fx - x0, ty = fy - y0;
    uint32_t a = src[y0 * w + x0], b = src[y0 * w + x1], c = src[y1 * w + x0], d = src[y1 * w + x1], out = 0;
    int k;
    for (k = 0; k < 32; k += 8) {
        float top = ((a >> k) & 0xFF) * (1 - tx) + ((b >> k) & 0xFF) * tx;
        float bot = ((c >> k) & 0xFF) * (1 - tx) + ((d >> k) & 0xFF) * tx;
        out |= (uint32_t)(top * (1 - ty) + bot * ty + 0.5f) << k;
    }
    return out;
}

static void write_level_direct(void* dstv, const void* srcv, int w, int h, int pw, int ph, int bpp);

/* One level into swizzled (Morton) order; ARGB8 NPOT images are resampled.
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

static void write_level_direct(void* dstv, const void* srcv, int w, int h, int pw, int ph, int bpp) {
    int x, y;
    swz_tables(pw, ph);
    if (bpp == 4 && (w != pw || h != ph)) {
        const uint32_t* src = (const uint32_t*)srcv;
        uint32_t* dst = (uint32_t*)dstv;
        for (y = 0; y < ph; y++) {
            float fy = ((y + 0.5f) * h / ph) - 0.5f;
            uint32_t yo = swz_y[y];
            if (fy < 0) fy = 0;
            for (x = 0; x < pw; x++) {
                float fx = ((x + 0.5f) * w / pw) - 0.5f;
                if (fx < 0) fx = 0;
                dst[yo | swz_x[x]] = sample_bilinear(src, w, h, fx, fy);
            }
        }
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
        case XGX_TEX_DXT1: return 0;
        default: return 4;
    }
}

static uint8_t nv_format(uint32_t fmt) {
    switch (fmt) {
        case XGX_TEX_RGB565: return NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5;
        case XGX_TEX_AY8: return NV097_SET_TEXTURE_FORMAT_COLOR_SZ_AY8;
        case XGX_TEX_A8Y8: return NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8;
        case XGX_TEX_DXT1: return NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5;
        case XGX_TEX_P8: return NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8;
        default: return NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8;
    }
}

static uint32_t level_size(uint32_t fmt, int w, int h) {
    if (fmt == XGX_TEX_DXT1) return (uint32_t)(((w + 3) / 4) * ((h + 3) / 4) * 8);
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
    if (pw != (int)w || ph != (int)h) {
        if (fmt != XGX_TEX_ARGB8) return 0;   /* only 32-bit images are resampled */
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
    base = (uint8_t*)pool_alloc(&s_tp, bytes);
    if (!base && s_ndeferred) {   /* nothing to gain from waiting when nothing is pending */
        wait_idle();
        release_deferred();
        base = (uint8_t*)pool_alloc(&s_tp, bytes);
    }
    if (!base) {
        s_st_tex_fail++;
        return 0;
    }
    mem = base + pal_bytes;
    dst = mem;
    sw = (int)w;
    sh = (int)h;
    lw = pw;
    lh = ph;
    for (l = 0; data && l < (int)levels; l++) {   /* no data: the GPU fills it (EFB copy) */
        if (fmt == XGX_TEX_DXT1) memcpy(dst, src, level_size(fmt, lw, lh));   /* block-linear, not swizzled */
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
static volatile int s_fbdump_once;

unsigned xgx_present_count(void) { return s_frame; }
static void frame_open(void);

static void clear_fb(int x, int y, int w, int h, uint32_t argb, int color, int depth, uint32_t z24) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_fbw) w = s_fbw - x;
    if (y + h > s_fbh) h = s_fbh - y;
    if (w <= 0 || h <= 0) return;
    pb_close();
    if (color) {
        uint32_t c = argb;
        if (s_bpp == 16) c = ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F);
        pb_fill(x, y, w, h, c);
    }
    if (depth) {
        uint32_t* p = pb_begin();
        uint32_t zv = pb_DepthFmt == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ? (z24 >> 8) : (z24 << 8);
        p = pb_push1(p, NV097_SET_CLEAR_RECT_HORIZONTAL, (uint32_t)((x + w - 1) << 16) | (uint32_t)x);
        p = pb_push1(p, NV097_SET_CLEAR_RECT_VERTICAL, (uint32_t)((y + h - 1) << 16) | (uint32_t)y);
        p = pb_push1(p, NV097_SET_ZSTENCIL_CLEAR_VALUE, zv);
        p = pb_push1(p, NV097_CLEAR_SURFACE, NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL);
        pb_end(p);
    }
}

void xgx_clear(const int32_t r[4], const uint8_t rgba[4], uint32_t z24, int color, int alpha, int depth) {
    int x0, y0, x1, y1;
    uint32_t argb = (uint32_t)rgba[3] << 24 | (uint32_t)rgba[0] << 16 | (uint32_t)rgba[1] << 8 | rgba[2];
    (void)alpha;
    frame_open();
    x0 = map_x((float)r[0]);
    y0 = map_y((float)r[1]);
    x1 = map_x((float)(r[0] + r[2]));
    y1 = map_y((float)(r[1] + r[3]));
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

static void frame_open(void) {
    if (s_frame_open) return;
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
    /* the bars outside the content rect, and a defined EFB */
    clear_fb(0, 0, s_fbw, s_fbh, 0xFF000000u, 1, 1, 0xFFFFFF);
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
static volatile int s_shot_once;
void xgx_shot_next(void) { s_shot_once = 1; }

void xgx_present(int black) {
    frame_open();
    if (black) clear_fb(0, 0, s_fbw, s_fbh, 0xFF000000u, 1, 0, 0);
    pb_budget();   /* the frame's pushbuffer peak */
    wait_idle();
    if (s_fbdump_once || (XHW_FBDUMP_EVERY && (s_frame + 1) % XHW_FBDUMP_EVERY == 0)) {
        s_fbdump_once = 0;
        xhw_fbdump(pb_back_buffer(), s_fbw, s_fbh, s_bpp, (int)pb_back_buffer_pitch());
    }
    if (s_shot_once) {
        s_shot_once = 0;
        xhw_fbdump_file(pb_back_buffer(), s_fbw, s_fbh, s_bpp, (int)pb_back_buffer_pitch());
    }
    release_deferred();
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
    frame_open();
}

/* ======================================================================
 * Vertex programs: cache + resident program memory
 * ====================================================================== */
typedef struct {
    VpKey key;
    uint32_t hash;     /* vp_hash(&key): compared before the key */
    VpProgram prog;
    int slot;          /* start in program memory, -1: not resident */
    uint32_t used;
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

#define VP_CACHE 48
static VpEntry* s_vp;
static int s_vp_count, s_vp_cur = -1;
static int s_vp_mem_top;     /* program memory used by resident programs */

static void vp_upload(VpEntry* e) {
    uint32_t i;
    if (s_vp_mem_top + (int)e->prog.n > VP_MAX_INSNS) {
        int k;
        for (k = 0; k < s_vp_count; k++) s_vp[k].slot = -1;
        s_vp_mem_top = 0;
    }
    e->slot = s_vp_mem_top;
    s_vp_mem_top += (int)e->prog.n;
    put1(NV097_SET_TRANSFORM_PROGRAM_LOAD, (uint32_t)e->slot);
    for (i = 0; i < e->prog.n; i += 8) {
        uint32_t n = e->prog.n - i < 8 ? e->prog.n - i : 8;
        pb_push(P++, NV097_SET_TRANSFORM_PROGRAM, n * 4);
        memcpy(P, &e->prog.words[i * 4], n * 16);
        P += n * 4;
    }
}

static void vp_select(const VpKey* k) {
    int i, pick = -1;
    uint32_t h = vp_hash(k);
    VpEntry* e;
    if (s_vp_cur >= 0 && s_vp[s_vp_cur].hash == h && memcmp(&s_vp[s_vp_cur].key, k, sizeof *k) == 0 &&
        s_vp[s_vp_cur].slot >= 0) {
        s_vp[s_vp_cur].used = s_frame;
        return;
    }
    for (i = 0; i < s_vp_count; i++)
        if (s_vp[i].hash == h && memcmp(&s_vp[i].key, k, sizeof *k) == 0) { pick = i; break; }
    if (pick < 0) {
        if (s_vp_count < VP_CACHE) {
            pick = s_vp_count++;
        } else {
            pick = 0;
            for (i = 1; i < s_vp_count; i++)
                if (s_vp[i].used < s_vp[pick].used) pick = i;
            if (s_vp[pick].slot >= 0) {   /* its memory can't be reused in place: flush all */
                for (i = 0; i < s_vp_count; i++) s_vp[i].slot = -1;
                s_vp_mem_top = 0;
            }
        }
        e = &s_vp[pick];
        e->key = *k;
        e->hash = h;
        vp_generate(k, &e->prog);
        e->slot = -1;
    }
    e = &s_vp[pick];
    if (e->slot < 0) vp_upload(e);
    e->used = s_frame;
    put1(NV097_SET_TRANSFORM_PROGRAM_START, (uint32_t)e->slot);
    s_vp_cur = pick;
    if (e->prog.approximated) s_approx++;
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

static void build_chans(const XgxState* st) {
    int c, k;
    vc_mark(VPC_CHAN, 4);
    for (c = 0; c < 2; c++)
        for (k = 0; k < 4; k++) {
            s_vc[VPC_CHAN + c * 2][k] = st->mat[c][k] / 255.0f;
            s_vc[VPC_CHAN + c * 2 + 1][k] = st->amb[c][k] / 255.0f;
        }
}

static void build_lights(const XgxState* st, uint32_t spec_lights) {
    int i;
    for (i = 0; i < XGX_MAX_LIGHTS; i++) {
        const XgxLight* l = &st->light[i];
        int b = VPC_LIGHT + i * 5;
        if (spec_lights & (1u << i)) {
            float n = sqrtf(l->pos[0] * l->pos[0] + l->pos[1] * l->pos[1] + l->pos[2] * l->pos[2]);
            n = n > 1e-12f ? 1.0f / n : 0.0f;
            set_row(b, l->pos[0] * n, l->pos[1] * n, l->pos[2] * n, 1);
        } else {
            set_row(b, l->pos[0], l->pos[1], l->pos[2], 1);
        }
        set_row(b + 1, l->dir[0], l->dir[1], l->dir[2], 0);
        set_row(b + 2, l->color[0] / 255.0f, l->color[1] / 255.0f, l->color[2] / 255.0f, l->color[3] / 255.0f);
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
static int s_rc_count, s_rc_last = -1;
static uint32_t s_rc_gen;             /* bumped whenever a cache slot is (re)compiled */
static const RcProg* s_rc_sent;       /* the program on the GPU, valid while s_rc_sent_gen == s_rc_gen */
static uint32_t s_rc_sent_gen;
static int s_rc_valid;
static uint32_t s_rc_consts[RC_MAX_STAGES][2], s_rc_fconsts[2];

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
        if (s_rc[k].hash == h && memcmp(&s_rc[k].cfg, cfg, n) == 0) {
            s_rc_last = k;
            return &s_rc[k].prog;
        }
    k = s_rc_count < RC_CACHE ? s_rc_count++ : (int)(s_draws % RC_CACHE);
    s_rc_last = k;
    s_rc_gen++;
    s_rc[k].hash = h;
    s_rc[k].cfg = *cfg;
    rc_compile(cfg, &s_rc[k].prog);
    return &s_rc[k].prog;
}

static uint8_t clamp_s10(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* RREF_FIXED constants: rgb, a (nv2a_rc.c's movie YUV program) */
static const uint8_t k_rc_fixed[4][4] = {
    { 90, 0, 0, 44 },     /* 0.351, 0, 0 | 0.1725 */
    { 0, 0, 113, 91 },    /* 0, 0, 0.443 | 0.357 */
    { 255, 0, 255, 0 },
    { 0, 255, 0, 0 },
};

static void ref_val(const XgxState* st, uint16_t ref, uint8_t rgb[3], uint8_t* a) {
    int t = ref >> 8, p = ref & 0xFF, k;
    switch (t) {
        case RREF_FIXED:
            for (k = 0; k < 3; k++) rgb[k] = k_rc_fixed[p & 3][k];
            *a = k_rc_fixed[p & 3][3];
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
        put1(NV097_SET_COMBINER_SPECULAR_FOG_CW0, rp->cw0);
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
static uint32_t s_tex_shadow[4][7];
static uint32_t s_tex_prog = 0xFFFFFFFFu;   /* NV097_SET_SHADER_STAGE_PROGRAM sent last */

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
        uint32_t v[7] = { 0, 0, 0, 0, 0, 0, 0 };
        const Tex* t = NULL;
        const XgxMap* m = NULL;
        if (u < nunits) {
            m = &st->map[unit_map[u]];
            if (m->tex && m->tex < MAX_TEX && s_tex[m->tex].used) t = &s_tex[m->tex];
        }
        if (t) {
            uint32_t minf = m->min_filter + 1, magf = m->mag_filter == 0 ? 1 : 2;
            if (t->levels <= 1 && minf > 2) minf = minf == 3 || minf == 5 ? 1 : 2;   /* no mips: drop the mip part */
            v[0] = (uint32_t)t->mem & 0x03FFFFFF;
            v[1] = 1 | (1u << 3) | (2u << 4) | ((uint32_t)t->nvfmt << 8) |
                   ((uint32_t)t->levels << 16) | ((uint32_t)log2i(t->w) << 20) | ((uint32_t)log2i(t->h) << 24);
            v[2] = wrap_mode(m->wrap_s) | (wrap_mode(m->wrap_t) << 8) | (3u << 16);
            v[3] = 0x4003FFC0u;
            v[4] = (minf << 16) | (magf << 24) | ((uint32_t)((int)(m->lod_bias * 256.0f)) & 0x1FFF) | 0x2000u;
            /* not sent: a texture made at a freed one's address, format and
             * size still re-sends the unit, so a P8 texture's palette is
             * loaded again (xemu reads palettes at each draw; the console
             * may keep the one it has) */
            v[5] = t->gen;
            v[6] = t->pal;   /* DMA A: bit 0 clear */
            prog |= 1u << (u * 5);   /* 2D_PROJECTIVE */
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
            }
            memcpy(s_tex_shadow[u], v, sizeof v);
        }
    }
    if (prog != s_tex_prog) {
        put1(NV097_SET_SHADER_STAGE_PROGRAM, prog);
        s_tex_prog = prog;
    }
}

/* ======================================================================
 * Fixed-function pixel state
 * ====================================================================== */
static int s_fixed[20];
/* vertex attribute arrays and inline values last sent (emit_vertex_arrays) */
static uint32_t s_attr_shadow[16], s_attr_off_shadow[16];
static uint32_t s_default_mtx = 0xFFFFFFFFu;
static int s_inline_col[2];   /* the inline colour holds our opaque white */

static void state_reset_shadows(void) {
    s_draw_force = XGX_DIRTY_ALL;
    memset(s_fixed, 0xFF, sizeof s_fixed);
    memset(s_tex_shadow, 0xFF, sizeof s_tex_shadow);
    s_tex_prog = 0xFFFFFFFFu;
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
    SETF(13, NV097_SET_WINDOW_CLIP_HORIZONTAL, (uint32_t)x0 | ((uint32_t)x1 << 16));
    SETF(14, NV097_SET_WINDOW_CLIP_VERTICAL, (uint32_t)y0 | ((uint32_t)y1 << 16));
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
        SETF(15, NV097_SET_ALPHA_TEST_ENABLE, en);
        if (en) {
            SETF(16, NV097_SET_ALPHA_FUNC, 0x200 + (fn & 7));
            SETF(17, NV097_SET_ALPHA_REF, ref);
        }
    }
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
/* -DXGX_DEBUG_TRACE: log every draw of the frame an autopad SHOT dumps */
static void trace_draw(uint32_t prim, uint32_t count, const XgxState* st, int approx) {
    uint32_t s, i;
    xhw_logf("[DRAW] #%u prim %u n %u tev %u ind %u texgen %u approx %d blend %u %u %u alpha %u/%u %u %u/%u z %u/%u/%u",
             s_draws, prim, count, st->ntev, st->nind, st->ntexgen, approx, st->blend_type, st->blend_src,
             st->blend_dst, st->alpha_comp0, st->alpha_ref0, st->alpha_op, st->alpha_comp1, st->alpha_ref1,
             st->z_enable, st->z_func, st->z_update);
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
    }
    for (i = 0; i < st->ntexgen && i < XGX_MAX_TEXGEN; i++)
        xhw_logf("[DRAW]  tg%u type %u src %u mtx %u norm %u pt %u", i, st->texgen[i].type, st->texgen[i].src,
                 st->texgen[i].mtx, st->texgen[i].normalize, st->texgen[i].pt_mtx);
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

/* texture units (one per distinct texcoord/texmap the TEV samples) and the combiner setup */
static void derive_units(const XgxState* st) {
    RcCfg* rc = &s_d_rc;
    int s, i, nunits = 0, nstages = st->ntev > RC_MAX_TEV ? RC_MAX_TEV : st->ntev ? (int)st->ntev : 1;
    memset(rc, 0, offsetof(RcCfg, st) + (size_t)nstages * sizeof(RcStage));   /* rc_used() */
    s_d_unit_miss = 0;
    rc->nstages = (uint8_t)nstages;
    for (s = 0; s < rc->nstages; s++) {
        const XgxTevStage* t = &st->tev[s];
        RcStage* r = &rc->st[s];
        int u = -1;
        for (i = 0; i < 4; i++) {
            r->cin[i] = (uint8_t)t->cin[i];
            r->ain[i] = (uint8_t)t->ain[i];
        }
        r->cop = (uint8_t)t->cop; r->aop = (uint8_t)t->aop;
        r->cbias = (uint8_t)t->cbias; r->cscale = (uint8_t)t->cscale;
        r->abias = (uint8_t)t->abias; r->ascale = (uint8_t)t->ascale;
        r->cclamp = (uint8_t)t->cclamp; r->aclamp = (uint8_t)t->aclamp;
        r->cout = (uint8_t)(t->cout & 3); r->aout = (uint8_t)(t->aout & 3);
        r->kcsel = (uint8_t)t->kcsel; r->kasel = (uint8_t)t->kasel;
        r->ras = t->chan == GX_COLOR0A0 ? 0 : t->chan == GX_COLOR1A1 ? 1 : 2;
        r->tex_alpha_bcast = st->swap[t->tex_swap & 3][0] == 3 && st->swap[t->tex_swap & 3][1] == 3;
        r->ras_alpha_bcast = st->swap[t->ras_swap & 3][0] == 3 && st->swap[t->ras_swap & 3][1] == 3;
        if (t->texmap != GX_NULL && t->texmap < XGX_MAX_MAPS && t->texcoord != GX_NULL && st->map[t->texmap].tex) {
            for (i = 0; i < nunits; i++)
                if (s_d_unit_map[i] == (int)t->texmap && s_d_unit_tc[i] == (int)t->texcoord) { u = i; break; }
            if (u < 0 && nunits < 4) {
                u = nunits++;
                s_d_unit_map[u] = (int)t->texmap;
                s_d_unit_tc[u] = (int)t->texcoord;
            }
            if (u < 0) s_d_unit_miss++;
        }
        r->unit = (int8_t)u;
        if (u >= 0) rc->units_used |= (uint8_t)(1u << u);
        if (r->ras == 1) rc->v1_used = 1;
    }
    s_d_nunits = nunits;
    s_d_rp = rc_lookup(rc);
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
    vk->ntex = (uint8_t)s_d_nunits;
    for (i = 0; i < s_d_nunits; i++) {
        const XgxTexGen* tg = &st->texgen[s_d_unit_tc[i] < XGX_MAX_TEXGEN ? s_d_unit_tc[i] : 0];
        vk->tex[i].src = (uint8_t)tg->src;
        vk->tex[i].proj = tg->type == GX_TG_MTX3x4;
        vk->tex[i].normalize = (uint8_t)(tg->normalize != 0);
        if (tg->mtx < GX_TEXMTX0) s_d_tg_posmtx = 1;
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
#ifdef XGX_DEBUG_TRACE
    if (s_fbdump_once) trace_draw(prim, count, st, s_d_rp->approximated);
#endif
    if ((d & DIRTY_TG) || (s_d_tg_posmtx && (d & XGX_DIRTY_POSMTX)))
        for (i = 0; i < s_d_nunits; i++)
            build_texgen(st, i, &st->texgen[s_d_unit_tc[i] < XGX_MAX_TEXGEN ? s_d_unit_tc[i] : 0]);

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
        int fx = map_x(src[0] + (x + 0.5f) * (float)src[2] / (float)dw);
        col[x] = fx >= s_fbw ? s_fbw - 1 : fx < 0 ? 0 : fx;
    }
    for (y = 0; y < dh; y++) {
        int fy = map_y(src[1] + (y + 0.5f) * (float)src[3] / (float)dh);
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
 * and xgx_draw re-sends every state group. 32-bit framebuffers only: at
 * 720p the depth buffer is 16-bit, and the NV2A wants colour and depth
 * surfaces of the same width. */
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
    uint32_t pw = t->w, ph = t->h, fb = (uint32_t)pb_back_buffer() & 0x03FFFFFF, i;
    float u0 = s_cx + src[0] * (float)s_cw / XGX_EFB_W + 0.5f, u1 = u0 + src[2] * (float)s_cw / XGX_EFB_W;
    float v0 = s_cy + src[1] * (float)s_ch / XGX_EFB_H + 0.5f, v1 = v0 + src[3] * (float)s_ch / XGX_EFB_H;
    /* nearest, unless the copy is smaller than its source (copy_dim) */
    uint32_t filt = (float)pw < u1 - u0 - 0.5f || (float)ph < v1 - v0 - 0.5f ? 2 : 1;
    float* v;
    int pf = xhw_perf_enter(XHW_PERF_EFB);
    s_st_efb++;
    frame_open();
    pb_budget();
    v = (float*)xgx_vtx_alloc(4, 20);
    if (!v) {
        xhw_perf_leave(pf);
        return;
    }
    /* x y z u v: the texture's corners and the source rect's, in texels
     * (+0.5: nearest sampling picks the texel the CPU path rounds to) */
    {
        const float q[4][5] = { { 0, 0, 1, u0, v0 }, { (float)pw, 0, 1, u1, v0 },
                                { 0, (float)ph, 1, u0, v1 }, { (float)pw, (float)ph, 1, u1, v1 } };
        memcpy(v, q, sizeof q);
    }
    pb_open();
    put1(NV097_WAIT_FOR_IDLE, 0);   /* the source's pixels are in memory */

    /* target: the texture, through pbkit's DMA object over all of RAM (3) */
    put1(NV097_SET_CONTEXT_DMA_COLOR, 3);
    put1(NV097_SET_SURFACE_FORMAT, NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8 |
                                       NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 << 4 |
                                       NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE << 8 |
                                       (uint32_t)log2i((int)pw) << 16 | (uint32_t)log2i((int)ph) << 24);
    put1(NV097_SET_SURFACE_PITCH, pw * 4 | (pw * 4) << 16);
    put1(NV097_SET_SURFACE_COLOR_OFFSET, (uint32_t)t->mem & 0x03FFFFFF);
    put1(NV097_SET_SURFACE_CLIP_HORIZONTAL, pw << 16);
    put1(NV097_SET_SURFACE_CLIP_VERTICAL, ph << 16);
    put1(NV097_SET_WINDOW_CLIP_HORIZONTAL, pw << 16);
    put1(NV097_SET_WINDOW_CLIP_VERTICAL, ph << 16);

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
                                       (uint32_t)NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 << 8 | (1u << 16));
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
    put1(NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_TRIANGLE_STRIP);   /* not a quad: see gx_vtx.c out_prim */
    *P++ = 1u << 18 | NV2A_SUPPRESS_COMMAND_INCREMENT(NV097_DRAW_ARRAYS);
    *P++ = 3u << 24;   /* 4 vertices from 0 */
    put1(NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    put1(NV097_WAIT_FOR_IDLE, 0);   /* the copy is in memory before anything samples it */

    /* back to the back buffer and the game's state */
    put1(NV097_SET_STENCIL_TEST_ENABLE, 1);   /* as pbkit leaves it */
    pb_close();
    ocx_pb_retarget_back_buffer();
    memset(s_fixed, 0xFF, sizeof s_fixed);
    memset(s_tex_shadow, 0xFF, sizeof s_tex_shadow);
    s_tex_prog = 0xFFFFFFFFu;
    s_rc_valid = 0;
    s_vp_cur = -1;
    s_draw_force = XGX_DIRTY_ALL;
    xhw_perf_leave(pf);
}
#endif

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
    reusable = reuse && reuse < MAX_TEX && s_tex[reuse].used && s_tex[reuse].w == pw && s_tex[reuse].h == ph &&
               s_tex[reuse].levels == 1 && s_tex[reuse].nvfmt == nv_format(XGX_TEX_ARGB8);
#if XGX_EFB_GPU_COPY
    if (s_bpp == 32) {
        uint32_t tex = reusable ? reuse : xgx_tex_create(pw, ph, 1, XGX_TEX_ARGB8, NULL);
        if (tex) efb_copy_gpu(src, &s_tex[tex], mode);
        return tex;
    }
#endif
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
        if (vm->bpp == 32) xhw_fatal("Graphics init failed", "The NV2A could not be started.");
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
    xhw_mem_log("after nv2a");
    return 1;
}
