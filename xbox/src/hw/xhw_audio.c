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
 *   looping APU buffer voice is used instead. Kill switch: -DXHW_AUDIO_APU=0.
 * - At boot the controller and the APU are logged as found, then brought to
 *   idle whatever ran before (audio_idle); an engine that never finishes a
 *   buffer from the boot on is given up: silent, no cold-reset loop. */
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <stdio.h>
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
static int s_pump_run, s_pump_alive, s_started, s_aci_on;
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
static unsigned s_rr_polls;   /* the last bus-master reset's wait (the boot's [AUDIO] idle line) */

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
    { int i; for (i = 0; i < 1000000 && ((ACI[0x11B] | ACI[0x17B]) & 2); i++) {} s_rr_polls = (unsigned)i; }
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
static int s_aci_played;   /* a buffer has finished since the boot */
static int s_aci_silent;   /* stuck since the boot: given up, the pump drains the ring in real time */
static uint64_t s_drain_ns;

/* Test switch -DXHW_AUDIO_TEST (docs/testing.md): 1 leaves the engine
 * running into the next boot at a relaunch, as a crash or another XBE
 * would; 2 reads CIV as 0 for good, an engine stuck from the boot on. */
#ifndef XHW_AUDIO_TEST
#define XHW_AUDIO_TEST 0
#endif
#define ACI_CIV() ((XHW_AUDIO_TEST & 2) ? 0u : (unsigned)(ACI[0x114] & 31))

/* Clears the run bit of the bus master at ACI + base and waits (20 ms at
 * most) for it to halt (DCH, SR bit 0). Returns 1 if it did; *us is the wait. */
static int aci_halt(unsigned base, unsigned* us) {
    uint64_t t0 = xhw_time_ns(), t;
    ACI[base + 0xB] = 0;
    for (;;) {
        t = xhw_time_ns() - t0;
        if (*(volatile uint16_t*)(ACI + base + 6) & 1) break;
        if (t > 20000000) {
            *us = (unsigned)(t / 1000);
            return 0;
        }
    }
    *us = (unsigned)(t / 1000);
    return 1;
}

/* Roadmap item 9: an engine that never finished a buffer since the boot,
 * through the boot's own idle sequence and one more cold reset, stays
 * stuck (console A, 2026-10-03/04: ten cold resets in 40 s, every one
 * freezing the game for about a second, silent until a power-off). Stop
 * it, play silent, say so once. The mixer keeps running: the pump takes
 * the ring's samples in real time as the engine would. */
static void aci_give_up(unsigned civ, uint8_t sr, uint8_t sr2) {
    volatile uint32_t* m = (volatile uint32_t*)ACI;
    unsigned us;
    xhw_logf("[AUDIO] stuck since boot: civ %u lvi %u sr %02x/%02x picb %u after %u restarts and %u cold resets "
             "(global control %08x status %08x); silent until the Xbox is switched off and on",
             civ, ACI[0x115] & 31u, sr, sr2, (unsigned)*(volatile uint16_t*)(ACI + 0x118), s_aci_restarts,
             s_aci_resets, (unsigned)m[0x12C >> 2], (unsigned)m[0x130 >> 2]);
    aci_halt(0x110, &us);
    aci_halt(0x170, &us);
    s_drain_ns = xhw_time_ns();
    s_aci_silent = 1;
    xhw_notice("Sound hardware is stuck.", "Turn the Xbox off and on to get sound back.");
}

static void aci_drain(void) {
    const uint64_t buf_ns = (uint64_t)OUT_FRAMES * 1000000000u / 48000;
    uint64_t now = xhw_time_ns();
    if (now - s_drain_ns > 10 * buf_ns) s_drain_ns = now - buf_ns;   /* no catch-up after a long stall */
    while (now - s_drain_ns >= buf_ns) {
        fill_48k(s_outbuf[0]);
        s_drain_ns += buf_ns;
    }
}

static void aci_check(unsigned civ) {
    uint8_t sr = ACI[0x116], sr2 = ACI[0x176];
    if (sr & 0x1C) ACI[0x116] = (uint8_t)(sr & 0x1C);   /* LVBCI BCIS FIFOE: write 1 to clear */
    if (sr2 & 0x1C) ACI[0x176] = (uint8_t)(sr2 & 0x1C);
    if (civ != s_aci_last_civ) {   /* a buffer finished: the engine runs */
        s_aci_dead = 0;
        s_aci_played = 1;
    }
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
            if (!s_aci_played && s_aci_resets >= 1) {
                aci_give_up(civ, sr, sr2);
                return;
            }
            s_aci_resets++;
            d.QuadPart = -10 * 1000 * (int64_t)(s_aci_resets < 10 ? s_aci_resets * 10 : 100);
            KeDelayExecutionThread(KernelMode, FALSE, &d);
            xhw_logf("[AUDIO] AC97 cold reset (%u): global control %08x status %08x", s_aci_resets,
                     (unsigned)m[0x12C >> 2], (unsigned)m[0x130 >> 2]);
            aci_reset();
            s_aci_dead = 0;
        }
        aci_start();
        s_aci_last_civ = ACI_CIV();
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
    ASET(s_pump_alive, 1);
    while (AGET(s_pump_run)) {
        if (s_apu) {
            apu_pump();
        } else if (s_aci_silent) {
            aci_drain();
        } else {
            unsigned civ, ahead;
            if (!s_aci_on) {   /* first start here, after the queue is filled */
                aci_start();
                s_aci_last_civ = ACI_CIV();
                s_aci_on = 1;
            }
            civ = ACI_CIV();
            aci_check(civ);
            if (s_aci_silent) continue;
            civ = ACI_CIV();
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
    ASET(s_pump_alive, 0);
}

/* ---- boot: the audio hardware as found, then idle ----
 * Roadmap item 9: some boots find the engine stuck from the first buffer
 * (running, sr 00, CIV never leaves 0) and no cold reset of ours brings it
 * back; a power-off does. Seen after a crash and after the dashboard
 * alone, so whatever ran before us may leave the AC97 or the APU running
 * or half set up: our own engine, or the dashboard's DirectSound (the
 * APU's DSPs write their output FIFOs to memory and the AC97 bus masters
 * play them: xboxdevwiki "APU"; register map from xemu
 * hw/xbox/mcpx/apu/apu_regs.h). The boot logs both as found, before
 * anything here touches them, then brings them to idle: the AC97's PCI
 * memory and bus-master enables on if off, both bus masters (PCM and
 * S/PDIF out) stopped and waited for (DCH), the APU's interrupts, setup
 * engine and DSPs stopped (AC97 path only: in xemu the APU plays our
 * voice), then the cold reset and bus-master reset as before, and the
 * codec powered up if it reports parts of it powered down. */
#define PCI_APU 5   /* bus 0, device 5 function 0: the MCPX APU */
#define PCI_ACI 6   /* device 6: the AC97 controller (xemu hw/xbox/xbox.c) */
#define APU_IO_SIZE 0x60000
#define AIO(o) (*(volatile uint32_t*)(apu + (o)))

static uint32_t pci_read32(unsigned slot, unsigned reg) {
    ULONG v = 0xFFFFFFFFu;
    HalReadWritePCISpace(0, slot, reg, &v, 4, FALSE);
    return v;
}

/* The APU's registers through its PCI BAR, NULL if its memory decode is off */
static volatile uint8_t* apu_io(void) {
    static volatile uint8_t* io;
    uint32_t cmd = pci_read32(PCI_APU, 0x04), bar = pci_read32(PCI_APU, 0x10) & ~0xFu;
    if (!io && (cmd & 2) && bar && bar != 0xFFFFFFF0u)
        io = (volatile uint8_t*)MmMapIoSpace(bar, APU_IO_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    return io;
}

/* A codec register over the AC-link, ICH style: reading the access
 * semaphore (CAS, ACI + 0x134) takes it, the access frees it; 0xFFFF if
 * the codec didn't answer (RCS, global status bit 15, write 1 to clear). */
static uint16_t codec_read(unsigned reg) {
    volatile uint32_t* m = (volatile uint32_t*)ACI;
    uint16_t v;
    int i;
    for (i = 0; i < 100 && (ACI[0x134] & 1); i++) KeStallExecutionProcessor(1);
    v = *(volatile uint16_t*)(ACI + reg);
    if (m[0x130 >> 2] & 0x8000) {
        m[0x130 >> 2] = 0x8000;
        return 0xFFFF;
    }
    return v;
}

static void codec_write(unsigned reg, uint16_t v) {
    int i;
    for (i = 0; i < 100 && (ACI[0x134] & 1); i++) KeStallExecutionProcessor(1);
    *(volatile uint16_t*)(ACI + reg) = v;
}

static int codec_ready(void) {
    volatile uint32_t* m = (volatile uint32_t*)ACI;
    return (m[0x12C >> 2] & 2) && (m[0x130 >> 2] & 0x100);   /* out of cold reset, primary codec ready */
}

/* Powers up the DAC, mixer, Vref and AC-link (power-down register 0x26,
 * PR1-PR5) if any of them is off, and waits (50 ms at most) for the DAC to
 * report ready. Returns register 0x26 as read before; *wrote 1 if it wrote. */
static uint16_t codec_power_up(int* wrote) {
    uint16_t pd = codec_ready() ? codec_read(0x26) : 0xFFFF;
    int i;
    *wrote = 0;
    if (pd == 0xFFFF || !(pd & 0x3E00)) return pd;
    codec_write(0x26, (uint16_t)(pd & 0xC100));   /* keep PR0 (ADC), PR6, EAPD as they were */
    for (i = 0; i < 50 && !(codec_read(0x26) & 2); i++) Sleep(1);
    *wrote = 1;
    return pd;
}

/* [AUDIO] found: the AC97 and the APU as the previous XBE left them. The
 * moving parts (CIV, PICB, the APU's sample counter, the DSPs' output
 * FIFO positions) are read twice, 5 ms apart: what moves is running. */
static void audio_found(void) {
    volatile uint32_t* m = (volatile uint32_t*)ACI;
    volatile uint8_t* apu = apu_io();
    uint32_t pci_aci = pci_read32(PCI_ACI, 0x04), pci_apu = pci_read32(PCI_APU, 0x04);
    uint32_t x0 = 0, x1 = 0, g0 = 0, g1 = 0, e0 = 0, e1 = 0;
    unsigned civ0 = ACI[0x114], sciv0 = ACI[0x174];
    unsigned picb0 = *(volatile uint16_t*)(ACI + 0x118), spicb0 = *(volatile uint16_t*)(ACI + 0x178);
    LARGE_INTEGER d;
    if (apu) {
        x0 = AIO(0x200C);   /* XGSCNT: the setup engine's sample counter */
        g0 = AIO(0x302C);   /* GPOFCUR0, EPOFCUR0: output FIFO 0 positions */
        e0 = AIO(0x402C);
    }
    d.QuadPart = -10 * 1000 * 5;
    KeDelayExecutionThread(KernelMode, FALSE, &d);
    if (apu) {
        x1 = AIO(0x200C);
        g1 = AIO(0x302C);
        e1 = AIO(0x402C);
    }
    xhw_logf("[AUDIO] found: pci aci %08x apu %08x, global control %08x status %08x, "
             "pcm bd %08x civ %u->%u lvi %u sr %04x picb %u->%u cr %02x, "
             "spdif bd %08x civ %u->%u lvi %u sr %04x picb %u->%u cr %02x",
             (unsigned)pci_aci, (unsigned)pci_apu, (unsigned)m[0x12C >> 2], (unsigned)m[0x130 >> 2],
             (unsigned)m[0x110 >> 2], civ0 & 31u, ACI[0x114] & 31u, ACI[0x115] & 31u,
             (unsigned)*(volatile uint16_t*)(ACI + 0x116), picb0, (unsigned)*(volatile uint16_t*)(ACI + 0x118),
             ACI[0x11B], (unsigned)m[0x170 >> 2], sciv0 & 31u, ACI[0x174] & 31u, ACI[0x175] & 31u,
             (unsigned)*(volatile uint16_t*)(ACI + 0x176), spicb0, (unsigned)*(volatile uint16_t*)(ACI + 0x178),
             ACI[0x17B]);
    if (apu)
        xhw_logf("[AUDIO] found: apu ists %08x ien %08x fectl %08x sectl %08x xgscnt %08x->%08x gprst %x eprst %x "
                 "gp fifo0 %08x->%08x ep fifo0 %08x->%08x (base %08x end %08x)",
                 (unsigned)AIO(0x1000), (unsigned)AIO(0x1004), (unsigned)AIO(0x1100), (unsigned)AIO(0x2000),
                 (unsigned)x0, (unsigned)x1, (unsigned)AIO(0x3FFFC), (unsigned)AIO(0x5FFFC), (unsigned)g0,
                 (unsigned)g1, (unsigned)e0, (unsigned)e1, (unsigned)AIO(0x4024), (unsigned)AIO(0x4028));
    else
        xhw_logf("[AUDIO] found: apu not mapped (pci %08x bar %08x)", (unsigned)pci_apu,
                 (unsigned)pci_read32(PCI_APU, 0x10));
    if (codec_ready())
        xhw_logf("[AUDIO] found: codec 26 %04x 2a %04x 2c %04x 02 %04x 18 %04x vendor %04x%04x", codec_read(0x26),
                 codec_read(0x2A), codec_read(0x2C), codec_read(0x02), codec_read(0x18), codec_read(0x7C),
                 codec_read(0x7E));
    else
        xhw_logf("[AUDIO] found: codec not ready");
}

/* Before aci_init's cold reset: the controller's PCI enables, both bus
 * masters halted, and (apu_too) the APU stopped. */
static void audio_idle(int apu_too, char* note, size_t cap) {
    volatile uint8_t* apu = apu_too ? apu_io() : NULL;
    uint32_t cmd = pci_read32(PCI_ACI, 0x04);
    unsigned us_pcm, us_spdif;
    int ok_pcm, ok_spdif;
    if ((cmd & 6) != 6) {   /* memory decode and bus mastering: the engine fetches nothing without the latter */
        USHORT c = (USHORT)(cmd | 6);
        HalReadWritePCISpace(0, PCI_ACI, 0x04, &c, 2, TRUE);
    }
    ACI[0x10B] = 0;   /* PCM in: never used here */
    ok_pcm = aci_halt(0x110, &us_pcm);
    ok_spdif = aci_halt(0x170, &us_spdif);
    if (apu) {
        AIO(0x1004) = 0;             /* IEN: no interrupts */
        AIO(0x2000) = 0;             /* SECTL XCNTMODE off: the setup engine (voices) stops */
        AIO(0x3FFFC) = 0;            /* GPRST, EPRST: both DSPs held in reset, their FIFO DMA stops */
        AIO(0x5FFFC) = 0;
        AIO(0x1000) = 0xFFFFFFFFu;   /* ISTS: write 1 to clear */
    }
    snprintf(note, cap, "pci aci %04x%s, halt pcm %s %u us spdif %s %u us, apu %s", (unsigned)(cmd & 0xFFFF),
             (cmd & 6) != 6 ? " (enabled)" : "", ok_pcm ? "ok" : "TIMEOUT", us_pcm, ok_spdif ? "ok" : "TIMEOUT",
             us_spdif, apu ? "stopped" : apu_too ? "not mapped" : "left (xemu voice)");
}

static int running_in_xemu(void) {
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    return !(d & (1u << 1));   /* xemu's CPU model has no VME */
}

int xhw_audio_init(uint32_t rate) {
    int i, xemu, wrote;
    uint16_t pd;
    char note[128];
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
    audio_found();
    audio_idle(!(XHW_AUDIO_APU && xemu), note, sizeof note);
    if (!aci_init()) {
        xhw_logf("[AUDIO] AC97 init failed");
        return 0;
    }
    pd = codec_power_up(&wrote);
    xhw_logf("[AUDIO] idle: %s, bus-master reset %u polls, codec 26 %04x%s", note, s_rr_polls, pd,
             wrote ? " (powered up)" : "");
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
    xhw_logf("[AUDIO] %s, %u Hz in -> 48 kHz (AC97 global status %08x)", s_apu ? "xemu APU voice" : "AC97 polled",
             s_in_rate, (unsigned)((volatile uint32_t*)ACI)[0x130 >> 2]);
    return 1;
}

void xhw_audio_stop(void) { xhw_audio_shutdown(); }

/* Before a relaunch or the dashboard: the pump has stopped (it could
 * restart the engine after a bare run-bit clear), then the bus masters are
 * stopped and reset, so the next XBE takes over an idle AC97. v47 on the
 * console: after the settings menu's restart the next boot's engine never
 * left descriptor 0 ("AC97 stuck: civ 0") and cold resets never brought
 * the codec back; only a power-off did. */
void xhw_audio_shutdown(void) {
    int i;
    if (!s_started) return;
    ASET(s_pump_run, 0);
    for (i = 0; i < 200 && AGET(s_pump_alive); i++) Sleep(1);
    if (s_apu) {
        APU_PIO(0x128, APU_VOICE);   /* VOICE_OFF */
    } else if (XHW_AUDIO_TEST & 1) {
        /* test: the engine keeps running into the next boot, every
         * descriptor ~0.7 s long (whatever memory they point at by then) */
        uint32_t phys = MmGetPhysicalAddress(s_outbuf[0]);
        unsigned civ = ACI[0x114] & 31u;
        for (i = 0; i < 32; i++) {
            s_desc_pcm[i].addr = s_desc_spdif[i].addr = phys;
            s_desc_pcm[i].samples = s_desc_spdif[i].samples = 0xFFFE;
            s_desc_pcm[i].ctl = s_desc_spdif[i].ctl = 0;
        }
        ACI[0x115] = ACI[0x175] = (uint8_t)((civ + 31) & 31);
        aci_run(1);
        xhw_log_try("[AUDIO] test: engine left running");
    } else {
        aci_run(0);
        aci_bm_reset();
    }
    s_started = 0;
}
