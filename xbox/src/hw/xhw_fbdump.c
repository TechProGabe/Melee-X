/* xhw_fbdump.c - screenshots over the log (from OpenCrossing-Xbox
 * xbox_fbdump.c; xemu has no screendump command).
 *
 * One frame is emitted as
 *     [FBDUMP] BEGIN w h bpp pitch
 *     [FBDUMP] <base64 of zlib(deflate) pixels>   (many lines)
 *     [FBDUMP] END
 * and tools/xbox/fbdump_to_png.py turns a serial log into PNGs. The NV2A's
 * colour buffer is plain system memory, so this reads it directly. The data
 * lines go to COM1 only (boot.log flushes every line).
 * Off unless built with -DXHW_FBDUMP_EVERY=N (every N presents). */
#include <stdlib.h>
#include <string.h>
#define Z_SOLO   /* how nxdk builds libzlib: no compress.c, caller-supplied allocator */
#include <zlib.h>

#include "xhw.h"
#include "xhw_internal.h"

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* each line is one log call, so lines from other threads land between
 * lines, never inside one */
static void emit_b64(const unsigned char* p, size_t n) {
    char line[9 + 76 + 1];
    size_t i = 0;
    while (i < n) {
        int k = 9;
        memcpy(line, "[FBDUMP] ", 9);
        while (i < n && k < 9 + 76) {
            unsigned v = (unsigned)p[i] << 16;
            int m = 1;
            if (i + 1 < n) {
                v |= (unsigned)p[i + 1] << 8;
                m++;
            }
            if (i + 2 < n) {
                v |= p[i + 2];
                m++;
            }
            line[k++] = B64[(v >> 18) & 63];
            line[k++] = B64[(v >> 12) & 63];
            line[k++] = m > 1 ? B64[(v >> 6) & 63] : '=';
            line[k++] = m > 2 ? B64[v & 63] : '=';
            i += 3;
        }
        line[k] = '\0';
        xhw_log_com1_line(line);
    }
}

static voidpf z_alloc(voidpf o, uInt n, uInt sz) {
    (void)o;
    return calloc(n, sz);
}
static void z_free(voidpf o, voidpf p) {
    (void)o;
    free(p);
}

void xhw_fbdump(const void* fb, int w, int h, int bpp, int pitch) {
    /* streamed: a whole compressed frame needn't fit the heap at once */
    enum { ZBUF = 32 * 1024 };
    static unsigned char out[ZBUF + 64];
    z_stream zs;
    size_t have = 0;   /* bytes in out[] not yet base64'd */
    int rc, y;
    memset(&zs, 0, sizeof zs);
    zs.zalloc = z_alloc;
    zs.zfree = z_free;
    if (deflateInit2(&zs, 1, Z_DEFLATED, 9, 1, Z_DEFAULT_STRATEGY) != Z_OK) {
        xhw_log("[FBDUMP] ERROR deflateInit");
        return;
    }
    xhw_watchdog_busy(1);
    xhw_logf("[FBDUMP] BEGIN %d %d %d %d", w, h, bpp, pitch);
    for (y = 0; y <= h; y++) {
        int flush = y == h ? Z_FINISH : Z_NO_FLUSH;
        zs.next_in = y < h ? (Bytef*)fb + (size_t)y * (size_t)pitch : (Bytef*)fb;
        zs.avail_in = y < h ? (uInt)pitch : 0;
        do {
            size_t emit;
            zs.next_out = out + have;
            zs.avail_out = (uInt)(ZBUF - have);
            rc = deflate(&zs, flush);
            have = ZBUF - zs.avail_out;
            /* base64 whole 57-byte lines only; keep the tail for the next pass */
            emit = rc == Z_STREAM_END ? have : have - have % 57;
            if (emit) {
                emit_b64(out, emit);
                memmove(out, out + emit, have - emit);
                have -= emit;
            }
        } while (zs.avail_out == 0 || (flush == Z_FINISH && rc != Z_STREAM_END));
    }
    xhw_log("[FBDUMP] END");
    deflateEnd(&zs);
    xhw_watchdog_busy(0);
}
