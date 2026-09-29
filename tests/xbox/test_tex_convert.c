/* test_tex_convert.c - host check of gx_tex.c's native-format conversion:
 * every native texture (DXT1, AY8, A8Y8, RGB565) is expanded back to ARGB
 * and compared with gx_tex.c's own A8R8G8B8 decoder. Built and run by
 * tools/xbox/test_tex_convert.py. */
#include <stdio.h>
#include <stdlib.h>

XgxState g_xgx;
static uint32_t s_fmt, s_w, s_h, s_levels;
static uint8_t s_data[1 << 20];
uint32_t xgx_tex_create(uint32_t w, uint32_t h, uint32_t levels, uint32_t fmt, const void* data) {
    uint32_t n = 0, l, lw = w, lh = h;
    for (l = 0; l < levels; l++) {
        n += fmt == XGX_TEX_DXT1 ? ((lw + 3) / 4) * ((lh + 3) / 4) * 8
                                 : lw * lh * (fmt == XGX_TEX_AY8 ? 1 : fmt == XGX_TEX_ARGB8 ? 4 : 2);
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }
    memcpy(s_data, data, n);
    s_fmt = fmt, s_w = w, s_h = h, s_levels = levels;
    return 1;
}
void xgx_tex_destroy(uint32_t tex) { (void)tex; }
uint32_t xgx_tex_from_efb(const int32_t src[4], uint32_t w, uint32_t h, int i, uint32_t reuse) { return 0; }
void xgx_read_efb(const int32_t src[4], uint32_t w, uint32_t h, uint8_t* rgba) {}
void xhw_logf(const char* fmt, ...) {}
void xhw_log(const char* s) {}
void gx_vtx_flush(void) {}

/* reference expansion of the back-end formats, as the NV2A samples them */
static uint32_t c565(uint16_t v) { return rgb565(v); }
static uint32_t lerp(uint32_t a, uint32_t b, int wa, int wb, int d) {
    uint32_t r = 0;
    int k;
    for (k = 0; k < 24; k += 8) r |= (uint32_t)((((a >> k) & 255) * wa + ((b >> k) & 255) * wb) / d) << k;
    return r | 0xFF000000u;
}
static uint32_t expand(const uint8_t* d, uint32_t fmt, uint32_t w, uint32_t x, uint32_t y) {
    switch (fmt) {
        case XGX_TEX_AY8: { uint32_t v = d[y * w + x]; return argb(v, v, v, v); }
        case XGX_TEX_A8Y8: { const uint8_t* p = d + (y * w + x) * 2; return argb(p[1], p[0], p[0], p[0]); }
        case XGX_TEX_RGB565: { uint16_t v; memcpy(&v, d + (y * w + x) * 2, 2); return c565(v); }
        default: {
            const uint8_t* b = d + ((y / 4) * ((w + 3) / 4) + x / 4) * 8;
            uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
            uint32_t i = (b[4 + (y & 3)] >> ((x & 3) * 2)) & 3, p0 = c565(c0), p1 = c565(c1);
            if (i == 0) return p0;
            if (i == 1) return p1;
            if (c0 > c1) return i == 2 ? lerp(p0, p1, 2, 1, 3) : lerp(p0, p1, 1, 2, 3);
            return i == 2 ? lerp(p0, p1, 1, 1, 2) : 0;
        }
    }
}

static int check(uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels) {
    static uint8_t src[1 << 20];
    static uint32_t ref[1 << 18];
    TexObj o;
    uint32_t i, l, lw = w, lh = h, bad = 0, off = 0, doff = 0;
    for (i = 0; i < sizeof src; i++) src[i] = (uint8_t)rand();
    memset(&o, 0, sizeof o);
    o.data = src, o.w = (uint16_t)w, o.h = (uint16_t)h, o.fmt = (uint8_t)fmt;
    if (!upload(&o, NULL, levels, 0)) return printf("fmt %u %ux%u: upload failed\n", fmt, w, h), 1;
    for (l = 0; l < levels; l++) {
        uint32_t x, y;
        decode_level(src + off, fmt, lw, lh, NULL, ref);
        for (y = 0; y < lh; y++)
            for (x = 0; x < lw; x++) {
                uint32_t got = expand(s_data + doff, s_fmt, lw, x, y), want = ref[y * lw + x];
                if (s_fmt == XGX_TEX_ARGB8) memcpy(&got, s_data + doff + (y * lw + x) * 4, 4);
                if (got != want && bad++ < 4)
                    printf("fmt %u %ux%u level %u (%u,%u): got %08x want %08x\n", fmt, w, h, l, x, y, got, want);
            }
        off += level_bytes(fmt, lw, lh);
        doff += native_size(s_fmt, lw, lh);
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }
    return bad != 0;
}

int main(void) {
    static const uint32_t fmts[] = {GX_TF_CMPR, GX_TF_I4, GX_TF_I8, GX_TF_IA4, GX_TF_IA8, GX_TF_RGB565};
    static const uint32_t sizes[][2] = {{8, 8}, {16, 8}, {64, 32}, {4, 4}, {128, 128}, {32, 256}, {24, 16}};
    int fail = 0;
    uint32_t f, s;
    for (f = 0; f < sizeof fmts / sizeof *fmts; f++)
        for (s = 0; s < sizeof sizes / sizeof *sizes; s++) {
            fail |= check(fmts[f], sizes[s][0], sizes[s][1], 1);
            fail |= check(fmts[f], sizes[s][0], sizes[s][1], 4);
        }
    if (native_fmt(&(TexObj){.w = 64, .h = 64, .fmt = GX_TF_CMPR}) != XGX_TEX_DXT1) fail = 1, puts("CMPR not DXT1");
    puts(fail ? "FAIL" : "ok: native texture formats match the ARGB decoder");
    return fail;
}
