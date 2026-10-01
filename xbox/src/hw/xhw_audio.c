/* xhw_audio.c - 32 kHz stereo from the AX mixer -> AC97 (hardware) or an MCPX
 * APU voice (xemu). The drivers come from OpenCrossing-Xbox xbox_audio.c.
 *
 * The mixer (src/pc/audio.c on the SDK side) writes 32 kHz s16 stereo into a
 * lock-free SPSC ring with xhw_audio_write(). A high-priority pump thread
 * resamples it to the AC97's fixed 48 kHz and keeps DMA descriptors queued.
 *
 * - AC97 is driven POLLED, no interrupt: nxdk's hal/audio IRQ handler froze a
 *   real Xbox at XAudioPlay, and the AC97 IRQ never fires in xemu.
 * - xemu (detected by CPUID: no VME) plays no AC97 on some hosts; there a
 *   looping APU buffer voice is used instead. Kill switch: -DXHW_AUDIO_APU=0. */
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <string.h>

#include "xhw.h"
#include "xhw_internal.h"

#define AGET(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define ASET(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

/* ring of 32 kHz s16 samples (interleaved stereo), ~512 ms */
#define RING_SAMPLES 32768
#define RING_MASK (RING_SAMPLES - 1)
static int16_t s_ring[RING_SAMPLES];
static uint32_t s_wp, s_rp;   /* in samples, free-running */
static uint32_t s_in_rate = 32000;

#define NBUF 8                /* divides 32 (the descriptor ring) */
#define OUT_FRAMES 1024       /* 48 kHz frames per buffer, ~21 ms */
#define ACI ((volatile uint8_t*)0xFEC00000)

static int16_t* s_outbuf[NBUF];
static int s_pump_run, s_started, s_aci_on;
static unsigned s_queued;
static uint32_t s_frac;

uint32_t xhw_audio_space(void) {
    uint32_t used = AGET(s_wp) - AGET(s_rp);
    return used >= RING_SAMPLES ? 0 : (RING_SAMPLES - used) / 2;
}

void xhw_audio_write(const int16_t* stereo, uint32_t frames) {
    uint32_t wp = s_wp, i, n = frames * 2;
    uint32_t space = xhw_audio_space() * 2;
    if (n > space) n = space;
    for (i = 0; i < n; i++) s_ring[(wp + i) & RING_MASK] = stereo[i];
    ASET(s_wp, wp + n);
}

static void fill_48k(int16_t* out) {
    const uint32_t step = (uint32_t)(((uint64_t)s_in_rate << 16) / 48000);
    uint32_t wp = AGET(s_wp), rp = AGET(s_rp);
    int i;
    for (i = 0; i < OUT_FRAMES; i++) {
        if ((int32_t)(wp - (rp + 4)) < 0) {
            out[2 * i] = out[2 * i + 1] = 0;   /* underrun: silence */
            continue;
        }
        {
            int32_t t = (int32_t)(s_frac & 0xFFFF);
            int32_t l0 = s_ring[rp & RING_MASK], r0 = s_ring[(rp + 1) & RING_MASK];
            int32_t l1 = s_ring[(rp + 2) & RING_MASK], r1 = s_ring[(rp + 3) & RING_MASK];
            out[2 * i] = (int16_t)(l0 + (((l1 - l0) * t) >> 16));
            out[2 * i + 1] = (int16_t)(r0 + (((r1 - r0) * t) >> 16));
        }
        s_frac += step;
        rp += (s_frac >> 16) * 2;
        s_frac &= 0xFFFF;
    }
    ASET(s_rp, rp);
}

/* ---- AC97 (MCPX ACI), polled ---- */
typedef struct { uint32_t addr; uint16_t samples; uint16_t ctl; } AciDesc;
static AciDesc* s_desc_pcm;
static AciDesc* s_desc_spdif;
static unsigned s_next_desc;

static int aci_wait(volatile uint32_t* reg, uint32_t mask, uint32_t want, const char* what) {
    int i;
    for (i = 0; i < 1000000; i++)
        if ((*reg & mask) == want) return 1;
    xhw_logf("[AUDIO] timeout waiting for %s", what);
    return 0;
}

static int aci_reset(void);

static int aci_init(void) {
    uint8_t* mem = (uint8_t*)MmAllocateContiguousMemoryEx(2 * 32 * sizeof(AciDesc), 0, 0xFFFFFFFF, 0, PAGE_READWRITE);
    if (!mem) return 0;
    memset(mem, 0, 2 * 32 * sizeof(AciDesc));
    s_desc_pcm = (AciDesc*)mem;
    s_desc_spdif = (AciDesc*)(mem + 32 * sizeof(AciDesc));
    return aci_reset();
}

/* Stops both bus masters, resets them (CIV and LVI back to 0) and points
 * them at empty descriptor lists. The engine stays stopped: aci_start
 * queues buffers before it sets the run bit. */
static void aci_bm_reset(void) {
    volatile uint32_t* m = (volatile uint32_t*)ACI;
    ACI[0x11B] = 0;   /* DMA and interrupt enables off first */
    ACI[0x17B] = 0;
    ACI[0x11B] = 1u << 1;   /* reset both bus masters */
    ACI[0x17B] = 1u << 1;
    { int i; for (i = 0; i < 1000000 && ((ACI[0x11B] | ACI[0x17B]) & 2); i++) {} }
    memset(s_desc_pcm, 0, 2 * 32 * sizeof(AciDesc));
    ACI[0x116] = 0xFF;
    ACI[0x176] = 0xFF;
    m[0x100 >> 2] = 0;
    m[0x110 >> 2] = MmGetPhysicalAddress(s_desc_pcm);
    m[0x170 >> 2] = MmGetPhysicalAddress(s_desc_spdif);
    s_next_desc = 0;
}

/* Cold-resets the AC-link, then the bus masters. */
static int aci_reset(void) {
    volatile uint32_t* m = (volatile uint32_t*)ACI;
    LARGE_INTEGER d;
    ACI[0x11B] = 0;
    ACI[0x17B] = 0;
    m[0x12C >> 2] &= ~2u;   /* cold reset the AC-link */
    d.QuadPart = -10 * 1000;
    KeDelayExecutionThread(KernelMode, FALSE, &d);
    m[0x12C >> 2] |= 2u;
    aci_wait(&m[0x130 >> 2], 0x100, 0x100, "codec ready");   /* logged; carry on as before */
    aci_bm_reset();
    return 1;
}

static void aci_queue(const int16_t* buf, unsigned bytes) {
    uint32_t phys = MmGetPhysicalAddress((void*)buf);
    unsigned i = s_next_desc;
    s_desc_pcm[i].addr = s_desc_spdif[i].addr = phys;
    s_desc_pcm[i].samples = s_desc_spdif[i].samples = (uint16_t)(bytes / 2);
    s_desc_pcm[i].ctl = s_desc_spdif[i].ctl = 0;
    __asm__ volatile("sfence" ::: "memory");   /* write-combined samples must land first */
    ACI[0x115] = (uint8_t)i;
    ACI[0x175] = (uint8_t)i;
    s_next_desc = (i + 1) % 32;
}

static void aci_run(int on) {
    ACI[0x11B] = on ? 1 : 0;
    ACI[0x17B] = on ? 1 : 0;
}

/* (Re)starts playback from a clean engine: bus masters reset, NBUF - 1
 * buffers of audio queued from index 0, LVI on the last of them, and only
 * then the run bit. v31 on the console: the run bit was set with
 * descriptor 0 still empty (at boot the pump thread raced the init's
 * aci_run, a restart only toggled the run bit, and a cold reset zeroed
 * every descriptor and ran at once): CIV stayed at 0 with the engine
 * running, silent for the whole boot, cold resets included (v27 too). */
static void aci_start(void) {
    aci_bm_reset();
    s_queued = 0;
    while (s_queued < NBUF - 1) {
        int16_t* b = s_outbuf[s_queued % NBUF];
        fill_48k(b);
        aci_queue(b, OUT_FRAMES * 4);
        s_queued++;
    }
    aci_run(1);
}

/* Polled, nobody clears the status bits or notices a halt. If the pump
 * misses its deadline (NBUF - 1 buffers, ~150 ms) the bus master plays up
 * to the last valid index and halts (DCH); moving LVI on doesn't restart it
 * on the MCPX, and the audio stayed silent for the whole boot (v13, audio 0%
 * in every [PERF] line: the ring never drained). Clear the sticky status,
 * and restart a halted or stuck engine. */
static unsigned s_aci_restarts, s_aci_stuck, s_aci_last_civ = 99, s_aci_dead, s_aci_resets;

static void aci_check(unsigned civ) {
    uint8_t sr = ACI[0x116], sr2 = ACI[0x176];
    if (sr & 0x1C) ACI[0x116] = (uint8_t)(sr & 0x1C);   /* LVBCI BCIS FIFOE: write 1 to clear */
    if (sr2 & 0x1C) ACI[0x176] = (uint8_t)(sr2 & 0x1C);
    if (civ != s_aci_last_civ) s_aci_dead = 0;   /* a buffer finished: the engine runs */
    s_aci_stuck = civ == s_aci_last_civ ? s_aci_stuck + 1 : 0;
    s_aci_last_civ = civ;
    /* halted, or no buffer finished for ~100 ms (a buffer is ~21 ms) */
    if ((sr & 1) || s_aci_stuck > 50) {
        if (s_aci_restarts++ < 8)
            xhw_logf("[AUDIO] AC97 %s: civ %u lvi %u sr %02x/%02x, restarting (%u)", sr & 1 ? "halted" : "stuck", civ,
                     ACI[0x115] & 31u, sr, sr2, s_aci_restarts);
        aci_run(0);
        /* v27 on the console: running (sr 00) but CIV never left 0 through
         * eight restarts, silent for the whole boot. If the codec isn't
         * taking frames, after three restarts without a finished buffer,
         * cold-reset the AC-link (with a pause that grows) as well. */
        if (++s_aci_dead >= 3 && s_aci_resets < 32) {
            volatile uint32_t* m = (volatile uint32_t*)ACI;
            LARGE_INTEGER d;
            s_aci_resets++;
            d.QuadPart = -10 * 1000 * (int64_t)(s_aci_resets < 10 ? s_aci_resets * 10 : 100);
            KeDelayExecutionThread(KernelMode, FALSE, &d);
            xhw_logf("[AUDIO] AC97 cold reset (%u): global control %08x status %08x", s_aci_resets,
                     (unsigned)m[0x12C >> 2], (unsigned)m[0x130 >> 2]);
            aci_reset();
            s_aci_dead = 0;
        }
        aci_start();
        s_aci_last_civ = ACI[0x114] & 31;
        s_aci_stuck = 0;
    }
}

/* ---- MCPX APU buffer voice (xemu) ---- */
#ifndef XHW_AUDIO_APU
#define XHW_AUDIO_APU 1
#endif
#define APU ((volatile uint8_t*)0xFE800000)
#define APU_REG(o) (*(volatile uint32_t*)(APU + (o)))
#define APU_PIO(m, v) (*(volatile uint32_t*)(APU + 0x20000 + (m)) = (v))
#define APU_VOICE 64
#define APU_RING_PAGES 16
#define APU_RING_FRAMES (APU_RING_PAGES * 4096 / 4)
#define APU_LEAD (4 * OUT_FRAMES)

static int s_apu;
static uint8_t* s_apu_mem;
static int16_t* s_apu_ring;
static volatile uint32_t* s_apu_cbo;
static uint32_t s_apu_wp;

static int apu_init(void) {
    const uint32_t voices = 3 * 4096, notify = 2 * 4096, sge = 4096;
    const uint32_t size = voices + notify + sge + APU_RING_PAGES * 4096;
    uint32_t i, pv, pn, ps, pr, *tab;
    s_apu_mem = (uint8_t*)MmAllocateContiguousMemoryEx(size, 0, 0xFFFFFFFF, 4096, PAGE_READWRITE);
    if (!s_apu_mem) return 0;
    memset(s_apu_mem, 0, size);
    pv = MmGetPhysicalAddress(s_apu_mem);
    pn = pv + voices;
    ps = pn + notify;
    pr = ps + sge;
    tab = (uint32_t*)(s_apu_mem + voices + notify);
    for (i = 0; i < APU_RING_PAGES; i++) {
        tab[2 * i] = pr + i * 4096;
        tab[2 * i + 1] = 0;
    }
    s_apu_ring = (int16_t*)(s_apu_mem + voices + notify + sge);
    s_apu_cbo = (volatile uint32_t*)(s_apu_mem + APU_VOICE * 0x80 + 0x58);
    APU_REG(0x1004) = 0;
    APU_REG(0x202C) = pv;
    APU_REG(0x2030) = ps;
    APU_REG(0x2034) = ps;
    APU_REG(0x115C) = pn;
    APU_REG(0x2054) = 0xFFFF;
    APU_REG(0x2060) = 0xFFFF;
    APU_REG(0x206C) = 0xFFFF;
    APU_REG(0x1100) = 0;
    APU_REG(0x2000) = 1u << 3;
    APU_PIO(0x2F8, APU_VOICE);
    APU_PIO(0x300, (1u << 5));
    /* LOOP STEREO S16, SAMPLES_PER_BLOCK field = channels - 1 (traps.md) */
    APU_PIO(0x304, (1u << 16) | (1u << 25) | (1u << 27) | (1u << 28) | (1u << 30));
    APU_PIO(0x308, 0); APU_PIO(0x30C, 0); APU_PIO(0x310, 0);
    APU_PIO(0x314, 0); APU_PIO(0x318, 0);
    APU_PIO(0x360, 0x000F000F);
    APU_PIO(0x364, 0xFFFFFFFF);
    APU_PIO(0x368, 0xFFFFFFFF);
    APU_PIO(0x36C, 0); APU_PIO(0x374, 0); APU_PIO(0x378, 0);
    APU_PIO(0x37C, 0);
    APU_PIO(0x3A0, 0);
    APU_PIO(0x3A4, 0);
    APU_PIO(0x3DC, APU_RING_FRAMES - 1);
    APU_PIO(0x3D8, 0);
    APU_PIO(0x120, 1u << 16);
    APU_PIO(0x124, APU_VOICE);
    s_apu_wp = 0;
    return 1;
}

static uint32_t apu_lead(void) {
    uint32_t cbo = *s_apu_cbo & 0xFFFFFF;
    return (s_apu_wp + APU_RING_FRAMES - cbo) % APU_RING_FRAMES;
}

static void apu_pump(void) {
    uint32_t lead = apu_lead();
    if (lead > APU_RING_FRAMES / 2) {
        uint32_t cbo = *s_apu_cbo & 0xFFFFFF;
        s_apu_wp = ((cbo / OUT_FRAMES) + 2) * OUT_FRAMES % APU_RING_FRAMES;
        lead = apu_lead();
    }
    while (lead < APU_LEAD) {
        fill_48k(s_apu_ring + s_apu_wp * 2);
        s_apu_wp = (s_apu_wp + OUT_FRAMES) % APU_RING_FRAMES;
        lead += OUT_FRAMES;
    }
}

static void pump(void* arg) {
    (void)arg;
    while (AGET(s_pump_run)) {
        if (s_apu) {
            apu_pump();
        } else {
            unsigned civ, ahead;
            if (!s_aci_on) {   /* first start here, after the queue is filled */
                aci_start();
                s_aci_last_civ = ACI[0x114] & 31;
                s_aci_on = 1;
            }
            civ = ACI[0x114] & 31;
            aci_check(civ);
            civ = ACI[0x114] & 31;
            ahead = ((s_queued & 31) - civ) & 31;
            while (ahead < NBUF - 1) {
                int16_t* b = s_outbuf[s_queued % NBUF];
                fill_48k(b);
                aci_queue(b, OUT_FRAMES * 4);
                s_queued++;
                ahead++;
            }
        }
        Sleep(2);
    }
}

static int running_in_xemu(void) {
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    return !(d & (1u << 1));   /* xemu's CPU model has no VME */
}

int xhw_audio_init(uint32_t rate) {
    int i, xemu;
    if (s_started) return 1;
    s_in_rate = rate ? rate : 32000;
    for (i = 0; i < NBUF; i++) {
        s_outbuf[i] = (int16_t*)MmAllocateContiguousMemoryEx(OUT_FRAMES * 4, 0, 0xFFFFFFFF, 0,
                                                            PAGE_READWRITE | PAGE_WRITECOMBINE);
        if (!s_outbuf[i]) {
            xhw_logf("[AUDIO] buffer alloc failed");
            return 0;
        }
        memset(s_outbuf[i], 0, OUT_FRAMES * 4);
    }
    xemu = running_in_xemu();
    if (!aci_init()) {
        xhw_logf("[AUDIO] AC97 init failed");
        return 0;
    }
    if (xemu) {
        /* xemu's codec resets muted; the retail codec has no mixer registers */
        *(volatile uint16_t*)(ACI + 0x02) = 0x0000;
        *(volatile uint16_t*)(ACI + 0x18) = 0x0000;
    }
    s_apu = XHW_AUDIO_APU && xemu && apu_init();
    s_aci_on = 0;
    ASET(s_pump_run, 1);
    xhw_thread_start(pump, NULL, 2, 16 * 1024);   /* AC97: the pump starts the engine */
    s_started = 1;
    xhw_logf("[AUDIO] %s, %u Hz in -> 48 kHz", s_apu ? "xemu APU voice" : "AC97 polled", s_in_rate);
    return 1;
}

void xhw_audio_stop(void) { xhw_audio_shutdown(); }

void xhw_audio_shutdown(void) {
    if (!s_started) return;
    ASET(s_pump_run, 0);
    Sleep(10);
    aci_run(0);
    if (s_apu) APU_PIO(0x128, APU_VOICE);   /* VOICE_OFF */
    s_started = 0;
}
