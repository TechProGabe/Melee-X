/* nv2a_vp.c - NV2A vertex programs for GX transform and lighting.
 *
 * The GameCube lights per vertex with up to 8 lights on two colour channels
 * (each with a colour and an alpha half), skins with a per-vertex matrix
 * index, and generates up to 8 texture coordinates. One general program
 * would not fit the NV2A's 136 instructions, so a program is generated per
 * configuration (VpKey) and cached by the back end. Constants are laid out
 * per nv2a_vp.h and uploaded separately.
 *
 * The instruction encoding follows nv2a-vsh (pip nv2a-vsh), the assembler
 * OpenCrossing-Xbox uses; tools/xbox/test_vp_encoder.py checks this encoder
 * against it bit for bit. */
#include <string.h>

#include "nv2a_vp.h"

/* ======================================================================
 * Encoder
 * ====================================================================== */
enum { MUX_R = 1, MUX_V = 2, MUX_C = 3 };
enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4, MAC_DST, MAC_MIN, MAC_MAX,
       MAC_SLT, MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };
/* output registers */
enum { O_POS = 0, O_D0 = 3, O_D1 = 4, O_FOG = 5, O_T0 = 9 };

typedef struct {
    uint8_t mux, idx, neg, rel;
    uint8_t swz;   /* x | y << 2 | z << 4 | w << 6 */
} Src;

typedef struct {
    uint8_t out;   /* 0: temp, 1: output register */
    uint8_t idx;
    uint8_t mask;  /* x 1, y 2, z 4, w 8 */
} Dst;

#define SWZ(x, y, z, w) (uint8_t)((x) | (y) << 2 | (z) << 4 | (w) << 6)
#define XYZW SWZ(0, 1, 2, 3)
#define XXXX SWZ(0, 0, 0, 0)
#define YYYY SWZ(1, 1, 1, 1)
#define ZZZZ SWZ(2, 2, 2, 2)
#define WWWW SWZ(3, 3, 3, 3)

static Src R(int i) { Src s = { MUX_R, (uint8_t)i, 0, 0, XYZW }; return s; }
static Src V(int i) { Src s = { MUX_V, (uint8_t)i, 0, 0, XYZW }; return s; }
static Src C(int i) { Src s = { MUX_C, (uint8_t)i, 0, 0, XYZW }; return s; }
static Src CA(int i) { Src s = { MUX_C, (uint8_t)i, 0, 1, XYZW }; return s; }   /* c[A0 + i] */
static Src sw(Src s, uint8_t swz) { s.swz = swz; return s; }
static Src neg(Src s) { s.neg ^= 1; return s; }

static Dst T(int i, int mask) { Dst d = { 0, (uint8_t)i, (uint8_t)mask }; return d; }
static Dst O(int i, int mask) { Dst d = { 1, (uint8_t)i, (uint8_t)mask }; return d; }

enum { MX = 1, MY = 2, MZ = 4, MW = 8, MXYZ = 7, MXYZW = 15 };

static uint32_t hw_mask(uint8_t m) {
    return (uint32_t)((m & 1) << 3 | (m & 2) << 1 | (m & 4) >> 1 | (m & 8) >> 3);
}

typedef struct {
    VpProgram* p;
    int overflow;
} Gen;

static uint32_t swz_fields(uint8_t swz, int shift_x, int shift_y, int shift_z, int shift_w) {
    return (uint32_t)(swz & 3) << shift_x | (uint32_t)((swz >> 2) & 3) << shift_y |
           (uint32_t)((swz >> 4) & 3) << shift_z | (uint32_t)((swz >> 6) & 3) << shift_w;
}

/* operand slots: 0 = A, 1 = B, 2 = C */
static void encode(Gen* g, int mac, int ilu, Dst d, const Src* slot[3]) {
    uint32_t w1 = 0, w2 = 0, w3 = 0;
    int i;
    uint32_t* out;
    if (g->p->n >= VP_MAX_INSNS) {
        g->overflow = 1;
        return;
    }
    /* defaults: identity swizzles, V muxes, no outputs */
    w1 |= swz_fields(XYZW, 6, 4, 2, 0);
    w2 |= swz_fields(XYZW, 23, 21, 19, 17) | swz_fields(XYZW, 8, 6, 4, 2);
    w2 |= (uint32_t)MUX_V << 26 | (uint32_t)MUX_V << 11;
    w3 |= (uint32_t)MUX_V << 28;
    w3 |= 0xFFu << 3 | 1u << 11 | 7u << 20;   /* out address 0xFF, ORB = O, temp 7 */
    w1 |= (uint32_t)mac << 21 | (uint32_t)ilu << 25;
    for (i = 0; i < 3; i++) {
        const Src* s = slot[i];
        if (!s) continue;
        if (s->rel) w3 |= 1u << 1;
        if (s->mux == MUX_C) w1 = (w1 & ~(0xFFu << 13)) | (uint32_t)s->idx << 13;
        if (s->mux == MUX_V) w1 = (w1 & ~(0xFu << 9)) | (uint32_t)(s->idx & 15) << 9;
        switch (i) {
            case 0:
                w1 = (w1 & ~0xFFu) | swz_fields(s->swz, 6, 4, 2, 0);
                w1 = (w1 & ~(1u << 8)) | (uint32_t)s->neg << 8;
                w2 = (w2 & ~(3u << 26)) | (uint32_t)s->mux << 26;
                if (s->mux == MUX_R) w2 = (w2 & ~(0xFu << 28)) | (uint32_t)(s->idx & 15) << 28;
                break;
            case 1:
                w2 = (w2 & ~(0xFFu << 17)) | swz_fields(s->swz, 23, 21, 19, 17);
                w2 = (w2 & ~(1u << 25)) | (uint32_t)s->neg << 25;
                w2 = (w2 & ~(3u << 11)) | (uint32_t)s->mux << 11;
                if (s->mux == MUX_R) w2 = (w2 & ~(0xFu << 13)) | (uint32_t)(s->idx & 15) << 13;
                break;
            default:
                w2 = (w2 & ~(0xFFu << 2)) | swz_fields(s->swz, 8, 6, 4, 2);
                w2 = (w2 & ~(1u << 10)) | (uint32_t)s->neg << 10;
                w3 = (w3 & ~(3u << 28)) | (uint32_t)s->mux << 28;
                if (s->mux == MUX_R) {
                    w2 = (w2 & ~3u) | (uint32_t)((s->idx >> 2) & 3);
                    w3 = (w3 & ~(3u << 30)) | (uint32_t)(s->idx & 3) << 30;
                }
                break;
        }
    }
    if (mac != MAC_ARL) {
        if (d.out) {
            w3 = (w3 & ~(0xFFu << 3)) | (uint32_t)d.idx << 3;
            w3 |= hw_mask(d.mask) << 12;
            if (ilu && mac == MAC_NOP) w3 |= 1u << 2;   /* OUT_MUX = ILU */
        } else {
            w3 = (w3 & ~(0xFu << 20)) | (uint32_t)(d.idx & 15) << 20;
            if (mac != MAC_NOP) w3 |= hw_mask(d.mask) << 24;
            else w3 |= hw_mask(d.mask) << 16;
        }
    }
    out = &g->p->words[g->p->n * 4];
    out[0] = 0;
    out[1] = w1;
    out[2] = w2;
    out[3] = w3;
    g->p->n++;
}

static void mac1(Gen* g, int op, Dst d, Src a) { const Src* s[3] = { &a, NULL, NULL }; encode(g, op, ILU_NOP, d, s); }
static void mac2(Gen* g, int op, Dst d, Src a, Src b) { const Src* s[3] = { &a, &b, NULL }; encode(g, op, ILU_NOP, d, s); }
static void mad(Gen* g, Dst d, Src a, Src b, Src c) { const Src* s[3] = { &a, &b, &c }; encode(g, MAC_MAD, ILU_NOP, d, s); }
static void add(Gen* g, Dst d, Src a, Src c) { const Src* s[3] = { &a, NULL, &c }; encode(g, MAC_ADD, ILU_NOP, d, s); }
static void ilu(Gen* g, int op, Dst d, Src c) { const Src* s[3] = { NULL, NULL, &c }; encode(g, MAC_NOP, op, d, s); }
static void arl(Gen* g, Src a) { const Src* s[3] = { &a, NULL, NULL }; Dst d = { 0, 0, 0 }; encode(g, MAC_ARL, ILU_NOP, d, s); }

#define MOV(d, a) mac1(g, MAC_MOV, d, a)
#define MUL(d, a, b) mac2(g, MAC_MUL, d, a, b)
#define DP3(d, a, b) mac2(g, MAC_DP3, d, a, b)
#define DP4(d, a, b) mac2(g, MAC_DP4, d, a, b)
#define MAX(d, a, b) mac2(g, MAC_MAX, d, a, b)
#define MIN(d, a, b) mac2(g, MAC_MIN, d, a, b)
#define SGE(d, a, b) mac2(g, MAC_SGE, d, a, b)
#define ADD(d, a, c) add(g, d, a, c)
#define MAD(d, a, b, c) mad(g, d, a, b, c)
#define RCP(d, c) ilu(g, ILU_RCP, d, c)
#define RSQ(d, c) ilu(g, ILU_RSQ, d, c)
#define EXPP(d, c) ilu(g, ILU_EXP, d, c)   /* .z = 2^c (partial precision) */

#ifdef VP_HOST_TEST
/* tools/xbox/test_vp_encoder.py: the same instructions as nv2a-vsh source */
void vp_test_program(VpProgram* p) {
    Gen gen = { p, 0 }, *g = &gen;
    p->n = 0;
    arl(g, sw(V(1), XXXX));
    DP4(T(0, MX), V(0), CA(96));
    DP4(O(O_POS, MXYZW), R(3), C(4));
    MOV(T(5, MXYZ), neg(sw(R(2), SWZ(2, 1, 0, 3))));
    MUL(T(1, MXYZ), R(1), sw(R(3), XXXX));
    ADD(T(4, MXYZ), C(70), neg(R(0)));
    MAD(T(6, MXYZW), sw(R(5), ZZZZ), C(72), R(6));
    RSQ(T(5, MY), sw(R(5), XXXX));
    RCP(T(3, MX), sw(R(2), WWWW));
    MAX(T(5, MZ), R(5), sw(C(4), XXXX));
    MIN(O(O_D0, MXYZW), R(6), sw(C(4), YYYY));
    SGE(T(5, MW), sw(R(5), ZZZZ), sw(C(4), XXXX));
    DP3(O(O_T0 + 1, MX), V(10), C(111));
    MOV(O(O_T0 + 2, MZ | MW), sw(C(4), SWZ(0, 0, 0, 1)));
    RCP(O(O_FOG, MX), sw(R(11), YYYY));
    EXPP(T(3, MZ), sw(R(3), YYYY));
    MAD(O(O_FOG, MX), sw(R(3), ZZZZ), sw(C(136), ZZZZ), sw(C(136), YYYY));
}
#endif

/* ======================================================================
 * Generator
 * ====================================================================== */
/* temporaries */
enum { RPOS = 0, RNRM = 1, RCLIP = 2, RS = 3, RL = 4, RD = 5, RACC = 6, RA = 7, RB = 8, RC0 = 9, RC1 = 10, RT = 11 };

static Src K0(void) { return sw(C(VPC_K), XXXX); }
static Src K1(void) { return sw(C(VPC_K), YYYY); }

/* acc(mask) += attn * light colour, one light */
static void light(Gen* g, const VpChan* ch, int li, int mask) {
    int base = VPC_LIGHT + li * 5;
    Src lpos = C(base), ldir = C(base + 1), lcol = C(base + 2), la = C(base + 3), lk = C(base + 4);
    if (ch->attn == VPL_SPEC) {
        /* GC specular: gate by N.L >= 0 (L = light position, normalized on
         * the CPU), attn = a(N.H) / k(N.H) with H in the direction slot */
        DP3(T(RD, MZ), R(RNRM), lpos);
        SGE(T(RD, MW), sw(R(RD), ZZZZ), K0());
        DP3(T(RD, MX), R(RNRM), ldir);
        MAX(T(RD, MX), sw(R(RD), XXXX), K0());
        MUL(T(RD, MX), sw(R(RD), XXXX), sw(R(RD), WWWW));
        MOV(T(RA, MX), K1());
        MOV(T(RA, MY), sw(R(RD), XXXX));
        MUL(T(RA, MZ), sw(R(RD), XXXX), sw(R(RD), XXXX));
        DP3(T(RB, MX), R(RA), la);
        MAX(T(RB, MX), sw(R(RB), XXXX), K0());
        DP3(T(RB, MY), R(RA), lk);
        RCP(T(RB, MY), sw(R(RB), YYYY));
        MUL(T(RD, MZ), sw(R(RB), XXXX), sw(R(RB), YYYY));
    } else {
        /* L = normalize(lpos - P); diffuse = f(N.L) */
        ADD(T(RL, MXYZ), lpos, neg(R(RPOS)));
        DP3(T(RD, MX), R(RL), R(RL));
        RSQ(T(RD, MY), sw(R(RD), XXXX));
        MUL(T(RL, MXYZ), R(RL), sw(R(RD), YYYY));
        if (ch->diff_fn == 0 /* GX_DF_NONE */) {
            MOV(T(RD, MZ), K1());
        } else {
            DP3(T(RD, MZ), R(RNRM), R(RL));
            if (ch->diff_fn == 2 /* GX_DF_CLAMP */) MAX(T(RD, MZ), sw(R(RD), ZZZZ), K0());
        }
        if (ch->attn == VPL_SPOT) {
            /* angular a(cos) with cos = L . dir, distance 1 / k(d) */
            DP3(T(RD, MW), R(RL), ldir);
            MOV(T(RA, MX), K1());
            MOV(T(RA, MY), sw(R(RD), WWWW));
            MUL(T(RA, MZ), sw(R(RD), WWWW), sw(R(RD), WWWW));
            DP3(T(RA, MW), R(RA), la);
            MAX(T(RA, MW), sw(R(RA), WWWW), K0());
            MOV(T(RB, MX), K1());
            MUL(T(RB, MY), sw(R(RD), XXXX), sw(R(RD), YYYY));   /* d = d2 * rsq(d2) */
            MOV(T(RB, MZ), sw(R(RD), XXXX));
            DP3(T(RB, MW), R(RB), lk);
            RCP(T(RB, MW), sw(R(RB), WWWW));
            MUL(T(RA, MW), sw(R(RA), WWWW), sw(R(RB), WWWW));
            MUL(T(RD, MZ), sw(R(RD), ZZZZ), sw(R(RA), WWWW));
        }
    }
    MAD(T(RACC, mask), sw(R(RD), ZZZZ), lcol, R(RACC));
}

/* one colour channel (index 0 or 1) -> out register */
static void channel(Gen* g, const VpKey* k, int ci, int out_reg) {
    const VpChan* cc = &k->chan[ci * 2];
    const VpChan* ca = &k->chan[ci * 2 + 1];
    Src vcol = V(ci ? VPI_COL1 : VPI_COL0);
    Src cmat = C(VPC_CHAN + ci * 2), camb = C(VPC_CHAN + ci * 2 + 1);
    int rres = ci ? RC1 : RC0;
    int same = cc->enable == ca->enable && cc->light_mask == ca->light_mask && cc->attn == ca->attn &&
               cc->diff_fn == ca->diff_fn && cc->amb_vtx == ca->amb_vtx && cc->mat_vtx == ca->mat_vtx;
    int part;
    for (part = 0; part < (same ? 1 : 2); part++) {
        const VpChan* ch = part == 0 ? cc : ca;
        int mask = same ? MXYZW : part == 0 ? MXYZ : MW;
        int li;
        if (!ch->enable) {
            MOV(T(rres, mask), ch->mat_vtx ? vcol : cmat);
            continue;
        }
        MOV(T(RACC, mask), ch->amb_vtx ? vcol : camb);
        for (li = 0; li < 8; li++)
            if (ch->light_mask & (1u << li)) light(g, ch, li, mask);
        MAX(T(RACC, mask), R(RACC), K0());
        MIN(T(RACC, mask), R(RACC), K1());
        MUL(T(rres, mask), R(RACC), ch->mat_vtx ? vcol : cmat);
    }
    MOV(O(out_reg, MXYZW), R(rres));
}

static void texgen(Gen* g, const VpKey* k, int u) {
    const VpTexGen* t = &k->tex[u];
    int base = VPC_TEXGEN + u * 3;
    Src src;
    /* source vector with w = 1 in RS */
    switch (t->src) {
        case 0: /* GX_TG_POS: model-space position */
            MOV(T(RS, MXYZ), V(VPI_POS));
            break;
        case 1: /* GX_TG_NRM */
            if (k->has_nrm) MOV(T(RS, MXYZ), V(VPI_NRM));
            else MOV(T(RS, MXYZ), K0());
            break;
        default:
            if (t->src >= 4 && t->src <= 11) {   /* GX_TG_TEX0..7 */
                MOV(T(RS, MX | MY), V(vpi_tex(t->src - 4)));
                MOV(T(RS, MZ), K1());
            } else {
                MOV(T(RS, MXYZ), K0());   /* binormal, tangent, colour, bump: not supported */
                g->p->approximated = 1;
            }
            break;
    }
    MOV(T(RS, MW), K1());
    src = R(RS);
    DP4(T(RT, MX), src, C(base));
    DP4(T(RT, MY), src, C(base + 1));
    if (t->proj) DP4(T(RT, MW), src, C(base + 2));
    else MOV(T(RT, MW), K1());
    MOV(T(RT, MZ), K0());
    if (t->normalize) {
        /* normalize (s, t, q) then the post-transform matrix */
        MOV(T(RT, MZ), sw(R(RT), WWWW));
        DP3(T(RD, MX), R(RT), R(RT));
        RSQ(T(RD, MX), sw(R(RD), XXXX));
        MUL(T(RS, MXYZ), R(RT), sw(R(RD), XXXX));
        MOV(T(RS, MW), K1());
        DP4(T(RT, MX), R(RS), C(VPC_POSTMTX + u * 3));
        DP4(T(RT, MY), R(RS), C(VPC_POSTMTX + u * 3 + 1));
        DP4(T(RT, MW), R(RS), C(VPC_POSTMTX + u * 3 + 2));
        MOV(T(RT, MZ), K0());
    }
    MOV(O(O_T0 + u, MXYZW), R(RT));
}

void vp_generate(const VpKey* k, VpProgram* out) {
    Gen gen = { out, 0 }, *g = &gen;
    int needs_nrm = 0, i;
    memset(out, 0, sizeof *out);
    if (k->copy) {
        /* screen-space position and a texel-space coordinate, straight through */
        MOV(O(O_POS, MXYZW), V(VPI_POS));
        MOV(O(O_T0, MXYZW), V(vpi_tex(0)));
        MOV(O(O_D0, MXYZW), K1());
        MOV(O(O_D1, MXYZW), K0());
        out->words[(out->n - 1) * 4 + 3] |= 1u;   /* FINAL */
        return;
    }
    for (i = 0; i < 4; i++)
        if (i < k->nchans * 2 && k->chan[i].enable && k->chan[i].light_mask) needs_nrm = 1;
    needs_nrm = needs_nrm && k->has_nrm;

    /* position: a0 = PNMTXIDX (the current matrix when the vertex has none) */
    arl(g, sw(V(VPI_MTX), XXXX));
    DP4(T(RPOS, MX), V(VPI_POS), CA(VPC_POS));
    DP4(T(RPOS, MY), V(VPI_POS), CA(VPC_POS + 1));
    DP4(T(RPOS, MZ), V(VPI_POS), CA(VPC_POS + 2));
    MOV(T(RPOS, MW), K1());
    DP4(T(RCLIP, MX), R(RPOS), C(VPC_PROJ));
    DP4(T(RCLIP, MY), R(RPOS), C(VPC_PROJ + 1));
    DP4(T(RCLIP, MZ), R(RPOS), C(VPC_PROJ + 2));
    DP4(T(RCLIP, MW), R(RPOS), C(VPC_PROJ + 3));
    /* screen space: xyz / w, w kept for perspective-correct interpolation */
    RCP(T(RS, MX), sw(R(RCLIP), WWWW));
    MUL(O(O_POS, MXYZ), R(RCLIP), sw(R(RS), XXXX));
    MOV(O(O_POS, MW), R(RCLIP));

    if (k->fog) {
        /* GX fog from this vertex's depth (nv2a_fog.h): y = num.P / den.P,
         * then the curve; the fog unit takes oFog.x as the factor */
        DP4(T(RS, MY), R(RPOS), C(VPC_FOG));
        DP4(T(RS, MZ), R(RPOS), C(VPC_FOG + 1));
        RCP(T(RS, MZ), sw(R(RS), ZZZZ));
        if (k->fog == VPF_LIN) {
            MUL(O(O_FOG, MX), sw(R(RS), YYYY), sw(R(RS), ZZZZ));
        } else {
            MUL(T(RS, MY), sw(R(RS), YYYY), sw(R(RS), ZZZZ));
            MAX(T(RS, MY), sw(R(RS), YYYY), K0());
            MIN(T(RS, MY), sw(R(RS), YYYY), K1());
            if (k->fog == VPF_EXP2) MUL(T(RS, MY), sw(R(RS), YYYY), sw(R(RS), YYYY));
            MUL(T(RS, MY), sw(R(RS), YYYY), sw(C(VPC_FOG + 2), XXXX));
            EXPP(T(RS, MZ), sw(R(RS), YYYY));
            MAD(O(O_FOG, MX), sw(R(RS), ZZZZ), sw(C(VPC_FOG + 2), ZZZZ), sw(C(VPC_FOG + 2), YYYY));
        }
    }

    if (needs_nrm) {
        DP3(T(RNRM, MX), V(VPI_NRM), CA(VPC_NRM));
        DP3(T(RNRM, MY), V(VPI_NRM), CA(VPC_NRM + 1));
        DP3(T(RNRM, MZ), V(VPI_NRM), CA(VPC_NRM + 2));
        DP3(T(RS, MX), R(RNRM), R(RNRM));
        RSQ(T(RS, MX), sw(R(RS), XXXX));
        MUL(T(RNRM, MXYZ), R(RNRM), sw(R(RS), XXXX));
    } else {
        MOV(T(RNRM, MXYZ), sw(C(VPC_K), SWZ(0, 0, 1, 1)));
    }

    if (k->nchans >= 1) channel(g, k, 0, O_D0);
    else MOV(O(O_D0, MXYZW), K1());
    if (k->nchans >= 2) channel(g, k, 1, O_D1);
    else MOV(O(O_D1, MXYZW), K0());

    for (i = 0; i < k->ntex && i < 4; i++) texgen(g, k, i);

    if (g->overflow || out->n == 0) {
        /* too long (many spot lights): drop the lights and retry unlit */
        VpKey simple = *k;
        for (i = 0; i < 4; i++) simple.chan[i].light_mask &= 0x3;
        for (i = 0; i < 4; i++) simple.chan[i].attn = simple.chan[i].attn == VPL_SPOT ? VPL_DIFFUSE : simple.chan[i].attn;
        if (memcmp(&simple, k, sizeof simple) != 0) {
            vp_generate(&simple, out);
            out->approximated = 1;
            return;
        }
        out->approximated = 1;
    }
    out->words[(out->n - 1) * 4 + 3] |= 1u;   /* FINAL */
}
