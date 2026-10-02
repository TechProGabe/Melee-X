/* test_vp_opt.c - host check that the vertex-program optimizer changes no
 * result: for random VpKeys, the program xbox/src/hw/nv2a_vp.c makes (after
 * vp_canon, as the back end calls it) and the program the generator made
 * before the optimizer (tests/xbox/vp_ref.c) run through an interpreter of
 * the NV2A instruction words on random vertices and constants, and every
 * output lane either writes must match bit for bit, with the same lanes
 * written. The interpreter decodes the encoded words, so pairing, dual
 * writes and the r1 rule are checked as the hardware would see them.
 *
 * Operations are compared, not maths: each op is evaluated the same way for
 * both programs, so equal bits mean the same ops ran on the same operands.
 * The facts the generator relies on beyond that: attributes with fewer than
 * 4 components read as (x, y, z, 1) / (u, v, 0, 1), as the back end declares
 * position and normal with 3 and texcoords with 2 (nv2a.c
 * emit_vertex_arrays); and a light the key flags (VpKey.ang_one, dist_one)
 * has the identity attenuation (1, 0, 0) in its rows, which the inputs
 * here give it, so the terms left out are 1 exactly. Temporaries start as random garbage, different on a
 * second run of the new program, so a read before a write shows too. The
 * new program also runs as xemu executes a pair (the MAC op's writes before
 * the ILU op reads), which must not change anything either.
 *
 * Built and run by tools/xbox/test_vp_opt.py. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_vp.h"

void vp_ref_generate(const VpKey* k, VpProgram* out);
int vp_test_unoptimized(const VpKey* key, uint32_t* words, int max);
int vp_test_legacy_len(const VpKey* k);

/* ---- interpreter ---- */
typedef struct {
    float v[16][4];
    float c[192][4];
} VpIn;

typedef struct {
    float o[16][4];
    uint8_t wr[16];    /* lanes written */
    int bad;           /* invalid instruction or index */
} VpOut;

static int hw_to_xyzw(uint32_t m) { return (int)((m >> 3 & 1) | (m >> 1 & 2) | (m << 1 & 4) | (m << 3 & 8)); }

static void fetch(const VpIn* in, float r[16][4], int a0, int mux, int tmp, int vidx, int cidx, int rel, int swz,
                  int negf, float out[4], VpOut* o) {
    const float* src;
    int i;
    static const float zero[4];
    switch (mux) {
        case 1: src = r[tmp]; break;
        case 2: src = in->v[vidx]; break;
        case 3: {
            int ci = rel ? a0 + cidx : cidx;
            if (ci < 0 || ci >= 192) {
                o->bad = 1;
                ci = 0;
            }
            src = in->c[ci];
            break;
        }
        default: src = zero; break;
    }
    for (i = 0; i < 4; i++) {
        out[i] = src[swz >> (6 - 2 * i) & 3];
        if (negf) out[i] = -out[i];
    }
}

/* seq: xemu's order within a pair, MAC op then ILU op; else every read
 * before any write */
static void run(const uint32_t* words, int n, const VpIn* in, unsigned seed, int seq, VpOut* o) {
    float r[16][4];
    int a0 = 0, p, i;
    memset(o, 0, sizeof *o);
    srand(seed);
    for (i = 0; i < 64; i++) r[i / 4][i % 4] = (float)(rand() % 20000 - 10000) / 7.0f;
    for (p = 0; p < n; p++) {
        uint32_t w1 = words[p * 4 + 1], w2 = words[p * 4 + 2], w3 = words[p * 4 + 3];
        int mac = w1 >> 21 & 15, ilu = w1 >> 25 & 7;
        int cidx = w1 >> 13 & 0xFF, vidx = w1 >> 9 & 15, rel = w3 >> 1 & 1;
        float a[4], b[4], c[4], m[4] = { 0 }, l[4] = { 0 };
        int tidx = w3 >> 20 & 15, mmask = hw_to_xyzw(w3 >> 24 & 15), imask = hw_to_xyzw(w3 >> 16 & 15);
        int omask = hw_to_xyzw(w3 >> 12 & 15), oaddr = w3 >> 3 & 0xFF, omux = w3 >> 2 & 1;
        int ctemp = (int)((w2 & 3) << 2 | (w3 >> 30 & 3));
        fetch(in, r, a0, w2 >> 26 & 3, w2 >> 28 & 15, vidx, cidx, rel, (int)(w1 & 0xFF), w1 >> 8 & 1, a, o);
        fetch(in, r, a0, w2 >> 11 & 3, w2 >> 13 & 15, vidx, cidx, rel, (int)(w2 >> 17 & 0xFF), w2 >> 25 & 1, b, o);
        fetch(in, r, a0, w3 >> 28 & 3, ctemp, vidx, cidx, rel, (int)(w2 >> 2 & 0xFF), w2 >> 10 & 1, c, o);
        switch (mac) {
            case 0: break;
            case 1: memcpy(m, a, sizeof m); break;
            case 2: for (i = 0; i < 4; i++) m[i] = a[i] * b[i]; break;
            case 3: for (i = 0; i < 4; i++) m[i] = a[i] + c[i]; break;
            case 4: for (i = 0; i < 4; i++) m[i] = a[i] * b[i] + c[i]; break;
            case 5: m[0] = m[1] = m[2] = m[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; break;
            case 7: m[0] = m[1] = m[2] = m[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; break;
            case 9: for (i = 0; i < 4; i++) m[i] = a[i] < b[i] ? a[i] : b[i]; break;
            case 10: for (i = 0; i < 4; i++) m[i] = a[i] >= b[i] ? a[i] : b[i]; break;
            case 11: for (i = 0; i < 4; i++) m[i] = a[i] < b[i] ? 1.0f : 0.0f; break;
            case 12: for (i = 0; i < 4; i++) m[i] = a[i] >= b[i] ? 1.0f : 0.0f; break;
            case 13: a0 = (int)floorf(a[0]); break;
            default: o->bad = 1; break;
        }
        /* writes, after every read (seq: the MAC op's first) */
        if (seq && mac && mac != 13 && mmask) {
            if (tidx >= 12) o->bad = 1;
            for (i = 0; i < 4; i++)
                if (mmask >> i & 1) r[tidx & 15][i] = m[i];
            fetch(in, r, a0, w3 >> 28 & 3, ctemp, vidx, cidx, rel, (int)(w2 >> 2 & 0xFF), w2 >> 10 & 1, c, o);
        }
        switch (ilu) {
            case 0: break;
            case 1: memcpy(l, c, sizeof l); break;
            case 2: l[0] = l[1] = l[2] = l[3] = 1.0f / c[0]; break;
            case 4: l[0] = l[1] = l[2] = l[3] = 1.0f / sqrtf(fabsf(c[0])); break;
            case 5:
                l[0] = exp2f(floorf(c[0]));
                l[1] = c[0] - floorf(c[0]);
                l[2] = exp2f(c[0]);
                l[3] = 1.0f;
                break;
            default: o->bad = 1; break;
        }
        if (!seq && mac && mac != 13 && mmask) {
            if (tidx >= 12) o->bad = 1;
            for (i = 0; i < 4; i++)
                if (mmask >> i & 1) r[tidx][i] = m[i];
        }
        if (ilu && imask) {
            int t = mac ? 1 : tidx;   /* paired: r1 */
            if (t >= 12) o->bad = 1;
            for (i = 0; i < 4; i++)
                if (imask >> i & 1) r[t][i] = l[i];
        }
        if (omask) {
            if (!(w3 >> 11 & 1) || oaddr >= 16) o->bad = 1;
            else
                for (i = 0; i < 4; i++)
                    if (omask >> i & 1) {
                        o->o[oaddr][i] = omux ? l[i] : m[i];
                        o->wr[oaddr] |= (uint8_t)(1u << i);
                    }
        }
        if (w3 & 1) {
            if (p != n - 1) o->bad = 1;   /* FINAL only on the last */
            return;
        }
    }
    o->bad = 1;   /* no FINAL */
}

static int out_eq(const VpOut* x, const VpOut* y) {
    int i, j;
    for (i = 0; i < 16; i++) {
        if (x->wr[i] != y->wr[i]) return 0;
        for (j = 0; j < 4; j++)
            if (x->wr[i] >> j & 1 && memcmp(&x->o[i][j], &y->o[i][j], 4) != 0) return 0;
    }
    return 1;
}

/* ---- random inputs and keys ---- */
static uint32_t s_rng = 12345;
static uint32_t rnd(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
static float rf(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }

static void rand_input(VpIn* in, const VpKey* k) {
    int i, j;
    for (i = 0; i < 192; i++)
        for (j = 0; j < 4; j++) in->c[i][j] = rf(-2.0f, 2.0f);
    /* the attenuation the key says is the identity (as HSD sets it for
     * infinite and point lights) */
    for (i = 0; i < 8; i++) {
        float* a = in->c[VPC_LIGHT + i * 5 + 3];
        float* kk = in->c[VPC_LIGHT + i * 5 + 4];
        if (k->ang_one >> i & 1) a[0] = 1, a[1] = a[2] = 0;
        if (k->dist_one >> i & 1) kk[0] = 1, kk[1] = kk[2] = 0;
    }
    in->c[VPC_K][0] = 0, in->c[VPC_K][1] = 1, in->c[VPC_K][2] = 0.5f, in->c[VPC_K][3] = 2;
    for (i = 0; i < 16; i++) {
        for (j = 0; j < 4; j++) in->v[i][j] = rf(-3.0f, 3.0f);
    }
    in->v[VPI_POS][3] = 1;                                          /* 3 components */
    in->v[VPI_MTX][0] = (float)(rnd() % 10 * 3);                    /* PNMTXIDX */
    in->v[VPI_MTX][1] = in->v[VPI_MTX][2] = 0, in->v[VPI_MTX][3] = 1;
    in->v[VPI_NRM][3] = 1;
    for (j = 0; j < 4; j++) in->v[VPI_COL0][j] = rf(0, 1), in->v[VPI_COL1][j] = rf(0, 1);
    for (i = 0; i < 8; i++) in->v[vpi_tex(i)][2] = 0, in->v[vpi_tex(i)][3] = 1;
}

static void rand_chan(VpChan* c) {
    static const uint8_t masks[] = { 0x01, 0x03, 0x07, 0x0F, 0x02, 0x05, 0x11, 0xFF, 0x30, 0x81, 0x00 };
    c->enable = (uint8_t)(rnd() % 4 != 0);
    c->amb_vtx = (uint8_t)(rnd() & 1);
    c->mat_vtx = (uint8_t)(rnd() & 1);
    c->diff_fn = (uint8_t)(rnd() % 3);
    c->attn = (uint8_t)(1 + rnd() % 3);
    c->light_mask = rnd() % 3 ? masks[rnd() % (sizeof masks)] : (uint8_t)rnd();
    if (!c->enable && rnd() % 2) c->light_mask = 0;
}

static void rand_key(VpKey* k) {
    static const uint8_t srcs[] = { 0, 1, 4, 5, 6, 11, 2, 12 };
    int i;
    memset(k, 0, sizeof *k);
    if (rnd() % 200 == 0) {
        k->copy = 1;
        return;
    }
    k->has_nrm = (uint8_t)(rnd() % 4 != 0);
    k->nchans = (uint8_t)(rnd() % 3);
    for (i = 0; i < 4; i++) rand_chan(&k->chan[i]);
    /* GXSetChanCtrl(GX_COLOR0A0, ...) sets both halves alike */
    if (rnd() % 2) k->chan[1] = k->chan[0];
    if (rnd() % 2) k->chan[3] = k->chan[2];
    /* or the same lights with other colour sources */
    if (rnd() % 4 == 0) {
        k->chan[1].enable = k->chan[0].enable, k->chan[1].light_mask = k->chan[0].light_mask;
        k->chan[1].attn = k->chan[0].attn, k->chan[1].diff_fn = k->chan[0].diff_fn;
    }
    k->ntex = (uint8_t)(rnd() % 5);
    for (i = 0; i < 4; i++) {
        k->tex[i].src = srcs[rnd() % (sizeof srcs)];
        k->tex[i].proj = (uint8_t)(rnd() & 1);
        k->tex[i].normalize = (uint8_t)(rnd() % 4 == 0);
    }
    k->fog = (uint8_t)(rnd() % 4);
    k->ang_one = rnd() % 3 ? (uint8_t)rnd() : 0;
    k->dist_one = rnd() % 3 ? (uint8_t)(rnd() & k->ang_one) : 0;
}

static void simplify(VpKey* s) {
    int i;
    for (i = 0; i < 4; i++) s->chan[i].light_mask &= 0x3;
    for (i = 0; i < 4; i++) s->chan[i].attn = s->chan[i].attn == VPL_SPOT ? VPL_DIFFUSE : s->chan[i].attn;
}

int main(int argc, char** argv) {
    int nkeys = argc > 1 ? atoi(argv[1]) : 200000, t, i, fails = 0, degenerate = 0, overflowed = 0, shown = 0;
    long sum_ref = 0, sum_new = 0, sum_unopt = 0, nfit = 0, npaired = 0;
    int hist_ref[3] = { 0 }, hist_new[3] = { 0 };
    static VpProgram ref, cur;
    static uint32_t unopt[512 * 4];
    for (t = 0; t < nkeys; t++) {
        VpKey k, kc, s;
        VpIn in;
        int deg, nun, len;
        rand_key(&k);
        s = k;
        simplify(&s);
        len = vp_test_legacy_len(&k);
        /* the old generator cut a program it couldn't simplify at 136:
         * nothing to compare against there */
        deg = len > VP_MAX_INSNS && (!memcmp(&s, &k, sizeof s) || vp_test_legacy_len(&s) > VP_MAX_INSNS);
        vp_ref_generate(&k, &ref);
        kc = k;
        vp_canon(&kc);
        vp_generate(&kc, &cur);
        if (len > VP_MAX_INSNS) overflowed++;
        if (deg) {
            degenerate++;
            continue;
        }
        if (len <= VP_MAX_INSNS && (int)ref.n != len) {
            printf("legacy_len %d, reference program %u instructions\n", len, ref.n);
            fails++;
        }
        if (ref.approximated != cur.approximated) {
            printf("key %d: approximated %d, reference %d\n", t, cur.approximated, ref.approximated);
            fails++;
        }
        nun = vp_test_unoptimized(&kc, unopt, 512);
        sum_ref += ref.n;
        sum_new += cur.n;
        if (!ref.approximated) {
            sum_unopt += nun;
            nfit++;
        }
        hist_ref[ref.n <= 40 ? 0 : ref.n <= 80 ? 1 : 2]++;
        hist_new[cur.n <= 40 ? 0 : cur.n <= 80 ? 1 : 2]++;
        for (i = 0; i < (int)cur.n; i++) npaired += (cur.words[i * 4 + 1] >> 21 & 15) && (cur.words[i * 4 + 1] >> 25 & 7);
        for (i = 0; i < 4; i++) {
            VpOut ro, co, co2;
            rand_input(&in, &k);
            run(ref.words, (int)ref.n, &in, 1u + (unsigned)i, 0, &ro);
            run(cur.words, (int)cur.n, &in, 1000u + (unsigned)i, 0, &co);
            run(cur.words, (int)cur.n, &in, 2000u + (unsigned)i, 1, &co2);
            if (ro.bad || co.bad || co2.bad || !out_eq(&ro, &co) || !out_eq(&co, &co2)) {
                fails++;
                if (shown++ < 10) {
                    int j;
                    printf("key %d differs (ref bad %d, new bad %d, garbage or xemu order %d): ", t, ro.bad, co.bad,
                           !out_eq(&co, &co2));
                    for (j = 0; j < (int)sizeof k; j++) printf("%02x", ((const uint8_t*)&k)[j]);
                    printf("\n");
                }
                break;
            }
        }
    }
    /* VP_PROJ_DIVIDE (a BUMPENVMAP unit, indirect texturing) has no
     * reference program: the optimized program must match the generator's
     * own, and its s, t the projective program's s / q, t / q, with q 1 */
    {
        int dfails = 0, dn = 0;
        for (t = 0; t < nkeys / 10; t++) {
            VpKey k, kp;
            VpIn in;
            VpOut co, uo, po;
            int u, nun, j;
            rand_key(&k);
            if (!k.ntex) k.ntex = 1;
            u = (int)(rnd() % k.ntex);
            k.tex[u].proj = 1;
            vp_canon(&k);
            kp = k;
            k.tex[u].proj = VP_PROJ_DIVIDE;
            vp_generate(&k, &cur);
            vp_generate(&kp, &ref);
            if (cur.approximated || ref.approximated) continue;
            nun = vp_test_unoptimized(&k, unopt, 512);
            rand_input(&in, &k);
            run(cur.words, (int)cur.n, &in, 3000u + (unsigned)t, 0, &co);
            run(unopt, nun, &in, 4000u + (unsigned)t, 0, &uo);
            run(ref.words, (int)ref.n, &in, 5000u + (unsigned)t, 0, &po);
            dn++;
            j = 9 + u;   /* oT0 + u */
            if (co.bad || uo.bad || !out_eq(&co, &uo) || co.o[j][3] != 1.0f ||
                fabsf(co.o[j][0] - po.o[j][0] / po.o[j][3]) > 1e-4f * (1.0f + fabsf(co.o[j][0])) ||
                fabsf(co.o[j][1] - po.o[j][1] / po.o[j][3]) > 1e-4f * (1.0f + fabsf(co.o[j][1]))) {
                if (dfails++ < 5) printf("divide key %d unit %d differs\n", t, u);
            }
        }
        printf("%d keys with a unit divided by q (VP_PROJ_DIVIDE): %d failed\n", dn, dfails);
        fails += dfails;
    }
    printf("%d keys (%d longer than 136 before, %d of them cut by the old generator and skipped): %d failed\n", nkeys,
           overflowed, degenerate, fails);
    printf("instructions per program: %.1f before, %.1f now (%.1f as generated), %.1f%% of instructions paired\n",
           (double)sum_ref / (nkeys - degenerate), (double)sum_new / (nkeys - degenerate),
           nfit ? (double)sum_unopt / nfit : 0.0, 100.0 * (double)npaired / (double)(sum_new ? sum_new : 1));
    printf("programs of <=40 / 41-80 / >80 instructions: before %d/%d/%d, now %d/%d/%d\n", hist_ref[0], hist_ref[1],
           hist_ref[2], hist_new[0], hist_new[1], hist_new[2]);
    return fails ? 1 : 0;
}
