/* xhw_code_repair.c - puts the game's code back the way default.xbe has it
 * (OpenCrossing-Xbox's xbox_code_repair.c, MIT, as Wings64-X carries it).
 *
 * On CPU-upgraded and overclocked consoles (OpenCrossing GitHub #2/#3: a
 * 1 GHz Coppermine and a 1.4 GHz Tualatin) the BIOS (Cerbios 3.x
 * AdvCPUSupport, on by default) rewrites `0f 31` (rdtsc) as `cd 2e`
 * (int 2Eh) in the title's code, by signature, to slow down games that time
 * themselves with the TSC. It matched a pair in OpenCrossing that was no
 * rdtsc at all, and the game faulted. Melee-X keeps time with the ACPI timer
 * (xhw_sys.c), but its .text has ~55 `0f 31` pairs: the profiler's and the
 * interrupt lock's rdtsc (xhw_perf.c, os.c), run thousands of times a frame
 * (each a trap once rewritten), and byte pairs inside other instructions,
 * one of them in Sheik's needles (docs/roadmap.md).
 *
 * Nothing in .text is ours to have changed after load (no relocations; the
 * kernel's import thunks are in .rdata), so at boot every byte that differs
 * from the file is put back, before anything reads the TSC. A big difference
 * means something else (the wrong file on D:): that is logged and left alone.
 * Switches: -DXHW_CODE_REPAIR=0 (not built), =2 (check and log only),
 * -DXHW_CODE_REPAIR_TEST (patch one pair the way the BIOS does, then repair). */
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <stdlib.h>
#include <string.h>

#include "xhw.h"
#include "xhw_internal.h"

#ifndef XHW_CODE_REPAIR
#define XHW_CODE_REPAIR 1
#endif

#if XHW_CODE_REPAIR
#define XBE_PATH "D:\\default.xbe"
#define CHUNK 65536
#define MAX_FIX 64   /* more than this and it isn't a patcher */
#define LOG_FIX 8

static struct {
    unsigned va;
    unsigned char ram, disk;
} s_fix[MAX_FIX];

/* one byte of code. The kernel maps .text read-only and CR0.WP makes that
 * hold in ring 0 too (MmSetAddressProtect on it faulted the write in xemu),
 * so WP is cleared around the store, with interrupts off. */
static void put_byte(unsigned va, unsigned char v) {
    unsigned cr0, tmp;
    __asm__ volatile("pushfl\n\t"
                     "cli\n\t"
                     "movl %%cr0, %0\n\t"
                     "movl %0, %1\n\t"
                     "andl $0xfffeffff, %1\n\t"
                     "movl %1, %%cr0\n\t"
                     "movb %b3, (%2)\n\t"
                     "movl %0, %%cr0\n\t"
                     "popfl"
                     : "=&r"(cr0), "=&r"(tmp)
                     : "r"(va), "q"(v)
                     : "memory", "cc");
}

/* the .text section header, from the image header the kernel loaded */
static const XBE_SECTION_HEADER* text_section(void) {
    const XBE_FILE_HEADER* h = (const XBE_FILE_HEADER*)0x00010000;
    const XBE_SECTION_HEADER* s = h->PointerToSectionTable;
    unsigned i;
    for (i = 0; i < h->NumberOfSections; i++, s++)
        if (s->SectionName && strcmp((const char*)s->SectionName, ".text") == 0) return s;
    return NULL;
}

#ifdef XHW_CODE_REPAIR_TEST
/* test builds: rewrite the first rdtsc the way the BIOS does, so a boot in
 * xemu or on a stock console shows the repair */
static void fake_patch(const XBE_SECTION_HEADER* s) {
    const unsigned char* p = (const unsigned char*)s->VirtualAddress;
    unsigned k;
    for (k = 0; k + 2 <= s->FileSize; k++) {
        if (p[k] != 0x0f || p[k + 1] != 0x31) continue;
        put_byte(s->VirtualAddress + k, 0xcd);
        put_byte(s->VirtualAddress + k + 1, 0x2e);
        xhw_logf("[BOOT] code check test: patched %08x like an upgraded-CPU BIOS", s->VirtualAddress + k);
        return;
    }
    xhw_logf("[BOOT] code check test: no 0f 31 found");
}
#endif

void xhw_code_repair(void) {
    const XBE_SECTION_HEADER* s = text_section();
    DWORD t0 = GetTickCount(), got;
    HANDLE h;
    unsigned char* buf;
    unsigned off, n, diff = 0, i;
    int ok = 1;

    if (!s) {
        xhw_logf("[BOOT] code check: no .text section in the image header");
        return;
    }
#ifdef XHW_CODE_REPAIR_TEST
    fake_patch(s);
#endif
    h = CreateFileA(XBE_PATH, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        xhw_logf("[BOOT] code check: could not open %s (error %lu)", XBE_PATH, (unsigned long)GetLastError());
        return;
    }
    buf = malloc(CHUNK);
    if (!buf || SetFilePointer(h, (LONG)s->FileAddress, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        free(buf);
        CloseHandle(h);
        return;
    }
    n = s->FileSize;
    for (off = 0; off < n; off += CHUNK) {
        unsigned len = n - off < CHUNK ? n - off : CHUNK, k;
        const unsigned char* ram = (const unsigned char*)(s->VirtualAddress + off);
        if (!ReadFile(h, buf, len, &got, NULL) || got != len) {
            ok = 0;
            break;
        }
        if (memcmp(ram, buf, len) == 0) continue;
        for (k = 0; k < len; k++) {
            if (ram[k] == buf[k]) continue;
            if (diff < MAX_FIX) {
                s_fix[diff].va = s->VirtualAddress + off + k;
                s_fix[diff].ram = ram[k];
                s_fix[diff].disk = buf[k];
            }
            diff++;
        }
    }
    free(buf);
    CloseHandle(h);

    if (!ok) {
        xhw_logf("[BOOT] code check: read of %s failed at .text+%x (error %lu)", XBE_PATH, off,
                 (unsigned long)GetLastError());
        return;
    }
    if (!diff) {
        xhw_logf("[BOOT] code check: .text matches %s (%lu ms)", XBE_PATH, (unsigned long)(GetTickCount() - t0));
        return;
    }
    if (diff > MAX_FIX) {
        xhw_logf("[BOOT] code check: %u bytes of .text differ from %s, first %08x; not a patcher, left alone", diff,
                 XBE_PATH, s_fix[0].va);
        return;
    }
    if (XHW_CODE_REPAIR != 2)
        for (i = 0; i < diff; i++) put_byte(s_fix[i].va, s_fix[i].disk);
    xhw_logf("[BOOT] code check: %u bytes of .text were changed in memory after load (BIOS patcher?), %s (%lu ms)",
             diff, XHW_CODE_REPAIR != 2 ? "put back" : "left alone (XHW_CODE_REPAIR=2)",
             (unsigned long)(GetTickCount() - t0));
    for (i = 0; i < diff && i < LOG_FIX; i++)
        xhw_logf("[BOOT]   %08x: %02x, file %02x%s", s_fix[i].va, s_fix[i].ram, s_fix[i].disk,
                 XHW_CODE_REPAIR != 2 && *(volatile unsigned char*)s_fix[i].va != s_fix[i].disk
                     ? " (write did not stick)"
                     : "");
}
#else
void xhw_code_repair(void) {}
#endif
