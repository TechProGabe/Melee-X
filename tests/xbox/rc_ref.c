/* rc_ref.c - xbox/src/hw/nv2a_rc.c as it was before swap tables (only the
 * names changed), for tools/xbox/test_rc.py: configurations that read no
 * swap table must still compile to the same words.
 *
 * nv2a_rc.c - GameCube TEV configuration -> NV2A register combiners.
 * From OpenCrossing-Xbox's xbox_tev_rc.c, generalised for Melee: up to 8
 * combiner stages, four texture units, both colour channels.
 *
 * A TEV stage computes, per colour and alpha,
 *     out = clamp((d OP ((1-c)*a + c*b) + bias) * scale)
 * and an NV2A general combiner stage computes AB + CD with per-input
 * mappings (invert, negate, half-bias, expand) and an output shift/bias. A
 * TEV stage becomes one NV2A stage when its lerp collapses (the common
 * modulate / replace / decal configurations) and two when it does not.
 *
 * Registers:
 *   R0            PREV (the final combiner reads it)
 *   R1, spare T*  REG0..REG2 once written, scratch; T* are the texture units
 *   (and V1)      no stage samples, and V1 while colour channel 1 is unused
 *   T0..T3        the textures of the units the stages sample
 *   V0 / V1       rasterised colour channels 0 / 1
 *   C0 / C1       per stage: KONST and not-yet-written TEV registers
 * Anything that does not fit is approximated and flagged. */
#include <string.h>

#include "rc_ref.h"

enum { S_ZERO = 0, S_C0 = 1, S_C1 = 2, S_FOG = 3, S_V0 = 4, S_V1 = 5,
       S_T0 = 8, S_T1 = 9, S_T2 = 10, S_T3 = 11, S_R0 = 12, S_R1 = 13 };
enum { M_UID = 0, M_UINV = 1, M_EXPN = 2, M_EXPNEG = 3, M_HBN = 4, M_HBNEG = 5, M_SID = 6, M_SNEG = 7 };
enum { OP_NOSHIFT = 0, OP_NOSHIFT_BIAS = 1, OP_SHL1 = 2, OP_SHL1_BIAS = 3, OP_SHL2 = 4, OP_SHR1 = 6 };

typedef struct {
    uint8_t src, alpha, map;
    uint16_t cref;
} Op;

static const Op OP_ZERO = { S_ZERO, 0, M_UID, 0 };
static const Op OP_ONE = { S_ZERO, 0, M_UINV, 0 };
static const Op OP_HALF = { S_ZERO, 0, M_HBNEG, 0 };

static int is_zero(Op o) { return o.src == S_ZERO && !o.cref && o.map == M_UID; }
static int is_one(Op o) { return o.src == S_ZERO && !o.cref && o.map == M_UINV; }
static int same(Op a, Op b) { return a.src == b.src && a.alpha == b.alpha && a.map == b.map && a.cref == b.cref; }

static Op op_inv(Op o) {
    if (o.src == S_ZERO && !o.cref) {
        if (o.map == M_UID) return OP_ONE;
        if (o.map == M_UINV) return OP_ZERO;
        return o;
    }
    if (o.map == M_UID) o.map = M_UINV;
    else if (o.map == M_UINV) o.map = M_UID;
    return o;
}

static int op_neg(Op o, Op* out) {
    if (o.src == S_ZERO && !o.cref) {
        if (o.map == M_UID) { *out = o; return 1; }
        if (o.map == M_UINV) { *out = o; out->map = M_EXPN; return 1; }
        if (o.map == M_HBNEG) { *out = o; out->map = M_HBN; return 1; }
        return 0;
    }
    if (o.map == M_UID) { *out = o; out->map = M_SNEG; return 1; }
    return 0;
}

/* ---- register allocation ---- */
typedef struct {
    uint8_t rgb[4], a[4];          /* where TEV reg r lives; 0: still its constant */
    const RefCfg* cfg;
    uint8_t pool_rgb[6], pool_a[6];
    int npool_rgb, npool_a;
} Alloc;

static int hw_used(const Alloc* al, int alpha, uint8_t hw) {
    int r;
    for (r = 0; r < 4; r++)
        if ((alpha ? al->a[r] : al->rgb[r]) == hw) return 1;
    return 0;
}

static uint8_t pick_free(const Alloc* al, int alpha, uint8_t avoid) {
    const uint8_t* pool = alpha ? al->pool_a : al->pool_rgb;
    int n = alpha ? al->npool_a : al->npool_rgb, i;
    for (i = 0; i < n; i++)
        if (pool[i] != avoid && !hw_used(al, alpha, pool[i])) return pool[i];
    return 0;
}

static Op reg_op(const Alloc* al, int reg, int want_alpha) {
    Op o = OP_ZERO;
    uint8_t hw = want_alpha ? al->a[reg] : al->rgb[reg];
    if (hw) {
        o.src = hw;
        o.alpha = (uint8_t)want_alpha;
    } else {
        o.cref = want_alpha ? RREF(RREF_TEVREG_A, reg) : RREF(RREF_TEVREG_RGB, reg);
        o.alpha = (uint8_t)want_alpha;
    }
    return o;
}

static Op ras_op(const RefStage* ts, int alpha) {
    Op o = OP_ZERO;
    if (ts->ras == 2) return OP_ZERO;
    o.src = ts->ras ? S_V1 : S_V0;
    o.alpha = (uint8_t)(alpha || ts->ras_alpha_bcast);
    return o;
}

static Op tex_op(const RefStage* ts, int alpha) {
    Op o = OP_ZERO;
    if (ts->unit < 0) return OP_ONE;
    o.src = (uint8_t)(S_T0 + ts->unit);
    o.alpha = (uint8_t)(alpha || ts->tex_alpha_bcast);
    return o;
}

static Op color_arg(const Alloc* al, int arg, const RefStage* ts) {
    Op o = OP_ZERO;
    switch (arg) {
        case 0: return reg_op(al, 0, 0);    /* CPREV */
        case 1: return reg_op(al, 0, 1);    /* APREV */
        case 2: return reg_op(al, 1, 0);    /* C0 */
        case 3: return reg_op(al, 1, 1);
        case 4: return reg_op(al, 2, 0);
        case 5: return reg_op(al, 2, 1);
        case 6: return reg_op(al, 3, 0);
        case 7: return reg_op(al, 3, 1);
        case 8: return tex_op(ts, 0);       /* TEXC */
        case 9: return tex_op(ts, 1);       /* TEXA */
        case 10: return ras_op(ts, 0);      /* RASC */
        case 11: return ras_op(ts, 1);      /* RASA */
        case 12: return OP_ONE;
        case 13: return OP_HALF;
        case 14: o.cref = RREF(RREF_KONST_C, ts->kcsel); return o;
        default: return OP_ZERO;
    }
}

static Op alpha_arg(const Alloc* al, int arg, const RefStage* ts) {
    Op o = OP_ZERO;
    switch (arg) {
        case 0: return reg_op(al, 0, 1);
        case 1: return reg_op(al, 1, 1);
        case 2: return reg_op(al, 2, 1);
        case 3: return reg_op(al, 3, 1);
        case 4: return tex_op(ts, 1);
        case 5: return ras_op(ts, 1);
        case 6: o.cref = RREF(RREF_KONST_A, ts->kasel); o.alpha = 1; return o;
        default: return OP_ZERO;
    }
}

/* ---- one portion (rgb or alpha) of one TEV stage ---- */
typedef struct { Op a, b; } Term;

typedef struct {
    int n;
    Term t[2][2];
    uint8_t dst[2];
    uint8_t op[2];
} Portion;

static void emit_portion(Op a, Op b, Op c, Op d, int sub, int bias, int scale, uint8_t dst, uint8_t scratch,
                         Portion* p, int* approx) {
    Term lerp[2], terms[4];
    int nl = 0, nt = 0, i, shift;
    if (is_zero(c) || same(a, b)) {
        if (!is_zero(a)) { lerp[nl].a = a; lerp[nl].b = OP_ONE; nl++; }
    } else if (is_one(c)) {
        if (!is_zero(b)) { lerp[nl].a = b; lerp[nl].b = OP_ONE; nl++; }
    } else {
        if (!is_zero(a)) { lerp[nl].a = a; lerp[nl].b = op_inv(c); nl++; }
        if (!is_zero(b)) { lerp[nl].a = b; lerp[nl].b = c; nl++; }
    }
    switch (scale) {
        case 1: shift = OP_SHL1; break;
        case 2: shift = OP_SHL2; break;
        case 3: shift = OP_SHR1; break;
        default: shift = OP_NOSHIFT; break;
    }
    {
        int ok = 1;
        for (i = 0; i < nl; i++) {
            Term t = lerp[i];
            if (sub) {
                Op n;
                if (op_neg(t.a, &n)) t.a = n;
                else if (op_neg(t.b, &n)) t.b = n;
                else { ok = 0; break; }
            }
            terms[nt++] = t;
        }
        if (ok && !is_zero(d)) { terms[nt].a = d; terms[nt].b = OP_ONE; nt++; }
        if (ok) {
            int op = shift;
            if (bias == 2 && (shift == OP_NOSHIFT || shift == OP_SHL1)) op = shift + 1;
            else if (bias == 1) { terms[nt].a = OP_HALF; terms[nt].b = OP_ONE; nt++; }
            else if (bias == 2) { terms[nt].a = OP_HALF; terms[nt].b = OP_ONE; op_neg(terms[nt].a, &terms[nt].a); nt++; }
            if (nt <= 2) {
                Term z = { OP_ZERO, OP_ZERO };
                p->n = 1;
                p->t[0][0] = nt > 0 ? terms[0] : z;
                p->t[0][1] = nt > 1 ? terms[1] : z;
                p->dst[0] = dst;
                p->op[0] = (uint8_t)op;
                return;
            }
        }
    }
    if (!scratch) { *approx = 1; scratch = dst; }
    {
        Term z = { OP_ZERO, OP_ZERO };
        int op = shift;
        Op s = { scratch, 0, M_UID, 0 };
        p->n = 2;
        p->t[0][0] = nl > 0 ? lerp[0] : z;
        p->t[0][1] = nl > 1 ? lerp[1] : z;
        p->dst[0] = scratch;
        p->op[0] = OP_NOSHIFT;
        if (bias == 2 && (shift == OP_NOSHIFT || shift == OP_SHL1)) {
            op = shift + 1;
        } else if (bias && nl < 2) {
            Term h = { OP_HALF, OP_ONE };
            int want_neg = (bias == 1) == (sub != 0);
            if (want_neg) op_neg(h.a, &h.a);
            p->t[0][1] = h;
        } else if (bias) {
            *approx = 1;
        }
        if (sub) s.map = M_SNEG;
        p->t[1][0].a = d;
        p->t[1][0].b = OP_ONE;
        p->t[1][1].a = s;
        p->t[1][1].b = OP_ONE;
        p->dst[1] = dst;
        p->op[1] = (uint8_t)op;
    }
}

static uint8_t alloc_const(uint16_t slots[4], uint16_t ref, int alpha_slot, int* approx) {
    int base = alpha_slot ? 1 : 0, k;
    for (k = 0; k < 2; k++)
        if (slots[base + 2 * k] == ref) return (uint8_t)(k ? S_C1 : S_C0);
    for (k = 0; k < 2; k++)
        if (slots[base + 2 * k] == 0) {
            slots[base + 2 * k] = ref;
            return (uint8_t)(k ? S_C1 : S_C0);
        }
    *approx = 1;
    return S_ZERO;
}

static uint32_t enc_in(Op o) { return (uint32_t)(o.src & 0xF) | (uint32_t)(o.alpha & 1) << 4 | (uint32_t)(o.map & 7) << 5; }

static void resolve(Op* o, uint16_t slots[4], int alpha_portion, int* approx) {
    int use_alpha;
    if (!o->cref) return;
    use_alpha = alpha_portion || o->alpha;
    o->src = alloc_const(slots, o->cref, use_alpha, approx);
    o->alpha = (uint8_t)use_alpha;
    o->cref = 0;
}

static uint32_t make_icw(Term ab, Term cd) {
    return enc_in(ab.a) << 24 | enc_in(ab.b) << 16 | enc_in(cd.a) << 8 | enc_in(cd.b);
}

static uint32_t make_ocw(uint8_t dst, uint8_t op) { return (uint32_t)(dst & 0xF) << 8 | (uint32_t)(op & 7) << 15; }

/* ---- movies: YUV -> RGB ----
 * sobjlib.c draws THP frames with Nintendo's stock 4-stage recipe (Cb, Cr,
 * Y as I8 textures; a negative S10 bias in REG0; unclamped intermediates;
 * G computed in the alpha path and moved over by a KONST lerp). Constants
 * here are unsigned and PREV reads clamp at 0, so the bias would vanish
 * and the picture go magenta. Recognise it and emit the same arithmetic
 * with signed registers:
 *   R = Y + 1.404 Cr - 0.702     = Y + 2 * 0.351 * (2Cr - 1)
 *   B = Y + 1.772 Cb - 0.886     = Y + 2 * 0.443 * (2Cb - 1)
 *   G = Y + 0.529 - 0.345 Cb - 0.714 Cr
 *     ~ Y - 0.1725 (2Cb - 1) - 0.357 (2Cr - 1)
 * The GX version subtracts 0.894 in B and keeps 0.0005 in G; the
 * difference is under one step of 8-bit colour. */
static int is_yuv_recipe(const RefCfg* c) {
    static const uint8_t cin[4][4] = { { 15, 8, 14, 2 }, { 15, 8, 14, 0 }, { 15, 8, 12, 0 }, { 1, 0, 14, 15 } };
    static const uint8_t ain[3][4] = { { 7, 4, 6, 1 }, { 7, 4, 6, 0 }, { 4, 7, 7, 0 } };
    int s, i;
    if (c->nstages != 4) return 0;
    for (s = 0; s < 4; s++)
        for (i = 0; i < 4; i++)
            if (c->st[s].cin[i] != cin[s][i]) return 0;
    for (s = 0; s < 3; s++)
        for (i = 0; i < 4; i++)
            if (c->st[s].ain[i] != ain[s][i]) return 0;
    return c->st[0].aop == 1 && c->st[1].aop == 1 && c->st[1].cscale == 1 && c->st[3].kcsel == 0x0E &&
           c->st[0].unit >= 0 && c->st[1].unit >= 0 && c->st[2].unit >= 0;
}

static void yuv_program(const RefCfg* cfg, RefProg* out) {
    const Op cb = { (uint8_t)(S_T0 + cfg->st[0].unit), 0, M_EXPN, 0 };
    const Op cr = { (uint8_t)(S_T0 + cfg->st[1].unit), 0, M_EXPN, 0 };
    const Op y = { (uint8_t)(S_T0 + cfg->st[2].unit), 0, M_UID, 0 };
    const Op c0 = { S_C0, 0, M_UID, 0 }, c1 = { S_C1, 0, M_UID, 0 };
    const Op r0 = { S_R0, 0, M_SID, 0 }, r0a = { S_R0, 1, M_SID, 0 };
    Term ab, cd, z = { OP_ZERO, OP_ZERO };
    Op cbn = cb, crn = cr;
    cbn.alpha = crn.alpha = 1;
    cbn.map = crn.map = M_EXPNEG;

    /* 0: rgb = 2 (0.351 (2Cr-1), 0, 0.443 (2Cb-1)); a = -(0.1725 (2Cb-1) + 0.357 (2Cr-1)) */
    ab.a = cr; ab.b = c0; cd.a = cb; cd.b = c1;
    out->cicw[0] = make_icw(ab, cd);
    out->cocw[0] = make_ocw(S_R0, OP_SHL1);
    ab.a = cbn; ab.b = c0; cd.a = crn; cd.b = c1;
    ab.b.alpha = cd.b.alpha = 1;
    out->aicw[0] = make_icw(ab, cd);
    out->aocw[0] = make_ocw(S_R0, OP_NOSHIFT);
    out->cref[0][0] = out->cref[0][1] = RREF(RREF_FIXED, 0);
    out->cref[0][2] = out->cref[0][3] = RREF(RREF_FIXED, 1);
    /* 1: rgb = (r, a, b) */
    ab.a = r0; ab.b = c0; cd.a = r0a; cd.b = c1;
    out->cicw[1] = make_icw(ab, cd);
    out->cocw[1] = make_ocw(S_R0, OP_NOSHIFT);
    out->aicw[1] = make_icw(z, z);
    out->aocw[1] = make_ocw(S_R0, OP_NOSHIFT);
    out->cref[1][0] = RREF(RREF_FIXED, 2);
    out->cref[1][2] = RREF(RREF_FIXED, 3);
    /* 2: rgb = Y + prev; alpha 0, as GX's stage 3 leaves it */
    ab.a = y; ab.b = OP_ONE; cd.a = r0; cd.b = OP_ONE;
    out->cicw[2] = make_icw(ab, cd);
    out->cocw[2] = make_ocw(S_R0, OP_NOSHIFT);
    out->aicw[2] = make_icw(z, z);
    out->aocw[2] = make_ocw(S_R0, OP_NOSHIFT);
    out->nstages = 3;
    out->cw0 = (uint32_t)S_R0 << 8;
    out->cw1 = (uint32_t)(S_R0 | 1 << 4) << 8 | 0x80;
}

void rc_ref_compile(const RefCfg* cfg, RefProg* out) {
    Alloc al;
    int s, n = 0, u;

    memset(out, 0, sizeof *out);
    if (is_yuv_recipe(cfg)) {
        yuv_program(cfg, out);
        return;
    }
    memset(&al, 0, sizeof al);
    al.cfg = cfg;
    al.pool_rgb[al.npool_rgb++] = S_R1;
    al.pool_a[al.npool_a++] = S_R1;
    for (u = 3; u >= 0; u--)
        if (!(cfg->units_used & (1u << u))) {
            al.pool_rgb[al.npool_rgb++] = (uint8_t)(S_T0 + u);
            al.pool_a[al.npool_a++] = (uint8_t)(S_T0 + u);
        }
    if (!cfg->v1_used) {
        al.pool_rgb[al.npool_rgb++] = S_V1;
        al.pool_a[al.npool_a++] = S_V1;
    }

    for (s = 0; s < cfg->nstages && s < RC_MAX_TEV; s++) {
        const RefStage* ts = &cfg->st[s];
        Portion pc, pa;
        uint8_t cdst, adst, cscr, ascr;
        int k, groups;

        if (ts->cop > 1 || ts->aop > 1) out->approximated = 1;   /* comparison modes */
        cdst = ts->cout == 0 ? S_R0 : al.rgb[ts->cout];
        if (!cdst) cdst = pick_free(&al, 0, 0);
        if (!cdst) { cdst = S_R1; out->approximated = 1; }
        adst = ts->aout == 0 ? S_R0 : al.a[ts->aout];
        if (!adst) adst = pick_free(&al, 1, 0);
        if (!adst) { adst = S_R1; out->approximated = 1; }
        cscr = pick_free(&al, 0, cdst);
        ascr = pick_free(&al, 1, adst);

        memset(&pc, 0, sizeof pc);
        memset(&pa, 0, sizeof pa);
        emit_portion(color_arg(&al, ts->cin[0], ts), color_arg(&al, ts->cin[1], ts), color_arg(&al, ts->cin[2], ts),
                     color_arg(&al, ts->cin[3], ts), ts->cop == 1, ts->cbias, ts->cscale, cdst, cscr, &pc,
                     &out->approximated);
        emit_portion(alpha_arg(&al, ts->ain[0], ts), alpha_arg(&al, ts->ain[1], ts), alpha_arg(&al, ts->ain[2], ts),
                     alpha_arg(&al, ts->ain[3], ts), ts->aop == 1, ts->abias, ts->ascale, adst, ascr, &pa,
                     &out->approximated);

        groups = pc.n > pa.n ? pc.n : pa.n;
        if (n + groups > RC_MAX_STAGES) {
            out->approximated = 1;
            break;
        }
        for (k = 0; k < groups; k++) {
            int ci = pc.n == groups ? k : k - (groups - pc.n);
            int ai = pa.n == groups ? k : k - (groups - pa.n);
            uint16_t* slots = out->cref[n];
            Term z = { OP_ZERO, OP_ZERO };
            Term cab = z, ccd = z, aab = z, acd = z;
            uint8_t cd = 0, ad = 0, cop = 0, aop = 0;
            if (ci >= 0) { cab = pc.t[ci][0]; ccd = pc.t[ci][1]; cd = pc.dst[ci]; cop = pc.op[ci]; }
            if (ai >= 0) { aab = pa.t[ai][0]; acd = pa.t[ai][1]; ad = pa.dst[ai]; aop = pa.op[ai]; }
            resolve(&cab.a, slots, 0, &out->approximated);
            resolve(&cab.b, slots, 0, &out->approximated);
            resolve(&ccd.a, slots, 0, &out->approximated);
            resolve(&ccd.b, slots, 0, &out->approximated);
            resolve(&aab.a, slots, 1, &out->approximated);
            resolve(&aab.b, slots, 1, &out->approximated);
            resolve(&acd.a, slots, 1, &out->approximated);
            resolve(&acd.b, slots, 1, &out->approximated);
            aab.a.alpha = aab.b.alpha = acd.a.alpha = acd.b.alpha = 1;
            out->cicw[n] = make_icw(cab, ccd);
            out->cocw[n] = make_ocw(cd, cop);
            out->aicw[n] = make_icw(aab, acd);
            out->aocw[n] = make_ocw(ad, aop);
            n++;
        }
        if (ts->cout != 0) al.rgb[ts->cout] = cdst; else al.rgb[0] = S_R0;
        if (ts->aout != 0) al.a[ts->aout] = adst; else al.a[0] = S_R0;
    }
    if (n == 0) n = 1;   /* one pass-through stage, PREV from its constant */
    out->nstages = n;

    /* final combiner: rgb = A*B + (1-A)*C + D with A = B = 0 -> C = PREV;
     * alpha = G = PREV.a */
    {
        Op prev_rgb = OP_ZERO, prev_a = OP_ZERO;
        if (al.rgb[0]) prev_rgb.src = al.rgb[0];
        else { prev_rgb.src = S_C1; out->fref[2] = RREF(RREF_TEVREG_RGB, 0); }
        if (al.a[0]) { prev_a.src = al.a[0]; prev_a.alpha = 1; }
        else { prev_a.src = S_C1; prev_a.alpha = 1; out->fref[3] = RREF(RREF_TEVREG_A, 0); }
        out->cw0 = (uint32_t)(prev_rgb.src | prev_rgb.alpha << 4) << 8;
        out->cw1 = (uint32_t)(prev_a.src | prev_a.alpha << 4) << 8 | 0x80;   /* specular clamp */
    }
}
