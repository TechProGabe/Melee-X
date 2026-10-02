/* test_rc.c - host check of the TEV -> register combiner compiler
 * (xbox/src/hw/nv2a_rc.c) for swap tables. Built and run by
 * tools/xbox/test_rc.py, which compiles nv2a_rc.c into this file and
 * tests/xbox/rc_ref.c (the compiler before swap tables) beside it.
 *
 * 1. Random TEV configurations that read no swap table (identity, or the
 *    alpha broadcast the old compiler knew) compile to the same combiner
 *    words, constants and approximation flag as before: draws without
 *    swizzles are unchanged on the GPU.
 * 2. A configuration that reads each texture unit and colour channel
 *    through one swap table, run through a model of the combiners, gives
 *    the same pixel as the identity-table configuration run on inputs
 *    swizzled beforehand (only programs neither compile approximates).
 *    This isolates the swizzles from the compiler's other approximations.
 * 3. The 1P clear screen's sepia freeze frame (lb_800122F0: three stages
 *    reading one texture through RRRA, GGGA and BBBA) fits the combiners
 *    and gives K0 r + K1 g + K2 b.
 *
 * The combiner model follows NV_register_combiners: input mappings, AB/CD
 * products or dot products, the sum, output shift and bias, clamping to
 * [-1, 1], and blue-to-alpha; the final combiner as nv2a_rc.c uses it. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc_ref.h"

static uint32_t s_rng = 12345;
static uint32_t rnd(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
static float rnd01(void) { return (float)(rnd() % 256) / 255.0f; }

/* ---- per-draw constants, as nv2a.c's ref_val / pack_const ---- */
static uint8_t s_konst[4][4];
static int16_t s_tevreg[4][4];
static const uint8_t k_fixed[8][4] = {
    { 90, 0, 0, 44 }, { 0, 0, 113, 91 }, { 255, 0, 255, 0 }, { 0, 255, 0, 0 },
    { 255, 0, 0, 0 }, { 0, 255, 0, 0 }, { 0, 0, 255, 0 }, { 0, 0, 0, 0 },
};

static uint8_t clamp_s10(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void ref_val(uint16_t ref, uint8_t rgb[3], uint8_t* a) {
    int t = ref >> 8, p = ref & 0xFF, k;
    switch (t) {
        case RREF_FIXED:
            for (k = 0; k < 3; k++) rgb[k] = k_fixed[p & 7][k];
            *a = k_fixed[p & 7][3];
            break;
        case RREF_TEVREG_RGB:
            for (k = 0; k < 3; k++) rgb[k] = clamp_s10(s_tevreg[p & 3][k]);
            break;
        case RREF_TEVREG_A: *a = clamp_s10(s_tevreg[p & 3][3]); break;
        case RREF_KONST_C:
            if (p <= 7) rgb[0] = rgb[1] = rgb[2] = (uint8_t)(255 * (8 - p) / 8);
            else if (p >= 0x0C && p <= 0x0F) for (k = 0; k < 3; k++) rgb[k] = s_konst[p - 0x0C][k];
            else if (p >= 0x10) rgb[0] = rgb[1] = rgb[2] = s_konst[(p - 0x10) & 3][((p - 0x10) >> 2) & 3];
            else rgb[0] = rgb[1] = rgb[2] = 0;
            break;
        case RREF_KONST_A:
            if (p <= 7) *a = (uint8_t)(255 * (8 - p) / 8);
            else if (p >= 0x10) *a = s_konst[(p - 0x10) & 3][((p - 0x10) >> 2) & 3];
            else *a = 0;
            break;
    }
}

static void const_val(uint16_t rgb_ref, uint16_t a_ref, float out[4]) {
    uint8_t rgb[3] = { 0, 0, 0 }, a = 0;
    int k;
    if (rgb_ref) ref_val(rgb_ref, rgb, &a);
    if (a_ref) ref_val(a_ref, rgb, &a);
    if (a_ref && !rgb_ref) rgb[0] = rgb[1] = rgb[2] = 0;
    if (rgb_ref && !a_ref) a = 0;
    for (k = 0; k < 3; k++) out[k] = rgb[k] / 255.0f;
    out[3] = a / 255.0f;
}

/* ---- the combiners ---- */
typedef struct { float r[16][4]; } Regs;   /* 0 zero, 1/2 C0/C1, 3 fog, 4/5 V0/V1, 8-11 T0-T3, 12/13 R0/R1 */

static float in_map(float x, int map) {
    float p = x > 0 ? x : 0;
    switch (map) {
        case 0: return p;
        case 1: return 1 - (p > 1 ? 1 : p);
        case 2: return 2 * p - 1;
        case 3: return -2 * p + 1;
        case 4: return p - 0.5f;
        case 5: return -p + 0.5f;
        case 6: return x;
        default: return -x;
    }
}

/* one input of a portion: rgb (alpha bit: alpha broadcast) or alpha (alpha bit clear: blue) */
static void in_val(const Regs* g, uint32_t enc, int alpha_portion, float out[3]) {
    int src = enc & 0xF, al = enc >> 4 & 1, map = enc >> 5 & 7, k;
    for (k = 0; k < 3; k++) {
        float v = alpha_portion ? (al ? g->r[src][3] : g->r[src][2]) : (al ? g->r[src][3] : g->r[src][k]);
        out[k] = in_map(v, map);
    }
}

static float out_op(float v, int op) {
    switch (op) {
        case 1: v = v - 0.5f; break;
        case 2: v = 2 * v; break;
        case 3: v = 2 * (v - 0.5f); break;
        case 4: v = 4 * v; break;
        case 6: v = v / 2; break;
        default: break;
    }
    return v < -1 ? -1 : v > 1 ? 1 : v;
}

static void stage(Regs* g, uint32_t icw, uint32_t ocw, int alpha_portion, const Regs* rd) {
    float a[3], b[3], c[3], d[3], ab[3], cd[3], sum[3];
    int k, n = alpha_portion ? 1 : 3, op = ocw >> 15 & 7;
    int cd_dst = ocw & 0xF, ab_dst = ocw >> 4 & 0xF, sum_dst = ocw >> 8 & 0xF;
    in_val(rd, icw >> 24, alpha_portion, a);
    in_val(rd, icw >> 16 & 0xFF, alpha_portion, b);
    in_val(rd, icw >> 8 & 0xFF, alpha_portion, c);
    in_val(rd, icw & 0xFF, alpha_portion, d);
    for (k = 0; k < 3; k++) {
        ab[k] = a[k] * b[k];
        cd[k] = c[k] * d[k];
    }
    if (!alpha_portion && (ocw & (1u << 13))) ab[0] = ab[1] = ab[2] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (!alpha_portion && (ocw & (1u << 12))) cd[0] = cd[1] = cd[2] = c[0] * d[0] + c[1] * d[1] + c[2] * d[2];
    for (k = 0; k < 3; k++) {
        ab[k] = out_op(ab[k], op);
        cd[k] = out_op(cd[k], op);
        sum[k] = out_op(a[k] * b[k] + c[k] * d[k], op);
    }
    if (alpha_portion) {
        if (ab_dst) g->r[ab_dst][3] = ab[0];
        if (cd_dst) g->r[cd_dst][3] = cd[0];
        if (sum_dst) g->r[sum_dst][3] = sum[0];
        return;
    }
    for (k = 0; k < n; k++) {
        if (ab_dst) g->r[ab_dst][k] = ab[k];
        if (cd_dst) g->r[cd_dst][k] = cd[k];
        if (sum_dst) g->r[sum_dst][k] = sum[k];
    }
    if (ab_dst && (ocw & (1u << 19))) g->r[ab_dst][3] = ab[2];
    if (cd_dst && (ocw & (1u << 18))) g->r[cd_dst][3] = cd[2];
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void run(const RcProg* p, const Regs* in, float out[4]) {
    Regs g = *in, rd;
    float x[3], fa[3], fb[3], fc[3], fd[3];
    int i, k;
    for (i = 0; i < p->nstages; i++) {
        const_val(p->cref[i][0], p->cref[i][1], g.r[1]);
        const_val(p->cref[i][2], p->cref[i][3], g.r[2]);
        rd = g;
        stage(&g, p->cicw[i], p->cocw[i], 0, &rd);
        stage(&g, p->aicw[i], p->aocw[i], 1, &rd);
    }
    /* final: A*B + (1-A)*C + D, inputs clamped to [0, 1] (bit 5: invert); alpha = G */
    const_val(p->fref[0], p->fref[1], g.r[1]);
    const_val(p->fref[2], p->fref[3], g.r[2]);
    {
        uint32_t w[4] = { p->cw0 >> 24, p->cw0 >> 16 & 0xFF, p->cw0 >> 8 & 0xFF, p->cw0 & 0xFF };
        float* f[4] = { fa, fb, fc, fd };
        for (i = 0; i < 4; i++)
            for (k = 0; k < 3; k++) {
                int src = w[i] & 0xF, al = w[i] >> 4 & 1, inv = w[i] >> 5 & 1;
                float v = clamp01(al ? g.r[src][3] : g.r[src][k]);
                f[i][k] = inv ? 1 - v : v;
            }
        for (k = 0; k < 3; k++) x[k] = fa[k] * fb[k] + (1 - fa[k]) * fc[k] + fd[k];
    }
    for (k = 0; k < 3; k++) out[k] = clamp01(x[k]);
    {
        uint32_t gw = p->cw1 >> 8 & 0xFF;
        int src = gw & 0xF, al = gw >> 4 & 1;
        out[3] = clamp01(al ? g.r[src][3] : g.r[src][2]);
    }
}

/* ---- random configurations ---- */
static const uint8_t k_kcsel[] = { 0, 3, 7, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x15, 0x1A, 0x1F };
static const uint8_t k_kasel[] = { 0, 4, 7, 0x10, 0x15, 0x1A, 0x1C, 0x1F };

static void rand_stage(RcStage* r) {
    int k;
    memset(r, 0, sizeof *r);
    for (k = 0; k < 4; k++) {
        r->cin[k] = (uint8_t)(rnd() % 16);
        r->ain[k] = (uint8_t)(rnd() % 8);
    }
    r->cop = (uint8_t)(rnd() % 8 == 0 ? 2 : rnd() % 2);
    r->aop = (uint8_t)(rnd() % 2);
    r->cbias = (uint8_t)(rnd() % 3);
    r->abias = (uint8_t)(rnd() % 3);
    r->cscale = (uint8_t)(rnd() % 4);
    r->ascale = (uint8_t)(rnd() % 4);
    r->cclamp = r->aclamp = 1;
    r->cout = (uint8_t)(rnd() % 4 ? 0 : rnd() % 4);
    r->aout = (uint8_t)(rnd() % 4 ? 0 : rnd() % 4);
    r->kcsel = k_kcsel[rnd() % sizeof k_kcsel];
    r->kasel = k_kasel[rnd() % sizeof k_kasel];
    r->unit = (int8_t)((int)(rnd() % 5) - 1);
    r->ras = (uint8_t)(rnd() % 3);
    r->tex_swz = r->ras_swz = RC_SWZ_ID;
}

static void finish_cfg(RcCfg* c) {
    int s;
    c->units_used = 0;
    c->v1_used = 0;
    for (s = 0; s < c->nstages; s++) {
        if (c->st[s].unit >= 0) c->units_used |= (uint8_t)(1u << c->st[s].unit);
        if (c->st[s].ras == 1) c->v1_used = 1;
        if (c->st[s].unit < 0) c->st[s].tex_swz = RC_SWZ_ID;   /* as derive_units leaves them */
        if (c->st[s].ras == 2) c->st[s].ras_swz = RC_SWZ_ID;
    }
}

static int same_prog(const RcProg* a, const RefProg* b) {
    int i;
    if (a->nstages != b->nstages || a->approximated != b->approximated || a->cw0 != b->cw0 || a->cw1 != b->cw1 ||
        memcmp(a->fref, b->fref, sizeof a->fref))
        return 0;
    for (i = 0; i < a->nstages; i++)
        if (a->cicw[i] != b->cicw[i] || a->cocw[i] != b->cocw[i] || a->aicw[i] != b->aicw[i] ||
            a->aocw[i] != b->aocw[i] || memcmp(a->cref[i], b->cref[i], sizeof a->cref[i]))
            return 0;
    return 1;
}

static int test_unchanged(int n) {
    int t, s, bad = 0, approx = 0;
    for (t = 0; t < n; t++) {
        RcCfg c;
        RefCfg r;
        RcProg p;
        RefProg q;
        memset(&c, 0, sizeof c);
        memset(&r, 0, sizeof r);
        c.nstages = (uint8_t)(1 + rnd() % 8);
        for (s = 0; s < c.nstages; s++) {
            int tb = rnd() % 4 == 0, rb = rnd() % 4 == 0;   /* AAAA tables */
            rand_stage(&c.st[s]);
            if (tb) c.st[s].tex_swz = 0xFF;
            if (rb) c.st[s].ras_swz = 0xFF;
        }
        finish_cfg(&c);
        r.nstages = c.nstages;
        r.units_used = c.units_used;
        r.v1_used = c.v1_used;
        for (s = 0; s < c.nstages; s++) {
            memcpy(&r.st[s], &c.st[s], sizeof r.st[s]);   /* same layout: the swizzle bytes are the flags */
            r.st[s].tex_alpha_bcast = c.st[s].tex_swz == 0xFF;
            r.st[s].ras_alpha_bcast = c.st[s].ras_swz == 0xFF;
        }
        rc_compile(&c, &p);
        rc_ref_compile(&r, &q);
        approx += p.approximated;
        if (!same_prog(&p, &q)) {
            if (bad < 5) printf("FAIL unchanged: config %d (%d stages) compiles differently\n", t, c.nstages);
            bad++;
        }
    }
    printf("unchanged: %d configurations without swizzles (%d approximated by both), %d differ\n", n, approx, bad);
    return bad;
}

static uint8_t rand_table(void) {
    static const uint8_t tables[] = {
        RC_SWZ_ID, 0xC0, 0xD5, 0xEA, 0xFF,   /* RGBA RRRA GGGA BBBA AAAA */
        0x24, 0x64, 0xA4,                    /* RGB with alpha from r, g, b */
        0x00, 0x55, 0xAA,                    /* RRRR GGGG BBBB */
        0xC6,                                /* GBRA: a permutation (approximated) */
    };
    return tables[rnd() % sizeof tables];
}

static void swizzle(const float in[4], uint8_t swz, float out[4]) {
    int k;
    for (k = 0; k < 4; k++) out[k] = in[swz >> (2 * k) & 3];
}

static void rand_regs(Regs* g) {
    int i, k;
    for (i = 0; i < 16; i++)
        for (k = 0; k < 4; k++) g->r[i][k] = rnd01();
    for (k = 0; k < 4; k++) g->r[0][k] = g->r[3][k] = 0;
}

static int test_swizzles(int n) {
    int t, s, k, bad = 0, ran = 0, preps = 0;
    for (t = 0; t < n; t++) {
        RcCfg c, id;
        RcProg p, q;
        uint8_t ttab[4], rtab[2];
        Regs g, gi;
        float o1[4], o2[4];
        int i;
        memset(&c, 0, sizeof c);
        c.nstages = (uint8_t)(1 + rnd() % 4);
        for (i = 0; i < 4; i++) ttab[i] = rand_table();
        for (i = 0; i < 2; i++) rtab[i] = rand_table();
        for (s = 0; s < c.nstages; s++) {
            rand_stage(&c.st[s]);
            if (c.st[s].unit >= 0) c.st[s].tex_swz = ttab[(int)c.st[s].unit];
            if (c.st[s].ras < 2) c.st[s].ras_swz = rtab[c.st[s].ras];
        }
        finish_cfg(&c);
        id = c;
        for (s = 0; s < id.nstages; s++) id.st[s].tex_swz = id.st[s].ras_swz = RC_SWZ_ID;
        rc_compile(&c, &p);
        rc_compile(&id, &q);
        if (p.approximated || q.approximated) continue;
        for (i = 0; i < 4; i++)
            for (k = 0; k < 4; k++) s_konst[i][k] = (uint8_t)(rnd() % 256);
        for (i = 0; i < 4; i++)
            for (k = 0; k < 4; k++) s_tevreg[i][k] = (int16_t)(rnd() % 256);
        rand_regs(&g);
        gi = g;
        for (i = 0; i < 4; i++)
            if (c.units_used >> i & 1) swizzle(g.r[8 + i], ttab[i], gi.r[8 + i]);
        swizzle(g.r[4], rtab[0], gi.r[4]);
        if (c.v1_used) swizzle(g.r[5], rtab[1], gi.r[5]);
        run(&p, &g, o1);
        run(&q, &gi, o2);
        ran++;
        if (p.nstages > q.nstages) preps++;
        for (k = 0; k < 4; k++)
            if (fabsf(o1[k] - o2[k]) > 1e-5f) {
                if (bad < 5) {
                    printf("FAIL swizzle: config %d channel %d: %f vs %f (%d / %d combiner stages)\n", t, k, o1[k],
                           o2[k], p.nstages, q.nstages);
                    for (s = 0; s < c.nstages; s++) {
                        const RcStage* r = &c.st[s];
                        printf("  stage %d: cin %d %d %d %d ain %d %d %d %d op %d/%d bias %d/%d scale %d/%d out %d/%d "
                               "unit %d ras %d swz %02x/%02x\n", s, r->cin[0], r->cin[1], r->cin[2], r->cin[3],
                               r->ain[0], r->ain[1], r->ain[2], r->ain[3], r->cop, r->aop, r->cbias, r->abias,
                               r->cscale, r->ascale, r->cout, r->aout, r->unit, r->ras, r->tex_swz, r->ras_swz);
                    }
                    for (s = 0; s < p.nstages; s++)
                        printf("  combiner %d: %08x %08x | %08x %08x\n", s, p.cicw[s], p.cocw[s], p.aicw[s],
                               p.aocw[s]);
                }
                bad++;
                break;
            }
    }
    printf("swizzles: %d of %d configurations compiled exactly (%d with prep stages), %d differ\n", ran, n, preps,
           bad);
    return bad || preps == 0;
}

/* lb_800122F0: rgb = K0 * tex.rrr + K1 * tex.ggg + K2 * tex.bbb, alpha = TEXA (1 - A0) */
static int test_sepia(void) {
    RcCfg c;
    RcProg p;
    Regs g;
    float o[4], want[3];
    int s, k, bad = 0;
    static const uint8_t cin[3][4] = { { 15, 8, 14, 15 }, { 15, 8, 14, 0 }, { 15, 8, 14, 0 } };
    static const uint8_t ain[3][4] = { { 4, 7, 1, 7 }, { 7, 7, 7, 0 }, { 7, 7, 7, 0 } };
    static const uint8_t swz[3] = { 0xC0, 0xD5, 0xEA };
    memset(&c, 0, sizeof c);
    c.nstages = 3;
    for (s = 0; s < 3; s++) {
        RcStage* r = &c.st[s];
        memcpy(r->cin, cin[s], 4);
        memcpy(r->ain, ain[s], 4);
        r->cclamp = r->aclamp = 1;
        r->kcsel = (uint8_t)(0x0C + s);
        r->kasel = 0;
        r->unit = 0;
        r->ras = 0;
        r->tex_swz = swz[s];
        r->ras_swz = RC_SWZ_ID;
    }
    finish_cfg(&c);
    rc_compile(&c, &p);
    s_konst[0][0] = 76; s_konst[0][1] = 68; s_konst[0][2] = 61;
    s_konst[1][0] = 150; s_konst[1][1] = 135; s_konst[1][2] = 120;
    s_konst[2][0] = 29; s_konst[2][1] = 26; s_konst[2][2] = 23;
    s_tevreg[1][3] = 0x40;
    rand_regs(&g);
    run(&p, &g, o);
    for (k = 0; k < 3; k++)
        want[k] = clamp01((s_konst[0][k] * g.r[8][0] + s_konst[1][k] * g.r[8][1] + s_konst[2][k] * g.r[8][2]) / 255.0f);
    for (k = 0; k < 3; k++)
        if (fabsf(o[k] - want[k]) > 1e-5f) bad = 1;
    if (fabsf(o[3] - g.r[8][3] * (1 - 0x40 / 255.0f)) > 1e-5f) bad = 1;
    printf("sepia (lb_800122F0): %d combiner stages%s, rgb %.4f %.4f %.4f (want %.4f %.4f %.4f) %s\n", p.nstages,
           p.approximated ? ", approximated" : "", o[0], o[1], o[2], want[0], want[1], want[2],
           bad || p.approximated ? "FAIL" : "ok");
    return bad || p.approximated;
}

int main(int argc, char** argv) {
    int n = argc > 1 ? atoi(argv[1]) : 200000, fail = 0;
    fail += test_unchanged(n);
    fail += test_swizzles(n);
    fail += test_sepia();
    printf("%s\n", fail ? "FAILED" : "all passed");
    return fail ? 1 : 0;
}
