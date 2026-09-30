/* nv2a_vp.c - NV2A vertex programs for GX transform and lighting.
 *
 * The GameCube lights per vertex with up to 8 lights on two colour channels
 * (each with a colour and an alpha half), skins with a per-vertex matrix
 * index, and generates up to 8 texture coordinates. One general program
 * would not fit the NV2A's 136 instructions, so a program is generated per
 * configuration (VpKey) and cached by the back end. Constants are laid out
 * per nv2a_vp.h and uploaded separately.
 *
 * The generator writes one operation per instruction over temporaries it
 * names (Op); vp_optimize then shortens the program without changing any
 * value in it (below). tests/xbox/test_vp_opt.c runs the programs of random
 * keys through an interpreter against the generator as it was before the
 * optimizer, and requires the same bits in every output.
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

/* One hardware instruction: a MAC op, an ILU op, or one of each. */
typedef struct {
    uint8_t mac, ilu;
    Src s[3];                  /* slots A, B, C (mux 0: unused); the ILU reads C */
    uint8_t tmp, tmask;        /* temp write: the MAC's, or the ILU's when it is alone */
    uint8_t imask;             /* temp write of an ILU op paired with a MAC op: always r1 */
    uint8_t out, omask, oilu;  /* output register write; oilu: the ILU's */
} Insn;

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

static uint32_t swz_fields(uint8_t swz, int shift_x, int shift_y, int shift_z, int shift_w) {
    return (uint32_t)(swz & 3) << shift_x | (uint32_t)((swz >> 2) & 3) << shift_y |
           (uint32_t)((swz >> 4) & 3) << shift_z | (uint32_t)((swz >> 6) & 3) << shift_w;
}

static void encode(const Insn* in, uint32_t* out) {
    uint32_t w1 = 0, w2 = 0, w3 = 0;
    int i;
    /* defaults: identity swizzles, V muxes, no outputs */
    w1 |= swz_fields(XYZW, 6, 4, 2, 0);
    w2 |= swz_fields(XYZW, 23, 21, 19, 17) | swz_fields(XYZW, 8, 6, 4, 2);
    w2 |= (uint32_t)MUX_V << 26 | (uint32_t)MUX_V << 11;
    w3 |= (uint32_t)MUX_V << 28;
    w3 |= 0xFFu << 3 | 1u << 11 | 7u << 20;   /* out address 0xFF, ORB = O, temp 7 */
    w1 |= (uint32_t)in->mac << 21 | (uint32_t)in->ilu << 25;
    for (i = 0; i < 3; i++) {
        const Src* s = &in->s[i];
        if (!s->mux) continue;
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
    if (in->tmask) {
        w3 = (w3 & ~(0xFu << 20)) | (uint32_t)(in->tmp & 15) << 20;
        w3 |= hw_mask(in->tmask) << (in->mac != MAC_NOP ? 24 : 16);
    }
    if (in->imask) {   /* the hardware writes r1; nv2a-vsh also names it in the temp field */
        if (!in->tmask) w3 = (w3 & ~(0xFu << 20)) | 1u << 20;
        w3 |= hw_mask(in->imask) << 16;
    }
    if (in->omask) {
        w3 = (w3 & ~(0xFFu << 3)) | (uint32_t)in->out << 3;
        w3 |= hw_mask(in->omask) << 12;
        if (in->oilu) w3 |= 1u << 2;   /* OUT_MUX = ILU */
    }
    out[0] = 0;
    out[1] = w1;
    out[2] = w2;
    out[3] = w3;
}

/* ======================================================================
 * Generator: one op per instruction, temporaries named r0..r11
 * ====================================================================== */
typedef struct {
    uint8_t mac, ilu;
    Dst d;
    Src s[3];
} Op;

#define VP_OPS_MAX 256

typedef struct {
    Op op[VP_OPS_MAX];
    int n;
    int overflow;
    int approximated;
} Gen;

static void emit(Gen* g, int mac, int ilu, Dst d, const Src* a, const Src* b, const Src* c) {
    Op* o;
    if (g->n >= VP_OPS_MAX) {
        g->overflow = 1;
        return;
    }
    o = &g->op[g->n++];
    memset(o, 0, sizeof *o);
    o->mac = (uint8_t)mac;
    o->ilu = (uint8_t)ilu;
    o->d = d;
    if (a) o->s[0] = *a;
    if (b) o->s[1] = *b;
    if (c) o->s[2] = *c;
}

static void mac1(Gen* g, int op, Dst d, Src a) { emit(g, op, ILU_NOP, d, &a, NULL, NULL); }
static void mac2(Gen* g, int op, Dst d, Src a, Src b) { emit(g, op, ILU_NOP, d, &a, &b, NULL); }
static void mad(Gen* g, Dst d, Src a, Src b, Src c) { emit(g, MAC_MAD, ILU_NOP, d, &a, &b, &c); }
static void add(Gen* g, Dst d, Src a, Src c) { emit(g, MAC_ADD, ILU_NOP, d, &a, NULL, &c); }
static void ilu(Gen* g, int op, Dst d, Src c) { emit(g, MAC_NOP, op, d, NULL, NULL, &c); }
static void arl(Gen* g, Src a) { Dst d = { 0, 0, 0 }; emit(g, MAC_ARL, ILU_NOP, d, &a, NULL, NULL); }

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

static Insn op_insn(const Op* o) {
    Insn in;
    memset(&in, 0, sizeof in);
    in.mac = o->mac;
    in.ilu = o->ilu;
    memcpy(in.s, o->s, sizeof in.s);
    if (o->mac == MAC_ARL) return in;
    if (o->d.out) {
        in.out = o->d.idx;
        in.omask = o->d.mask;
        in.oilu = o->mac == MAC_NOP;
    } else {
        in.tmp = o->d.idx;
        in.tmask = o->d.mask;
    }
    return in;
}

#ifdef VP_HOST_TEST
/* tools/xbox/test_vp_encoder.py: the same instructions as nv2a-vsh source */
static void pair(Gen* g, VpProgram* p, Insn in) {
    (void)g;
    encode(&in, &p->words[p->n * 4]);
    p->n++;
}

void vp_test_program(VpProgram* p) {
    static Gen gen;
    Gen* g = &gen;
    Insn in;
    int i;
    g->n = 0;
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
    p->n = 0;
    for (i = 0; i < g->n; i++) pair(g, p, op_insn(&g->op[i]));

    /* mul r2.xyz, r0, c4.x + rsq r1.y, r3.x */
    memset(&in, 0, sizeof in);
    in.mac = MAC_MUL, in.ilu = ILU_RSQ;
    in.s[0] = R(0), in.s[1] = sw(C(4), XXXX), in.s[2] = sw(R(3), XXXX);
    in.tmp = 2, in.tmask = MXYZ, in.imask = MY;
    pair(g, p, in);
    /* dp4 oPos.w, r0, c3 + rcp r1.x, r2.w */
    memset(&in, 0, sizeof in);
    in.mac = MAC_DP4, in.ilu = ILU_RCP;
    in.s[0] = R(0), in.s[1] = C(3), in.s[2] = sw(R(2), WWWW);
    in.out = O_POS, in.omask = MW, in.imask = MX;
    pair(g, p, in);
    /* mov r5.x, r3 + rcp oFog.x, r2.w */
    memset(&in, 0, sizeof in);
    in.mac = MAC_MOV, in.ilu = ILU_RCP;
    in.s[0] = R(3), in.s[2] = sw(R(2), WWWW);
    in.tmp = 5, in.tmask = MX, in.out = O_FOG, in.omask = MX, in.oilu = 1;
    pair(g, p, in);
    /* dp4 r2.w, r0, c3 + dp4 oPos.w, r0, c3 */
    memset(&in, 0, sizeof in);
    in.mac = MAC_DP4;
    in.s[0] = R(0), in.s[1] = C(3);
    in.tmp = 2, in.tmask = MW, in.out = O_POS, in.omask = MW;
    pair(g, p, in);
    /* mul r2.xyz, r0, c4.x + mov r1.y, c4.x */
    memset(&in, 0, sizeof in);
    in.mac = MAC_MUL, in.ilu = ILU_MOV;
    in.s[0] = R(0), in.s[1] = sw(C(4), XXXX), in.s[2] = sw(C(4), XXXX);
    in.tmp = 2, in.tmask = MXYZ, in.imask = MY;
    pair(g, p, in);
    /* dp3 r3.x, r1, r1 + mov oT0.zw, c4.xxxy */
    memset(&in, 0, sizeof in);
    in.mac = MAC_DP3, in.ilu = ILU_MOV;
    in.s[0] = R(1), in.s[1] = R(1), in.s[2] = sw(C(4), SWZ(0, 0, 0, 1));
    in.tmp = 3, in.tmask = MX, in.out = O_T0, in.omask = MZ | MW, in.oilu = 1;
    pair(g, p, in);
}
#endif

/* temporaries, as the generator names them */
enum { RPOS = 0, RNRM = 1, RCLIP = 2, RS = 3, RL = 4, RD = 5, RACC = 6, RA = 7, RB = 8, RC0 = 9, RC1 = 10, RT = 11 };

static Src K0(void) { return sw(C(VPC_K), XXXX); }
static Src K1(void) { return sw(C(VPC_K), YYYY); }

/* The constant 1 in the lanes the light loop builds its vectors around:
 * RA = (1, cos, cos^2) for spot lights and (1, x, x^2) for specular, RD =
 * (d^2, d, 1, rsq(d^2)); read as RD.zyx for the distance attenuation. */
static void light_setup(Gen* g, const VpKey* k, const VpChan* ch) {
    if (ch->attn == VPL_SPEC && ch->light_mask) MOV(T(RA, MX), K1());
    if (ch->attn != VPL_SPOT) return;
    if (ch->light_mask & ~k->ang_one) MOV(T(RA, MX), K1());
    if (ch->light_mask & ~k->dist_one) MOV(T(RD, MZ), K1());
}

/* acc(mask) += attn * light colour, one light; the factor ends in RB.x */
static void light(Gen* g, const VpKey* k, const VpChan* ch, int li, int mask) {
    int base = VPC_LIGHT + li * 5;
    Src lpos = C(base), ldir = C(base + 1), lcol = C(base + 2), la = C(base + 3), lk = C(base + 4);
    if (ch->attn == VPL_SPEC) {
        /* GC specular: gate by N.L >= 0 (L = light position, normalized on
         * the CPU), attn = a(N.H) / k(N.H) with H in the direction slot */
        DP3(T(RB, MZ), R(RNRM), lpos);
        SGE(T(RB, MW), sw(R(RB), ZZZZ), K0());
        DP3(T(RB, MX), R(RNRM), ldir);
        MAX(T(RB, MX), sw(R(RB), XXXX), K0());
        MUL(T(RA, MY), sw(R(RB), XXXX), sw(R(RB), WWWW));
        MUL(T(RA, MZ), sw(R(RA), YYYY), sw(R(RA), YYYY));
        DP3(T(RB, MX), R(RA), la);
        MAX(T(RB, MX), sw(R(RB), XXXX), K0());
        DP3(T(RB, MY), R(RA), lk);
        RCP(T(RB, MY), sw(R(RB), YYYY));
        MUL(T(RB, MX), sw(R(RB), XXXX), sw(R(RB), YYYY));
    } else {
        /* L = normalize(lpos - P); diffuse = f(N.L) */
        ADD(T(RL, MXYZ), lpos, neg(R(RPOS)));
        DP3(T(RD, MX), R(RL), R(RL));
        RSQ(T(RD, MW), sw(R(RD), XXXX));
        MUL(T(RL, MXYZ), R(RL), sw(R(RD), WWWW));
        if (ch->diff_fn == 0 /* GX_DF_NONE */) {
            MOV(T(RB, MX), K1());
        } else {
            DP3(T(RB, MX), R(RNRM), R(RL));
            if (ch->diff_fn == 2 /* GX_DF_CLAMP */) MAX(T(RB, MX), sw(R(RB), XXXX), K0());
        }
        if (ch->attn == VPL_SPOT) {
            /* angular a(cos) with cos = L . dir, distance 1 / k(d). An
             * identity term is 1 exactly (1 + 0 cos + 0 cos^2, rcp(1)), and
             * x * 1 = x, so it and its product are left out */
            int ang = !(k->ang_one >> li & 1), dist = !(k->dist_one >> li & 1);
            if (ang) {
                DP3(T(RA, MY), R(RL), ldir);
                MUL(T(RA, MZ), sw(R(RA), YYYY), sw(R(RA), YYYY));
                DP3(T(RA, MW), R(RA), la);
                MAX(T(RA, MW), sw(R(RA), WWWW), K0());
            }
            if (dist) {
                MUL(T(RD, MY), sw(R(RD), XXXX), sw(R(RD), WWWW));   /* d = d2 * rsq(d2) */
                DP3(T(RB, MW), sw(R(RD), SWZ(2, 1, 0, 3)), lk);      /* (1, d, d2) . k */
                RCP(T(RB, MW), sw(R(RB), WWWW));
            }
            if (ang && dist) MUL(T(RA, MW), sw(R(RA), WWWW), sw(R(RB), WWWW));
            if (ang || dist) MUL(T(RB, MX), sw(R(RB), XXXX), ang ? sw(R(RA), WWWW) : sw(R(RB), WWWW));
        }
    }
    MAD(T(RACC, mask), sw(R(RB), XXXX), lcol, R(RACC));
}

/* the lights of one channel half (or both, mask xyzw) into acc(mask) */
static void lights(Gen* g, const VpKey* k, const VpChan* ch, int mask) {
    int li;
    for (li = 0; li < 8; li++)
        if (ch->light_mask & (1u << li)) light(g, k, ch, li, mask);
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
    /* both halves lit by the same lights the same way: the per-light values
     * are the same for both, so one loop accumulates all four lanes */
    int shared = !same && cc->enable && ca->enable && cc->light_mask == ca->light_mask && cc->attn == ca->attn &&
                 cc->diff_fn == ca->diff_fn;
    int part;
    if (shared) {
        MOV(T(RACC, MXYZ), cc->amb_vtx ? vcol : camb);
        MOV(T(RACC, MW), ca->amb_vtx ? vcol : camb);
        light_setup(g, k, cc);
        lights(g, k, cc, MXYZW);
        MAX(T(RACC, MXYZW), R(RACC), K0());
        MIN(T(RACC, MXYZW), R(RACC), K1());
        MUL(T(rres, MXYZ), R(RACC), cc->mat_vtx ? vcol : cmat);
        MUL(T(rres, MW), R(RACC), ca->mat_vtx ? vcol : cmat);
    } else {
        for (part = 0; part < (same ? 1 : 2); part++) {
            const VpChan* ch = part == 0 ? cc : ca;
            int mask = same ? MXYZW : part == 0 ? MXYZ : MW;
            if (!ch->enable) {
                MOV(T(rres, mask), ch->mat_vtx ? vcol : cmat);
                continue;
            }
            MOV(T(RACC, mask), ch->amb_vtx ? vcol : camb);
            light_setup(g, k, ch);
            lights(g, k, ch, mask);
            MAX(T(RACC, mask), R(RACC), K0());
            MIN(T(RACC, mask), R(RACC), K1());
            MUL(T(rres, mask), R(RACC), ch->mat_vtx ? vcol : cmat);
        }
    }
    MOV(O(out_reg, MXYZW), R(rres));
}

static void texgen(Gen* g, const VpKey* k, int u) {
    const VpTexGen* t = &k->tex[u];
    int base = VPC_TEXGEN + u * 3;
    Src src = R(RS);
    /* the source vector with w = 1: position and normal attributes have
     * three components, so the vertex's own w is 1 */
    switch (t->src) {
        case 0: /* GX_TG_POS: model-space position */
            src = V(VPI_POS);
            break;
        case 1: /* GX_TG_NRM */
            if (k->has_nrm) {
                src = V(VPI_NRM);
            } else {
                MOV(T(RS, MXYZ), K0());
                MOV(T(RS, MW), K1());
            }
            break;
        default:
            if (t->src >= 4 && t->src <= 11) {   /* GX_TG_TEX0..7 */
                MOV(T(RS, MX | MY), V(vpi_tex(t->src - 4)));
                MOV(T(RS, MZ), K1());
            } else {
                MOV(T(RS, MXYZ), K0());   /* binormal, tangent, colour, bump: not supported */
                g->approximated = 1;
            }
            MOV(T(RS, MW), K1());
            break;
    }
    DP4(T(RT, MX), src, C(base));
    DP4(T(RT, MY), src, C(base + 1));
    if (!t->normalize) {
        if (t->proj) DP4(T(RT, MW), src, C(base + 2));
        else MOV(T(RT, MW), K1());
        MOV(T(RT, MZ), K0());
    } else {
        /* normalize (s, t, q) then the post-transform matrix */
        if (t->proj) DP4(T(RT, MZ), src, C(base + 2));
        else MOV(T(RT, MZ), K1());
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

static void gen_program(Gen* g, const VpKey* k) {
    int needs_nrm = 0, i;
    g->n = g->overflow = g->approximated = 0;
    if (k->copy) {
        /* screen-space position and a texel-space coordinate, straight through */
        MOV(O(O_POS, MXYZW), V(VPI_POS));
        MOV(O(O_T0, MXYZW), V(vpi_tex(0)));
        MOV(O(O_D0, MXYZW), K1());
        MOV(O(O_D1, MXYZW), K0());
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
}

/* ======================================================================
 * Optimizer
 *
 * The generated program is simple and long: it builds vectors in
 * temporaries and copies them to the outputs, computes lanes nobody reads,
 * and issues one op per instruction although the NV2A runs a MAC op and an
 * ILU op (RCP, RSQ, EXP, MOV) in the same instruction. Every step below
 * keeps each value's operation and operands, so the results are the same
 * bits:
 * - a MOV to an output register goes, and the ops that computed its source
 *   write the output themselves (and the temporary too if it is read again);
 * - lanes nobody reads aren't written, and ops left writing nothing go;
 * - MOVs of the same constant or input register into one destination merge;
 * - the ops are scheduled again in dependency order, pairing an ILU op (or
 *   a MOV) with a MAC op where both are ready (the paired ILU op writes r1),
 *   and the temporaries are allocated again for that order.
 * Vectors whose lanes come from several ops ("vreg", e.g. RPOS from three
 * DP4s and a MOV) stay together in one register. If allocation fails the
 * program is scheduled without pairing, and failing that it goes out as
 * generated.
 * ====================================================================== */
#define OPW ((VP_OPS_MAX + 31) / 32)
#define NREG 12   /* r0..r11; r12 reads oPos back */

typedef struct {
    int16_t w[3][4];   /* the op writing each temp lane a slot reads */
    uint8_t tmask;     /* temp lanes written (and read later, after dce) */
    uint8_t omask, oidx;
    uint8_t live, unit, paired, reg;
    int16_t parent;    /* vreg: union-find */
    int16_t pos;       /* instruction, -1: not scheduled */
    int16_t last;      /* instruction of its last reader */
    int16_t npred;     /* ops it waits for, not scheduled yet */
    int16_t nrd;       /* ops reading it, not scheduled yet */
    int16_t vsize;     /* ops in its vreg (at the vreg's root) */
} Node;

enum { U_MAC = 1, U_ILU = 2 };

static Node s_nd[VP_OPS_MAX];
static uint32_t s_pred[VP_OPS_MAX][OPW];   /* scheduled before this op */
static uint32_t s_rdr[VP_OPS_MAX][OPW];    /* ops reading this op's temp */
static uint32_t s_succ[VP_OPS_MAX][OPW];   /* ops waiting for this one */
static int16_t s_at[VP_MAX_INSNS][2];      /* the MAC and the ILU op of each instruction */

static int op_scalar(const Op* o) { return o->mac == MAC_ARL || (o->ilu >= ILU_RCP && o->ilu != ILU_LIT); }
static int op_bcast(const Op* o) {
    return o->mac == MAC_DP3 || o->mac == MAC_DP4 || o->ilu == ILU_RCP || o->ilu == ILU_RSQ;
}
static int op_lanewise(const Op* o) {
    return o->ilu == ILU_NOP && (o->mac == MAC_MOV || o->mac == MAC_MUL || o->mac == MAC_ADD || o->mac == MAC_MAD ||
                                 o->mac == MAC_MIN || o->mac == MAC_MAX || o->mac == MAC_SGE || o->mac == MAC_SLT);
}
/* a MAC op that leaves slot C to a paired ILU op */
static int op_pairs_mac(const Op* o) {
    return o->ilu == ILU_NOP && o->mac != MAC_NOP && o->mac != MAC_ADD && o->mac != MAC_MAD &&
           o->mac != MAC_ARL && o->mac != MAC_DPH && o->mac != MAC_DST;
}

/* the lanes of slot s's register an op reads when it writes `lanes` */
static uint8_t op_reads(const Op* o, int s, uint8_t lanes) {
    uint8_t swz = o->s[s].swz, r = 0;
    int c;
    if (!o->s[s].mux) return 0;
    if (o->mac == MAC_DP4) lanes = 15;
    else if (o->mac == MAC_DP3) lanes = 7;
    else if (op_scalar(o)) lanes = 1;
    for (c = 0; c < 4; c++)
        if (lanes >> c & 1) r |= (uint8_t)(1u << (swz >> 2 * c & 3));
    return r;
}

static uint8_t node_reads(const Gen* g, int i, int s) {
    const Op* o = &g->op[i];
    return o->s[s].mux == MUX_R ? op_reads(o, s, s_nd[i].tmask | s_nd[i].omask) : 0;
}

static int bit(const uint32_t* set, int i) { return (int)(set[i >> 5] >> (i & 31) & 1); }
/* the next member of a set from i on, -1: none */
static int next_bit(const uint32_t* set, int i) {
    int w = i >> 5;
    uint32_t b;
    if (i >= VP_OPS_MAX) return -1;
    b = set[w] & (0xFFFFFFFFu << (i & 31));
    while (!b) {
        if (++w >= OPW) return -1;
        b = set[w];
    }
    return w * 32 + __builtin_ctz(b);
}
static void set_bit(uint32_t* set, int i) { set[i >> 5] |= 1u << (i & 31); }

static int vr_find(int i) {
    while (s_nd[i].parent != i) i = s_nd[i].parent = s_nd[s_nd[i].parent].parent;
    return i;
}

/* nodes and the writer of every lane each op reads */
static int opt_build(const Gen* g) {
    int lastw[NREG][4], i, s, c;
    uint8_t written[16];
    memset(lastw, 0xFF, sizeof lastw);
    memset(written, 0, sizeof written);
    for (i = 0; i < g->n; i++) {
        const Op* o = &g->op[i];
        Node* n = &s_nd[i];
        memset(n, 0, sizeof *n);
        memset(n->w, 0xFF, sizeof n->w);
        n->live = 1;
        n->parent = (int16_t)i;
        n->pos = n->last = -1;
        if (o->mac != MAC_ARL) {
            if (o->d.out) {
                if (o->d.idx >= 16 || (written[o->d.idx] & o->d.mask)) return 0;   /* outputs are written once */
                written[o->d.idx] |= o->d.mask;
                n->omask = o->d.mask;
                n->oidx = o->d.idx;
            } else {
                if (o->d.idx >= NREG) return 0;
                n->tmask = o->d.mask;
            }
        }
        for (s = 0; s < 3; s++) {
            uint8_t rd = o->s[s].mux == MUX_R ? op_reads(o, s, o->d.mask) : 0;
            if (rd && o->s[s].idx >= NREG) return 0;
            for (c = 0; c < 4; c++) {
                if (!(rd >> c & 1)) continue;
                if (lastw[o->s[s].idx][c] < 0) return 0;
                n->w[s][c] = (int16_t)lastw[o->s[s].idx][c];
            }
        }
        if (n->tmask)
            for (c = 0; c < 4; c++)
                if (n->tmask >> c & 1) lastw[o->d.idx][c] = i;
    }
    return 1;
}

/* MOV o, r: the ops that wrote r's lanes write o instead */
static void opt_forward(const Gen* g) {
    int i, c;
    for (i = 0; i < g->n; i++) {
        const Op* o = &g->op[i];
        Node* m = &s_nd[i];
        int ok = 1;
        if (!m->live || o->mac != MAC_MOV || !m->omask || o->s[0].mux != MUX_R || o->s[0].neg) continue;
        for (c = 0; c < 4 && ok; c++) {
            int sc = o->s[0].swz >> 2 * c & 3, w;
            if (!(m->omask >> c & 1)) continue;
            w = m->w[0][sc];
            if (g->op[w].mac == MAC_ARL || (!op_bcast(&g->op[w]) && sc != c)) ok = 0;
            if (s_nd[w].omask && s_nd[w].oidx != m->oidx) ok = 0;
        }
        if (!ok) continue;
        for (c = 0; c < 4; c++) {
            int w;
            if (!(m->omask >> c & 1)) continue;
            w = m->w[0][o->s[0].swz >> 2 * c & 3];
            s_nd[w].omask |= (uint8_t)(1u << c);
            s_nd[w].oidx = m->oidx;
        }
        m->live = 0;
    }
}

/* drop the temp lanes nobody reads, and the ops left writing nothing */
static void opt_dce(const Gen* g) {
    static uint8_t need[VP_OPS_MAX];
    int i, s, c, arl = 0;
    memset(need, 0, sizeof need);
    for (i = g->n - 1; i >= 0; i--) {
        const Op* o = &g->op[i];
        Node* n = &s_nd[i];
        if (!n->live) continue;
        if (o->mac == MAC_ARL) {
            n->live = (uint8_t)arl;
            continue;
        }
        n->tmask &= need[i];
        if (!n->tmask && !n->omask) {
            n->live = 0;
            continue;
        }
        for (s = 0; s < 3; s++) {
            uint8_t rd = node_reads(g, i, s);
            if (o->s[s].rel) arl = 1;
            for (c = 0; c < 4; c++)
                if (rd >> c & 1) need[n->w[s][c]] |= (uint8_t)(1u << c);
        }
    }
}

static int src_eq(const Src* a, const Src* b) {
    return a->mux == b->mux && a->idx == b->idx && a->neg == b->neg && a->rel == b->rel;
}

/* two ops of one kind from the same constant or input rows into one
 * destination (MOV RS.z, 1 and MOV RS.w, 1): one op writing both */
static void opt_merge(Gen* g) {
    int i, j, k, s, c;
    for (j = 0; j < g->n; j++) {
        Op* oj = &g->op[j];
        Node* nj = &s_nd[j];
        if (!nj->live || !op_lanewise(oj)) continue;
        for (s = 0; s < 3; s++)
            if (oj->s[s].mux == MUX_R) break;
        if (s < 3) continue;
        for (i = j - 1; i >= 0; i--) {
            Op* oi = &g->op[i];
            Node* ni = &s_nd[i];
            int ok = 1;
            if (!ni->live || oi->mac != oj->mac || oi->ilu) continue;
            for (s = 0; s < 3; s++)
                if (!src_eq(&oi->s[s], &oj->s[s])) ok = 0;
            if (ok && nj->tmask)
                ok = !ni->omask && !nj->omask && oi->d.idx == oj->d.idx && !(ni->tmask & nj->tmask);
            else if (ok)
                ok = !ni->tmask && ni->oidx == nj->oidx && !(ni->omask & nj->omask);
            /* nothing in between may see the lanes the earlier op now writes */
            for (k = i + 1; k < j && ok && nj->tmask; k++) {
                if (!s_nd[k].live) continue;
                if (!g->op[k].d.out && g->op[k].d.idx == oj->d.idx && (s_nd[k].tmask & nj->tmask)) ok = 0;
                for (s = 0; s < 3; s++)
                    if (g->op[k].s[s].mux == MUX_R && g->op[k].s[s].idx == oj->d.idx &&
                        (node_reads(g, k, s) & nj->tmask))
                        ok = 0;
            }
            if (!ok) continue;
            for (s = 0; s < 3; s++)
                for (c = 0; c < 4; c++)
                    if ((nj->tmask | nj->omask) >> c & 1)
                        oi->s[s].swz = (uint8_t)((oi->s[s].swz & ~(3u << 2 * c)) | (oj->s[s].swz & (3u << 2 * c)));
            ni->tmask |= nj->tmask;
            ni->omask |= nj->omask;
            nj->live = 0;
            for (k = j + 1; k < g->n; k++)
                for (s = 0; s < 3; s++)
                    for (c = 0; c < 4; c++)
                        if (s_nd[k].w[s][c] == j) s_nd[k].w[s][c] = (int16_t)i;
            break;
        }
    }
}

/* vregs, and what each op must wait for */
static void opt_deps(const Gen* g) {
    int i, j, s, c, arl = -1;
    memset(s_pred, 0, sizeof s_pred);
    memset(s_rdr, 0, sizeof s_rdr);
    for (i = 0; i < g->n; i++) {
        const Op* o = &g->op[i];
        if (!s_nd[i].live) continue;
        if (o->mac == MAC_ARL) arl = i;
        for (s = 0; s < 3; s++) {
            uint8_t rd = node_reads(g, i, s);
            int first = -1;
            if (o->s[s].rel && arl >= 0) set_bit(s_pred[i], arl);
            for (c = 0; c < 4; c++) {
                int w;
                if (!(rd >> c & 1)) continue;
                w = s_nd[i].w[s][c];
                set_bit(s_pred[i], w);
                set_bit(s_rdr[w], i);
                if (first < 0) first = w;
                else s_nd[vr_find(w)].parent = (int16_t)vr_find(first);
            }
        }
    }
    for (i = 0; i < g->n; i++) s_nd[i].vsize = 0;
    for (i = 0; i < g->n; i++)
        if (s_nd[i].live) s_nd[vr_find(i)].vsize++;
    /* in a vreg, an op overwriting a lane waits for the lane's readers */
    for (j = 0; j < g->n; j++) {
        if (!s_nd[j].live || !s_nd[j].tmask) continue;
        for (i = 0; i < j; i++) {
            int wd;
            if (!s_nd[i].live || !(s_nd[i].tmask & s_nd[j].tmask) || vr_find(i) != vr_find(j)) continue;
            set_bit(s_pred[j], i);
            for (wd = 0; wd < OPW; wd++) s_pred[j][wd] |= s_rdr[i][wd];
            s_pred[j][j >> 5] &= ~(1u << (j & 31));
        }
    }
    memset(s_succ, 0, sizeof s_succ);
    for (j = 0; j < g->n; j++)
        if (s_nd[j].live)
            for (i = next_bit(s_pred[j], 0); i >= 0; i = next_bit(s_pred[j], i + 1)) set_bit(s_succ[i], j);
}

static int vreg_single(int i) { return s_nd[vr_find(i)].vsize == 1; }

static int popcount_and(const uint32_t* a, const uint32_t* b) {
    int w, n = 0;
    for (w = 0; w < OPW; w++) {
        uint32_t x = a[w] & b[w];
        while (x) x &= x - 1, n++;
    }
    return n;
}

/* can `ilu_op` run on the ILU next to `mac_op` on the MAC? */
static int can_pair(const Gen* g, int m, int l, uint8_t r1busy) {
    const Op* om = &g->op[m];
    const Op* ol = &g->op[l];
    const Src* srcs[3];
    int n = 0, i, j;
    if (!op_pairs_mac(om)) return 0;
    if (!(ol->ilu == ILU_RCP || ol->ilu == ILU_RSQ || ol->ilu == ILU_EXP || (ol->mac == MAC_MOV && !ol->ilu)))
        return 0;
    if (s_nd[m].omask && s_nd[l].omask) return 0;   /* one output write per instruction */
    if (s_nd[l].tmask && ((r1busy & s_nd[l].tmask) || !vreg_single(l))) return 0;
    /* one constant row (and one relative flag) and one input per instruction */
    for (i = 0; i < 3; i++)
        if (om->s[i].mux) srcs[n++] = &om->s[i];
    srcs[n++] = ol->mac == MAC_MOV ? &ol->s[0] : &ol->s[2];
    for (i = 0; i < n; i++)
        for (j = 0; j < i; j++)
            if (srcs[i]->mux == srcs[j]->mux && srcs[i]->mux != MUX_R &&
                (srcs[i]->idx != srcs[j]->idx || srcs[i]->rel != srcs[j]->rel))
                return 0;
    return 1;
}

static int is_ilu(const Gen* g, int i) { return g->op[i].ilu != ILU_NOP; }

/* list scheduling in program order; returns instructions, 0 on failure */
static int opt_schedule(const Gen* g, int pairing) {
    int cur = 0, left = 0, i, k;
    uint8_t r1busy = 0;
    int16_t r1own[4] = { -1, -1, -1, -1 };
    uint32_t live[OPW];
    memset(live, 0, sizeof live);
    for (i = 0; i < g->n; i++)
        if (s_nd[i].live) set_bit(live, i);
    for (i = 0; i < g->n; i++) {
        s_nd[i].pos = -1;
        s_nd[i].paired = 0;
        if (!s_nd[i].live) continue;
        left++;
        s_nd[i].npred = (int16_t)popcount_and(s_pred[i], live);
        s_nd[i].nrd = (int16_t)popcount_and(s_rdr[i], live);
    }
    while (left) {
        int a = -1, b = -1, placed[2], np, mac, ilu_n;
        if (cur >= VP_MAX_INSNS) return 0;
        for (i = 0; i < g->n; i++)
            if (s_nd[i].live && s_nd[i].pos < 0 && !s_nd[i].npred) { a = i; break; }
        if (a < 0) return 0;
        mac = is_ilu(g, a) ? -1 : a;
        ilu_n = is_ilu(g, a) ? a : -1;
        if (pairing) {
            for (i = a + 1; i < g->n && b < 0; i++) {
                if (!s_nd[i].live || s_nd[i].pos >= 0 || s_nd[i].npred) continue;
                if (is_ilu(g, a)) {
                    if (!is_ilu(g, i) && can_pair(g, i, a, r1busy)) b = i, mac = i;
                } else if (is_ilu(g, i) || g->op[i].mac == MAC_MOV) {
                    if (can_pair(g, a, i, r1busy)) b = i, ilu_n = i;
                    else if (g->op[a].mac == MAC_MOV && !is_ilu(g, i) && can_pair(g, i, a, r1busy))
                        b = i, mac = i, ilu_n = a;
                } else if (g->op[a].mac == MAC_MOV && can_pair(g, i, a, r1busy)) {
                    b = i, mac = i, ilu_n = a;
                }
            }
            if (b < 0) mac = is_ilu(g, a) ? -1 : a, ilu_n = is_ilu(g, a) ? a : -1;
        }
        s_at[cur][0] = (int16_t)mac;
        s_at[cur][1] = (int16_t)ilu_n;
        placed[0] = a;
        placed[1] = b;
        np = b < 0 ? 1 : 2;
        for (k = 0; k < np; k++) {
            Node* n = &s_nd[placed[k]];
            n->pos = (int16_t)cur;
            n->unit = placed[k] == ilu_n ? U_ILU : U_MAC;
            n->paired = (uint8_t)(np == 2);
            left--;
        }
        if (np == 2 && s_nd[ilu_n].tmask) {
            for (k = 0; k < 4; k++)
                if (s_nd[ilu_n].tmask >> k & 1) r1own[k] = (int16_t)ilu_n;
            r1busy |= s_nd[ilu_n].tmask;
        }
        /* their successors wait for one op fewer, the ops they read have one
         * reader fewer left */
        for (k = 0; k < np; k++) {
            int p = placed[k];
            for (i = next_bit(s_succ[p], 0); i >= 0; i = next_bit(s_succ[p], i + 1))
                if (s_nd[i].live) s_nd[i].npred--;
            for (i = next_bit(s_pred[p], 0); i >= 0; i = next_bit(s_pred[p], i + 1))
                if (bit(s_rdr[i], p)) s_nd[i].nrd--;
        }
        /* r1 lanes whose value has been read for the last time */
        for (k = 0; k < 4; k++)
            if (r1own[k] >= 0 && !s_nd[r1own[k]].nrd) {
                r1own[k] = -1;
                r1busy &= (uint8_t)~(1u << k);
            }
        cur++;
    }
    return cur;
}

/* one lane of two vregs in one register: live ranges [d, u] */
static int lane_clash(int d1, int u1, int d2, int u2) {
    if (d1 == d2) return 1;
    if (d1 > d2) {
        int t = d1;
        d1 = d2, d2 = t;
        t = u1, u1 = u2, u2 = t;
    }
    if (d2 < u1) return 1;
    /* written where the other is read last: fine for one op, but not
     * across the two ops of a pair (xemu runs the MAC op first) */
    return d2 == u1 && s_at[d2][0] >= 0 && s_at[d2][1] >= 0;
}

static int opt_alloc(const Gen* g) {
    static int16_t d[VP_OPS_MAX][4], u[VP_OPS_MAX][4], order[VP_OPS_MAX];
    static uint8_t pre[VP_OPS_MAX], nor1[VP_OPS_MAX], lanes[VP_OPS_MAX], done[VP_OPS_MAX];
    int i, j, c, n = 0;
    for (i = 0; i < g->n; i++) {
        pre[i] = nor1[i] = lanes[i] = done[i] = 0;
        for (c = 0; c < 4; c++) d[i][c] = 0x7FFF, u[i][c] = -1;
    }
    for (i = 0; i < g->n; i++) {
        Node* nd = &s_nd[i];
        int r;
        if (!nd->live || !nd->tmask) continue;
        nd->last = nd->pos;
        for (j = next_bit(s_rdr[i], 0); j >= 0; j = next_bit(s_rdr[i], j + 1))
            if (s_nd[j].live && s_nd[j].pos > nd->last) nd->last = s_nd[j].pos;
        r = vr_find(i);
        for (c = 0; c < 4; c++) {
            if (!(nd->tmask >> c & 1)) continue;
            if (nd->pos < d[r][c]) d[r][c] = nd->pos;
            if (nd->last > u[r][c]) u[r][c] = nd->last;
        }
        lanes[r] |= nd->tmask;
        if (nd->paired && nd->unit == U_ILU) pre[r] = 1;
        if (nd->paired && nd->unit == U_MAC) nor1[r] = 1;
    }
    /* r1's pairs first, then by first write */
    for (i = 0; i < g->n; i++)
        if (lanes[i] && pre[i]) order[n++] = (int16_t)i;
    for (i = 0; i < g->n; i++)
        if (lanes[i] && !pre[i]) {
            int16_t start = 0x7FFF;
            for (c = 0; c < 4; c++)
                if (d[i][c] < start) start = d[i][c];
            for (j = n; j > 0 && !pre[order[j - 1]]; j--) {
                int16_t s2 = 0x7FFF;
                for (c = 0; c < 4; c++)
                    if (d[order[j - 1]][c] < s2) s2 = d[order[j - 1]][c];
                if (s2 <= start) break;
                order[j] = order[j - 1];
            }
            order[j] = (int16_t)i;
            n++;
        }
    for (i = 0; i < n; i++) {
        int r = order[i], p, ok = 0;
        if (pre[r] && nor1[r]) return 0;
        for (p = pre[r] ? 1 : 0; p < NREG && !ok; p++) {
            int q;
            if (nor1[r] && p == 1) continue;
            ok = 1;
            for (q = 0; q < i && ok; q++) {
                int o = order[q];
                if (s_nd[o].reg != p || !(lanes[o] & lanes[r])) continue;
                for (c = 0; c < 4 && ok; c++)
                    if ((lanes[o] & lanes[r]) >> c & 1 && lane_clash(d[o][c], u[o][c], d[r][c], u[r][c])) ok = 0;
            }
            if (ok) s_nd[r].reg = (uint8_t)p;
            if (pre[r]) break;
        }
        if (!ok) return 0;
    }
    return 1;
}

static Src phys_src(const Gen* g, int i, int s) {
    Src r = g->op[i].s[s];
    int c;
    if (r.mux == MUX_R) {
        uint8_t rd = node_reads(g, i, s);
        for (c = 0; c < 4; c++)
            if (rd >> c & 1) {
                r.idx = s_nd[vr_find(s_nd[i].w[s][c])].reg;
                break;
            }
    }
    return r;
}

static void opt_emit(const Gen* g, int ninsn, VpProgram* out) {
    int p;
    for (p = 0; p < ninsn; p++) {
        Insn in;
        int m = s_at[p][0], l = s_at[p][1];
        memset(&in, 0, sizeof in);
        if (m >= 0) {
            const Op* o = &g->op[m];
            in.mac = o->mac;
            in.s[0] = phys_src(g, m, 0);
            in.s[1] = phys_src(g, m, 1);
            in.s[2] = phys_src(g, m, 2);
            if (s_nd[m].tmask) in.tmp = s_nd[vr_find(m)].reg, in.tmask = s_nd[m].tmask;
            if (s_nd[m].omask) in.out = s_nd[m].oidx, in.omask = s_nd[m].omask;
        }
        if (l >= 0) {
            const Op* o = &g->op[l];
            if (o->mac == MAC_MOV) {
                in.ilu = ILU_MOV;
                in.s[2] = phys_src(g, l, 0);
            } else {
                in.ilu = o->ilu;
                in.s[2] = phys_src(g, l, 2);
            }
            if (s_nd[l].tmask) {
                if (m >= 0) in.imask = s_nd[l].tmask;
                else in.tmp = s_nd[vr_find(l)].reg, in.tmask = s_nd[l].tmask;
            }
            if (s_nd[l].omask) in.out = s_nd[l].oidx, in.omask = s_nd[l].omask, in.oilu = 1;
        }
        encode(&in, &out->words[p * 4]);
    }
    out->n = (uint32_t)ninsn;
}

static int vp_optimize(Gen* g, VpProgram* out, int pairing) {
    int n;
    if (g->overflow || !opt_build(g)) return 0;
    opt_forward(g);
    opt_dce(g);
    opt_merge(g);
    opt_deps(g);
    n = opt_schedule(g, pairing);
    if (!n || !opt_alloc(g)) return 0;
    opt_emit(g, n, out);
    return 1;
}

/* ======================================================================
 * Keys
 * ====================================================================== */
/* Instructions the generator emitted for k before the optimizer. Keys whose
 * program was longer than 136 then are approximated (lights past the first
 * two dropped, spot attenuation off) exactly as they were, so the picture
 * doesn't change; most of them would fit now. */
static int light_len(const VpChan* ch) {
    if (ch->attn == VPL_SPEC) return 14;
    return 4 + (ch->diff_fn == 2 ? 2 : 1) + (ch->attn == VPL_SPOT ? 13 : 0) + 1;
}

static int legacy_len(const VpKey* k) {
    int n, i, ci;
    if (k->copy) return 4;
    n = 12 + (k->fog == VPF_LIN ? 4 : k->fog == VPF_EXP ? 9 : k->fog == VPF_EXP2 ? 10 : 0);
    ci = 0;
    for (i = 0; i < 4; i++)
        if (i < k->nchans * 2 && k->chan[i].enable && k->chan[i].light_mask) ci = 1;
    n += ci && k->has_nrm ? 6 : 1;
    for (ci = 0; ci < 2; ci++) {
        const VpChan* cc = &k->chan[ci * 2];
        const VpChan* ca = &k->chan[ci * 2 + 1];
        int same = cc->enable == ca->enable && cc->light_mask == ca->light_mask && cc->attn == ca->attn &&
                   cc->diff_fn == ca->diff_fn && cc->amb_vtx == ca->amb_vtx && cc->mat_vtx == ca->mat_vtx;
        int part;
        if (ci >= k->nchans) {
            n++;
            continue;
        }
        for (part = 0; part < (same ? 1 : 2); part++) {
            const VpChan* ch = part == 0 ? cc : ca;
            if (!ch->enable) {
                n++;
                continue;
            }
            n += 4;
            for (i = 0; i < 8; i++)
                if (ch->light_mask & (1u << i)) n += light_len(ch);
        }
        n++;
    }
    for (i = 0; i < k->ntex && i < 4; i++)
        n += (k->tex[i].src >= 4 && k->tex[i].src <= 11 ? 2 : 1) + 6 + (k->tex[i].normalize ? 9 : 0);
    return n;
}

void vp_canon(VpKey* k) {
    int i, ci, lit = 0, nrm_src = 0;
    if (k->copy) {
        memset(k, 0, sizeof *k);
        k->copy = 1;
        return;
    }
    for (ci = 0; ci < 2; ci++) {
        VpChan raw[2];
        int differ;
        memcpy(raw, &k->chan[ci * 2], sizeof raw);
        differ = memcmp(&raw[0], &raw[1], sizeof raw[0]) != 0;
        for (i = ci * 2; i < ci * 2 + 2; i++) {
            VpChan* c = &k->chan[i];
            if (ci >= k->nchans) {
                memset(c, 0, sizeof *c);
            } else if (!c->enable) {
                uint8_t m = c->mat_vtx;
                memset(c, 0, sizeof *c);
                c->mat_vtx = m;
            } else if (!c->light_mask) {
                c->attn = c->diff_fn = 0;
            } else if (c->attn == VPL_SPEC) {
                c->diff_fn = 0;
            }
        }
        /* halves the generator (and legacy_len) took apart stay apart */
        if (ci < k->nchans && differ && !memcmp(&k->chan[ci * 2], &k->chan[ci * 2 + 1], sizeof raw[0]))
            memcpy(&k->chan[ci * 2], raw, sizeof raw);
    }
    for (i = 0; i < 4; i++)
        if (k->chan[i].enable && k->chan[i].light_mask) lit = 1;
    for (i = 0; i < 4; i++) {
        if (i >= k->ntex) memset(&k->tex[i], 0, sizeof k->tex[i]);
        else if (k->tex[i].src == 1) nrm_src = 1;
    }
    if (!lit && !nrm_src) k->has_nrm = 0;
    /* the identity flags matter for the lights of spot channels only */
    ci = 0;
    for (i = 0; i < 4; i++)
        if (k->chan[i].enable && k->chan[i].attn == VPL_SPOT) ci |= k->chan[i].light_mask;
    k->ang_one &= (uint8_t)ci;
    k->dist_one &= (uint8_t)ci;
    k->pad = 0;
}

void vp_generate(const VpKey* key, VpProgram* out) {
    static Gen gen;
    Gen* g = &gen;
    VpKey k = *key;
    int i;
    memset(out, 0, sizeof *out);
    if (legacy_len(&k) > VP_MAX_INSNS) {
        /* too long (many spot lights): drop the lights and retry unlit */
        VpKey simple = k;
        for (i = 0; i < 4; i++) simple.chan[i].light_mask &= 0x3;
        for (i = 0; i < 4; i++) simple.chan[i].attn = simple.chan[i].attn == VPL_SPOT ? VPL_DIFFUSE : simple.chan[i].attn;
        if (memcmp(&simple, &k, sizeof simple) != 0) {
            k = simple;
            out->approximated = 1;
        }
    }
    gen_program(g, &k);
    if (g->approximated) out->approximated = 1;
    if (!vp_optimize(g, out, 1) && !vp_optimize(g, out, 0)) {
        /* as generated, cut at 136 if need be */
        for (i = 0; i < g->n && i < VP_MAX_INSNS; i++) {
            Insn in = op_insn(&g->op[i]);
            encode(&in, &out->words[i * 4]);
        }
        out->n = (uint32_t)i;
        if (g->n > VP_MAX_INSNS || g->overflow) out->approximated = 1;
    }
    out->words[(out->n - 1) * 4 + 3] |= 1u;   /* FINAL */
}

#ifdef VP_HOST_TEST
/* tests/xbox/test_vp_opt.c: the program as generated, not optimized */
int vp_test_unoptimized(const VpKey* key, uint32_t* words, int max) {
    static Gen gen;
    int i;
    gen_program(&gen, key);
    for (i = 0; i < gen.n && i < max; i++) {
        Insn in = op_insn(&gen.op[i]);
        encode(&in, &words[i * 4]);
    }
    if (i) words[(i - 1) * 4 + 3] |= 1u;
    return i;
}

int vp_test_legacy_len(const VpKey* k) { return legacy_len(k); }
#endif
