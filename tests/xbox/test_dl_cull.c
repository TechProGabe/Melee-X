/* test_dl_cull.c - host check of gx_cull.h (gx_dl_culled's box test with the
 * clip planes kept per projection) against tests/xbox/dl_cull_ref.c, the
 * planes made again for every box. Built and run by
 * tools/xbox/test_dl_cull.py. Projections shaped as GXSetProjection's
 * (perspective, off-centre, orthographic) and random ones, changing every
 * few boxes (a new generation each time, as GXSetProjection counts them),
 * sometimes by one word (an ulp, a sign, -0); model-view matrices
 * of joints in a scene and random ones; boxes all around the view volume.
 * Every answer must be the same, and the planes the same bits (NaNs only
 * NaNs: their sign is the compiler's). Then special values: zeros, -0,
 * denormals, infinities and NaNs anywhere. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_cull.h"

extern float ref_proj[4][4];
void ref_planes(float out[5][4]);
int ref_culled(const float bmin[3], const float bmax[3], const float mtx[3][4]);

static uint32_t s_rng = 0x9E3779B9u;
static uint32_t rnd(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
static float frand(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() >> 8) * (1.0f / 16777216.0f); }
static uint32_t bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static float from_bits(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static float special(void) {
    static const uint32_t k[] = { 0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x7F800000u, 0xFF800000u,
                                  0x7FC00000u, 0xFFC00001u, 0x7F7FFFFFu, 0x00800000u, 0x3F800000u, 0xBF800000u };
    return rnd() & 1 ? from_bits(k[rnd() % (sizeof k / sizeof k[0])]) : from_bits(rnd());
}

static void make_proj(float p[4][4], int kind) {
    memset(p, 0, sizeof(float) * 16);
    if (kind == 0 || kind == 1) {   /* perspective, centred or not */
        float n = frand(0.1f, 50.0f), f = n + frand(10.0f, 100000.0f), t = n * frand(0.1f, 2.0f),
              b = kind ? -t * frand(0.5f, 1.5f) : -t, r = t * frand(0.5f, 2.0f), l = kind ? -r * frand(0.5f, 1.5f) : -r;
        p[0][0] = 2 * n / (r - l);
        p[0][2] = (r + l) / (r - l);
        p[1][1] = 2 * n / (t - b);
        p[1][2] = (t + b) / (t - b);
        p[2][2] = -n / (f - n);
        p[2][3] = -(f * n) / (f - n);
        p[3][2] = -1.0f;
    } else if (kind == 2) {         /* orthographic (the HUD, shadow maps) */
        float t = frand(1.0f, 500.0f), b = -frand(1.0f, 500.0f), r = frand(1.0f, 700.0f), l = -frand(1.0f, 700.0f),
              n = frand(-100.0f, 10.0f), f = n + frand(1.0f, 10000.0f);
        p[0][0] = 2 / (r - l);
        p[0][3] = -(r + l) / (r - l);
        p[1][1] = 2 / (t - b);
        p[1][3] = -(t + b) / (t - b);
        p[2][2] = -1 / (f - n);
        p[2][3] = -f / (f - n);
        p[3][3] = 1.0f;
    } else {                        /* anything */
        int i, j;
        for (i = 0; i < 4; i++)
            for (j = 0; j < 4; j++) p[i][j] = rnd() & 3 ? frand(-4.0f, 4.0f) : 0.0f;
    }
}

static void make_mtx(float m[3][4]) {
    float a = frand(-3.2f, 3.2f), b = frand(-3.2f, 3.2f), c = frand(-3.2f, 3.2f), s = rnd() % 8 ? frand(0.01f, 20.0f) : frand(-5.0f, 5.0f);
    float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b), cc = cosf(c), sc = sinf(c);
    float r[3][3] = { { cb * cc, -cb * sc, sb },
                      { sa * sb * cc + ca * sc, -sa * sb * sc + ca * cc, -sa * cb },
                      { -ca * sb * cc + sa * sc, ca * sb * sc + sa * cc, ca * cb } };
    int i, j;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) m[i][j] = rnd() % 16 ? r[i][j] * s : 0.0f;
    m[0][3] = frand(-1500.0f, 1500.0f);
    m[1][3] = frand(-1000.0f, 1000.0f);
    m[2][3] = rnd() % 4 ? frand(-3000.0f, 20.0f) : frand(-50.0f, 50.0f);
}

static void make_box(float bmin[3], float bmax[3]) {
    int k;
    for (k = 0; k < 3; k++) {
        float c = frand(-300.0f, 300.0f), h = rnd() % 8 ? frand(0.0f, 80.0f) : 0.0f;
        bmin[k] = c - h;
        bmax[k] = c + h;
    }
    if (rnd() % 64 == 0) {   /* never filled: 1e30 .. -1e30 */
        for (k = 0; k < 3; k++) {
            bmin[k] = 1e30f;
            bmax[k] = -1e30f;
        }
    }
}

int main(void) {
    static GxCullPlanes planes;
    float proj[4][4], mtx[3][4], bmin[3], bmax[3];
    long long n = 0, culled = 0, recomputed = 0, bad = 0;
    uint32_t gen = 0;
    int round;
    for (round = 0; round < 400000; round++) {
        int kind = (int)(rnd() % 5), boxes = 1 + (int)(rnd() % 60), wild = round >= 300000, b;
        if (round == 0 || rnd() % 4) {
            make_proj(proj, kind > 3 ? 0 : kind);
        } else {   /* one word nudged: an ulp, the sign, a zero's sign */
            int i = (int)(rnd() % 16);
            uint32_t u = bits(proj[i / 4][i % 4]);
            u = rnd() & 1 ? u ^ 0x80000000u : u + 1;
            proj[i / 4][i % 4] = from_bits(u);
        }
        if (wild) {
            int i = (int)(rnd() % 16);
            proj[i / 4][i % 4] = special();
        }
        /* GXSetProjection: a new generation when the bits change (GXInit
         * starts one without a change) */
        if (memcmp(ref_proj, proj, sizeof proj) != 0 || rnd() % 64 == 0) {
            gen++;
            recomputed++;
        }
        memcpy(ref_proj, proj, sizeof proj);
        gx_cull_planes(&planes, (const float(*)[4])proj, gen);
        {   /* the planes, bit for bit */
            float rp[5][4];
            int i, k;
            ref_planes(rp);
            for (i = 0; i < 5; i++)
                for (k = 0; k < 4; k++) {
                    float x = planes.n[i][k], y = rp[i][k];
                    if (isnan(x) && isnan(y)) continue;
                    if (bits(x) != bits(y) || bits(planes.an[i][k < 3 ? k : 0]) != bits(fabsf(planes.n[i][k < 3 ? k : 0]))) {
                        if (bad++ < 10)
                            printf("FAIL plane %d.%d: %08x, reference %08x\n", i, k, bits(x), bits(y));
                    }
                }
        }
        for (b = 0; b < boxes; b++) {
            int r1, r2, k, j;
            if (b == 0 || rnd() % 3 == 0) make_mtx(mtx);
            make_box(bmin, bmax);
            if (wild) {
                for (k = 0; k < 3; k++) {
                    if (rnd() % 8 == 0) bmin[k] = special();
                    if (rnd() % 8 == 0) bmax[k] = special();
                    for (j = 0; j < 4; j++)
                        if (rnd() % 16 == 0) mtx[k][j] = special();
                }
            }
            r1 = gx_cull_box(gx_cull_planes(&planes, (const float(*)[4])proj, gen), bmin, bmax, (const float(*)[4])mtx);
            r2 = ref_culled(bmin, bmax, (const float(*)[4])mtx);
            n++;
            culled += r1;
            if (r1 != r2 && bad++ < 10)
                printf("FAIL box %lld: culled %d, reference %d\n", n, r1, r2);
        }
    }
    printf("%lld boxes (%lld culled), %lld projection generations\n", n, culled, recomputed);
    if (bad) {
        printf("FAIL: %lld differences\n", bad);
        return 1;
    }
    printf("ok: the same answers and planes as the reference\n");
    return 0;
}
