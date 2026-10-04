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
 * Off unless built with -DXHW_FBDUMP_EVERY=N (every N presents).
 *
 * On the console there is no COM1: BACK on any controller, and an autopad
 * script's SHOT, write the next frame to E:\UDATA\4d580001\shotNN.bmp
 * instead (xhw_fbdump_file; shot_<folder>_NN.bmp in a console round's
 * chained builds), fetched over FTP with the logs. */
#include <stdio.h>
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

/* 24-bit bottom-up BMP, rows converted one at a time (the framebuffer is
 * write-combined: read each row once, in order) */
void xhw_fbdump_file(const void* fb, int w, int h, int bpp, int pitch) {
    static unsigned s_n;
    static unsigned char row[1280 * 3 + 4];
    char path[96];
    unsigned char hdr[54];
    unsigned stride = ((unsigned)w * 3 + 3) & ~3u, size = 54 + stride * (unsigned)h;
    DWORD done;
    HANDLE f;
    int x, y;
    char folder[40];
    if (w > 1280 || w <= 0 || h <= 0) return;
    /* a console round's chained builds (env MX_NEXT_XBE) name theirs after
     * their folder, so the next build's don't overwrite them */
    if (getenv("MX_NEXT_XBE") && xhw_image_folder(folder, sizeof folder))
        snprintf(path, sizeof path, XHW_UDATA_DIR "shot_%s_%02u.bmp", folder, s_n % 100);
    else
        snprintf(path, sizeof path, XHW_UDATA_DIR "shot%02u.bmp", s_n % 100);
    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        xhw_logf("[SHOT] could not create %s", path);
        return;
    }
    xhw_watchdog_busy(1);
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    WriteFile(f, hdr, sizeof hdr, &done, NULL);
    memset(row, 0, sizeof row);
    for (y = h - 1; y >= 0; y--) {
        const unsigned char* src = (const unsigned char*)fb + (size_t)y * (size_t)pitch;
        for (x = 0; x < w; x++) {
            unsigned char* d = row + x * 3;
            if (bpp == 16) {
                unsigned v = ((const unsigned short*)src)[x], r = v >> 11, g = (v >> 5) & 63, b = v & 31;
                d[0] = (unsigned char)(b << 3 | b >> 2);
                d[1] = (unsigned char)(g << 2 | g >> 4);
                d[2] = (unsigned char)(r << 3 | r >> 2);
            } else {
                unsigned v = ((const unsigned*)src)[x];
                d[0] = (unsigned char)v;
                d[1] = (unsigned char)(v >> 8);
                d[2] = (unsigned char)(v >> 16);
            }
        }
        WriteFile(f, row, stride, &done, NULL);
    }
    CloseHandle(f);
    xhw_watchdog_busy(0);
    xhw_logf("[SHOT] wrote %s", path);
    s_n++;
}
