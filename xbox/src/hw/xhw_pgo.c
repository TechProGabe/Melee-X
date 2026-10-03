/* xhw_pgo.c - the instrumented build's counters out over COM1
 * (docs/fps-plan.md A4; -DXHW_PGO=1, with XBOX_PGO=gen: docs/pgo.md).
 *
 * -fprofile-generate code counts into the .lprfc section and refers to
 * __llvm_profile_runtime, which compiler-rt would define and which this
 * image does not have. Defining it here is all the runtime it needs: the
 * function records (.lprfd) and names (.lprfn) are constant and are read
 * from the linked image on the host. At each scene's exit (and the match's
 * end) the counters so far go to the log as [PGOC] lines, nonzero 64-bit
 * counters as hex in runs ("+n" skips n zero counters);
 * tools/xbox/pgo_raw.py makes a .profraw of the last complete dump. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xhw.h"
#include "xhw_internal.h"

#ifndef XHW_PGO
#define XHW_PGO 0
#endif

#if XHW_PGO
int __llvm_profile_runtime;

/* an XBE section by name: +0x11C count, +0x120 headers (56 bytes: flags,
 * address, size, raw address, raw size, name address, ...) */
static const uint8_t* xbe_section(const char* name, uint32_t* size) {
    const uint8_t* xbe = (const uint8_t*)xhw_image_base;
    const uint8_t* sh = (const uint8_t*)*(const uint32_t*)(xbe + 0x120);
    uint32_t n = *(const uint32_t*)(xbe + 0x11C), i;
    for (i = 0; i < n && i < 64; i++, sh += 56) {
        const char* s = (const char*)*(const uint32_t*)(sh + 20);
        if (s && !strcmp(s, name)) {
            *size = *(const uint32_t*)(sh + 8);
            return (const uint8_t*)*(const uint32_t*)(sh + 4);
        }
    }
    return NULL;
}

static char s_buf[16384];
static int s_len;

static void out(const char* s) {
    size_t n = strlen(s);
    if (s_len + (int)n + 2 >= (int)sizeof s_buf) {
        xhw_log(s_buf);
        s_len = 0;
    }
    if (s_len) s_buf[s_len++] = '\n';
    memcpy(s_buf + s_len, s, n + 1);
    s_len += (int)n;
}

void xhw_pgo_dump(const char* why) {
    static uint32_t s_dumps;
    char line[600];
    uint32_t size = 0, n, i, k = 0, nonzero = 0, sum = 0;
    const uint64_t* c = (const uint64_t*)xbe_section(".lprfc", &size);
    if (!c) {
        xhw_logf("[PGOC] no .lprfc section (not a -fprofile-generate build?)");
        return;
    }
    n = size / 8;
    s_len = 0;
    snprintf(line, sizeof line, "[PGOC] begin %lu %s: counters %08lx, %lu", (unsigned long)++s_dumps, why,
             (unsigned long)(uintptr_t)c, (unsigned long)n);
    out(line);
    while (k < n) {
        int len = snprintf(line, sizeof line, "[PGOC] %lx", (unsigned long)k), gap = 0;
        while (k < n && len < 480) {
            if (!c[k]) {
                gap++;
                k++;
                continue;
            }
            if (gap) len += snprintf(line + len, sizeof line - (size_t)len, " +%x", gap);
            gap = 0;
            len += snprintf(line + len, sizeof line - (size_t)len, " %lx%08lx", (unsigned long)(c[k] >> 32),
                            (unsigned long)c[k]);
            sum += (uint32_t)c[k];
            nonzero++;
            k++;
        }
        if (gap && k < n) len += snprintf(line + len, sizeof line - (size_t)len, " +%x", gap);
        out(line);
    }
    snprintf(line, sizeof line, "[PGOC] end %lu: %lu nonzero, sum %08lx", (unsigned long)s_dumps,
             (unsigned long)nonzero, (unsigned long)sum);
    out(line);
    xhw_log(s_buf);
    s_len = 0;
}
#else
void xhw_pgo_dump(const char* why) { (void)why; }
#endif
