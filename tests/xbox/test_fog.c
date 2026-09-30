/* test_fog.c - host check of GX fog on the NV2A (nv2a_fog.c): the fog amount
 * the port's vertex program and fog unit produce, against a reference GX
 * (libogc GX_SetFog's registers, Dolphin's per-pixel formula on the 24-bit
 * EFB depth), for every fog type, perspective and orthographic, over a range
 * of depths; LIN fog also along triangle edges (perspective-correct
 * interpolation); and the final combiner's blend against GX's. Built and run
 * by tools/xbox/test_fog.py.
 *
 * GX itself resolves perspective fog coarsely at depth: ze divides by
 * b_mag - (Zs >> b_shift), a few hundred steps at Melee's stage depths with
 * its 0.1..16384 camera. A vertex passes when the port's amount is within
 * 1/255 of what GX gives over one step of that depth (Zs +- 2^b_shift). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOL (1.0 / 255.0)
/* Along an edge the port interpolates the vertex values linearly in view
 * space (perspective-correct). GX's ze is linear there only up to b_mag's
 * rounding of B (8388638 / 2^23): that bends ze by about 3.6e-6 * far / near
 * over the camera's range, which is small for most cameras but 60% for
 * Melee's 0.1..16384. Edges are checked where the bend is under 1%; for the
 * others the worst difference is reported (docs/renderer.md). */
#define EDGE_BEND_MAX 0.01

/* ---- reference GX ---- */
typedef struct {
    uint32_t a, b_mag, b_shift, c, proj, fsel;   /* a, c: sign, exponent, 11 mantissa bits (BP 0xEE, 0xF1) */
} GxFog;

/* libogc GX_SetFog */
static void gx_set_fog(uint32_t type, float startz, float endz, float nearz, float farz, GxFog* r) {
    float A, B, B_mant, C, A_f;
    uint32_t b_expn, b_m, proj = (type >> 3) & 1;
    union { float f; uint32_t i; } v;
    if (proj) {
        if (farz == nearz || endz == startz) {
            A_f = 0.0f;
            C = 0.0f;
        } else {
            A = 1.0f / (endz - startz);
            A_f = (farz - nearz) * A;
            C = (startz - nearz) * A;
        }
        b_expn = 0;
        b_m = 0;
    } else {
        if (farz == nearz || endz == startz) {
            A = 0.0f;
            B = 0.5f;
            C = 0.0f;
        } else {
            A = (farz * nearz) / ((farz - nearz) * (endz - startz));
            B = farz / (farz - nearz);
            C = startz / (endz - startz);
        }
        B_mant = B;
        b_expn = 1;
        while (B_mant > 1.0f) {
            B_mant /= 2.0f;
            b_expn++;
        }
        while (B_mant > 0.0f && B_mant < 0.5f) {
            B_mant *= 2.0f;
            b_expn--;
        }
        A_f = A / (1 << b_expn);
        b_m = (uint32_t)(B_mant * 8388638.0f);
    }
    v.f = A_f;
    r->a = v.i >> 12;
    v.f = C;
    r->c = v.i >> 12;
    r->b_mag = b_m & 0xFFFFFF;
    r->b_shift = b_expn & 0x1F;
    r->proj = proj;
    r->fsel = type & 7;
}

static float gx_reg_float(uint32_t r20) {
    union { float f; uint32_t i; } v;
    v.i = r20 << 12;
    return v.f;
}

/* Dolphin PixelShaderGen WriteFog: the fog amount (0..1) at a pixel of EFB depth zs */
static double gx_fog_amount(const GxFog* r, uint32_t zs) {
    double ze, fog;
    if (!r->fsel) return 0.0;
    if (!r->proj) ze = gx_reg_float(r->a) * 16777216.0 / (double)((int32_t)r->b_mag - (int32_t)(zs >> r->b_shift));
    else ze = gx_reg_float(r->a) * (double)zs / 16777216.0;
    fog = ze - gx_reg_float(r->c);
    fog = fog < 0.0 ? 0.0 : fog > 1.0 ? 1.0 : fog;
    switch (r->fsel) {
        case 4: fog = 1.0 - exp2(-8.0 * fog); break;
        case 5: fog = 1.0 - exp2(-8.0 * fog * fog); break;
        case 6: fog = exp2(-8.0 * (1.0 - fog)); break;
        case 7: fog = exp2(-8.0 * (1.0 - fog) * (1.0 - fog)); break;
    }
    return fog;
}

/* GX's EFB depth of a view-space point: clip z/w (-1 near .. 0 far) through the viewport */
static uint32_t gx_depth(const float p[4][4], float vn, float vf, const double P[4]) {
    double z = 0, w = 0, d;
    int c;
    for (c = 0; c < 4; c++) {
        z += p[2][c] * P[c];
        w += p[3][c] * P[c];
    }
    d = (z / w * (vf - vn) + vf) * 16777215.0;
    d = floor(d + 0.5);
    return d < 0 ? 0u : d > 16777215.0 ? 16777215u : (uint32_t)d;
}

/* ---- the port ---- */
typedef struct {
    float zrow[4], wrow[4], zmax;
    FogSetup fs;
} Port;

/* nv2a.c build_proj's depth and w rows, then fog_setup */
static void port_setup(Port* pt, const float p[4][4], float vn, float vf, float zmax, uint32_t type, float s, float e,
                       float n, float f) {
    int c;
    pt->zmax = zmax;
    for (c = 0; c < 4; c++) {
        pt->zrow[c] = zmax * ((vf - vn) * p[2][c] + vf * p[3][c]);
        pt->wrow[c] = p[3][c];
    }
    fog_setup(type, s, e, n, f, pt->zrow, pt->wrow, zmax, &pt->fs);
}

/* the vertex program's oFog.x (nv2a_vp.c), in float as the NV2A computes it */
static float port_vertex(const Port* pt, const double P[4]) {
    const FogSetup* fs = &pt->fs;
    float num = 0, den = 0, y;
    int c;
    for (c = 0; c < 4; c++) {
        num += fs->num[c] * (float)P[c];
        den += fs->den[c] * (float)P[c];
    }
    y = num * (1.0f / den);
    if (fs->kind == VPF_LIN) return y;
    y = y < 0.0f ? 0.0f : y > 1.0f ? 1.0f : y;
    if (fs->kind == VPF_EXP2) y = y * y;
    return fs->curve[2] * exp2f(y * fs->curve[0]) + fs->curve[1];
}

/* the fog unit: LINEAR, FOG_PARAMS (1, 1, 0): p0 + p1 * oFog - 1, then clamped per pixel */
static double port_factor(double ofog) {
    double f = 1.0 + 1.0 * ofog - 1.0;
    return f < 0.0 ? 0.0 : f > 1.0 ? 1.0 : f;
}

/* ---- projections as HSD sets them (MTXPerspective / MTXOrtho, GXSetProjection) ---- */
static void persp(float p[4][4], float fovy, float aspect, float n, float f) {
    float c = 1.0f / tanf(fovy * 0.5f * 3.14159265f / 180.0f);
    memset(p, 0, 64);
    p[0][0] = c / aspect;
    p[1][1] = c;
    p[2][2] = -n / (f - n);
    p[2][3] = -(f * n) / (f - n);
    p[3][2] = -1.0f;
}

static void ortho(float p[4][4], float t, float b, float l, float r, float n, float f) {
    memset(p, 0, 64);
    p[0][0] = 2.0f / (r - l);
    p[0][3] = -(r + l) / (r - l);
    p[1][1] = 2.0f / (t - b);
    p[1][3] = -(t + b) / (t - b);
    p[2][2] = -1.0f / (f - n);
    p[2][3] = -f / (f - n);
    p[3][3] = 1.0f;
}

static int s_fail, s_checks;
static double s_worst, s_worst_bent, *s_worst_to = &s_worst;
static char s_worst_bent_what[160];

/* ref_lo..ref_hi: what GX gives (over one depth step for a vertex) */
static double check_range(const char* what, double ref_lo, double ref_hi, double got, double tol) {
    double e = got < ref_lo ? ref_lo - got : got > ref_hi ? got - ref_hi : 0.0;
    s_checks++;
    if (e > *s_worst_to) *s_worst_to = e;
    if (e > tol) {
        if (s_fail < 20) printf("FAIL %s: GX %.5f..%.5f port %.5f (off %.2f/255)\n", what, ref_lo, ref_hi, got, e * 255.0);
        s_fail++;
    }
    return e;
}

static void check(const char* what, double ref, double got, double tol) { check_range(what, ref, ref, got, tol); }

/* GX's fog amount over the depths within one of its steps of zs (past the
 * far plane too: the formula holds there, and it is the step's size) */
static void gx_fog_range(const GxFog* r, uint32_t zs, double* lo, double* hi) {
    int32_t step = r->proj ? 1 : 1 << r->b_shift, z;
    *lo = 1.0;
    *hi = 0.0;
    for (z = (int32_t)zs - step; z <= (int32_t)zs + step; z++) {
        double v;
        if (z < 0) continue;
        v = gx_fog_amount(r, (uint32_t)z);
        if (v < *lo) *lo = v;
        if (v > *hi) *hi = v;
    }
}

static const uint32_t k_types[] = { 0, 2, 4, 5, 6, 7 };
static const char* const k_names[] = { "NONE", "?", "LIN", "?", "EXP", "EXP2", "REVEXP", "REVEXP2" };

/* one camera and one fog: every type, depths from near to far along a ray
 * off the view axis, vertex by vertex; LIN also across triangles */
static void run_case(int ortho_proj, float n, float f, float vn, float vf, float s, float e, float zmax) {
    float p[4][4];
    unsigned ti;
    int i;
    if (ortho_proj) ortho(p, 240, -240, -320, 320, n, f);
    else persp(p, 30.0f, 4.0f / 3.0f, n, f);
    for (ti = 0; ti < 2 * sizeof k_types / sizeof k_types[0]; ti++) {
        uint32_t type = k_types[ti % 6] | (ti >= 6 ? 8u : 0u);   /* then the GX_FOG_ORTHO_* types */
        int matched = vn == 0.0f && vf == 1.0f && ortho_proj == (int)(type >> 3);
        GxFog r;
        Port pt;
        double bend;
        char what[160];
        gx_set_fog(type, s, e, n, f, &r);
        port_setup(&pt, p, vn, vf, zmax, type, s, e, n, f);
        if (pt.fs.kind != fog_kind(type) || (type & 7) == 0) {
            check("fog off", 0.0, pt.fs.kind, 0.0);
            continue;
        }
        for (i = 0; i <= 400; i++) {
            /* view space looks down -z */
            double d = n + (f - n) * i / 400.0, P[4] = { 0.3 * d, -0.2 * d, -d, 1.0 }, lo, hi;
            gx_fog_range(&r, gx_depth(p, vn, vf, P), &lo, &hi);
            snprintf(what, sizeof what, "%s %s cam %g..%g vp %g..%g fog %g..%g depth %g", ortho_proj ? "ortho" : "persp",
                     k_names[type & 7], n, f, vn, vf, s, e, d);
            check_range(what, lo, hi, port_factor(port_vertex(&pt, P)), TOL);
        }
        if (pt.fs.kind != VPF_LIN || !matched) continue;
        bend = ortho_proj ? 0.0 : (8388638.0 / 8388608.0 - 1.0) * f / n;
        /* a triangle edge from near the camera to the far plane: the vertex
         * values interpolated perspective-correctly, clamped per pixel */
        for (i = 0; i < 16; i++) {
            double d0 = n + (f - n) * (i % 4) / 8.0, d1 = n + (f - n) * (4 + i / 4) / 8.0;
            double P0[4] = { -0.4 * d0, 0.1 * d0, -d0, 1 }, P1[4] = { 0.35 * d1, -0.3 * d1, -d1, 1 };
            double w0 = ortho_proj ? 1.0 : d0, w1 = ortho_proj ? 1.0 : d1;
            double f0 = port_vertex(&pt, P0), f1 = port_vertex(&pt, P1);
            int k;
            for (k = 0; k <= 64; k++) {
                double t = k / 64.0, q0 = (1 - t) / w0, q1 = t / w1, P[4], lo, hi, err, got;
                int c;
                for (c = 0; c < 4; c++) P[c] = (q0 * P0[c] + q1 * P1[c]) / (q0 + q1);
                snprintf(what, sizeof what, "%s %s cam %g..%g fog %g..%g edge %g..%g at %g", ortho_proj ? "ortho" : "persp",
                         k_names[type & 7], n, f, s, e, d0, d1, t);
                gx_fog_range(&r, gx_depth(p, vn, vf, P), &lo, &hi);
                got = port_factor((q0 * f0 + q1 * f1) / (q0 + q1));
                if (bend < EDGE_BEND_MAX) {
                    check_range(what, lo, hi, got, TOL);
                    continue;
                }
                err = got < lo ? lo - got : got > hi ? got - hi : 0.0;
                if (err > s_worst_bent) {
                    s_worst_bent = err;
                    snprintf(s_worst_bent_what, sizeof s_worst_bent_what, "%s", what);
                }
            }
        }
    }
}

/* NV2A final combiner, unsigned identity/invert inputs: rgb = A*B + (1-A)*C + D */
static double fc_input(uint32_t field, const double reg[16][4], int ch) {
    double v = reg[field & 15][(field >> 4) & 1 ? 3 : ch];
    return (field >> 5) & 7 ? 1.0 - v : v;
}

static double s_worst_fc;

static void check_combiner(void) {
    /* R0 = PREV (nv2a_rc.c's final combiner reads it through C) */
    uint32_t cw0 = fog_final_cw0(12u << 8);
    int i;
    s_worst_to = &s_worst_fc;
    srand(1);
    for (i = 0; i < 20000; i++) {
        double reg[16][4] = { { 0 } };
        uint8_t prev[3], fog[3];
        double F = (rand() % 1001) / 1000.0;
        int ch, ifog = (int)floor(F * 256.0 + 0.5);
        for (ch = 0; ch < 3; ch++) {
            prev[ch] = (uint8_t)(rand() & 255);
            fog[ch] = (uint8_t)(rand() & 255);
            reg[12][ch] = prev[ch] / 255.0;
            reg[3][ch] = fog[ch] / 255.0;
        }
        reg[12][3] = 0.5;
        reg[3][3] = F;   /* FOG.a: the fog factor */
        for (ch = 0; ch < 3; ch++) {
            double a = fc_input(cw0 >> 24, reg, ch), b = fc_input(cw0 >> 16, reg, ch), c = fc_input(cw0 >> 8, reg, ch),
                   d = fc_input(cw0, reg, ch);
            double gx = (double)((prev[ch] * (256 - ifog) + fog[ch] * ifog) >> 8);
            check("final combiner", gx / 255.0, a * b + (1.0 - a) * c + d, 1.5 / 255.0);
        }
    }
}

int main(void) {
    /* Melee's game camera is 0.1..16384 (cm/camera.c) */
    static const float cams[][2] = { { 0.1f, 16384 }, { 1, 1000 }, { 10, 30000 }, { 0.1f, 100 }, { 100, 5000 }, { 1, 16384 } };
    static const float fogs[][2] = { { 0, 1 }, { 0.3f, 0.7f }, { 0.05f, 0.5f }, { 0.6f, 0.95f }, { 0.8f, 0.2f } };
    unsigned c, k;
    for (c = 0; c < sizeof cams / sizeof cams[0]; c++)
        for (k = 0; k < sizeof fogs / sizeof fogs[0]; k++) {
            float n = cams[c][0], f = cams[c][1];
            float s = n + (f - n) * fogs[k][0], e = n + (f - n) * fogs[k][1];
            run_case(0, n, f, 0.0f, 1.0f, s, e, 16777215.0f);
            run_case(0, n, f, 0.0f, 1.0f, s, e, 65535.0f);      /* 720p: Z16 */
            run_case(0, n, f, 0.1f, 0.9f, s, e, 16777215.0f);   /* another depth range: per vertex only */
            run_case(1, n, f, 0.0f, 1.0f, s, e, 16777215.0f);
        }
    /* GXSetFog's odd cases: start == end, near == far */
    run_case(0, 1, 1000, 0.0f, 1.0f, 500, 500, 16777215.0f);
    run_case(1, 1, 1000, 0.0f, 1.0f, 500, 500, 16777215.0f);
    check_combiner();
    printf("%d checks, %d failed; fog amount: worst %.2f/255 beyond GX's own depth step; final combiner: worst "
           "%.2f/255 off GX's 8-bit blend\n",
           s_checks, s_fail, s_worst * 255.0, s_worst_fc * 255.0);
    printf("not checked: LIN edges of cameras whose b_mag bends ze by 1%% or more, worst %.1f/255 (%s)\n",
           s_worst_bent * 255.0, s_worst_bent_what);
    return s_fail != 0;
}
