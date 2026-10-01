/* test_anim_mtx.c - host check that the Pentium III rewrites of sysdolphin's
 * animation and matrix code give the same bits as before. The current
 * fobj.c and mtx.c (built as on the Xbox, TARGET_XBOX with SSE) run side by
 * side with anim_mtx_ref.c, the code as it was, on:
 *   - pc_sincosf against pc_sinf and pc_cosf (sampled over all 2^32 floats,
 *     every float with --full), and the boundaries of their branches;
 *   - parseFloat for every frac byte and 16-bit pattern, and float keys;
 *   - the spline evaluation with 1/fterm in float, for every u16 fterm;
 *   - random keyframe streams of every opcode, interpolation and frac type
 *     (and garbage), interpreted frame by frame at random rates with stops
 *     and rewinds, comparing every update callback and the FObj state;
 *   - HSD_MtxSRT and the fused envelope blend on random matrices, angles and
 *     scales, including denormals, negative zero, infinities and NaNs.
 * Built and run by tools/xbox/test_anim_mtx.py. */
#include <stdio.h>
#include <stdlib.h>

#include "anim_mtx_ref.c"

/* what fobj.c and mtx.c link against */
void __assert(const char* file, u32 line, const char* cond)
{
    fprintf(stderr, "assert %s:%u: %s\n", file, (unsigned) line, cond);
    abort();
}
void* HSD_ObjAlloc(HSD_ObjAllocData* data)
{
    (void) data;
    return calloc(1, 64);
}
void HSD_ObjFree(HSD_ObjAllocData* data, void* obj)
{
    (void) data;
    free(obj);
}
void HSD_ObjAllocInit(HSD_ObjAllocData* data, size_t size, u32 align)
{
    (void) data, (void) size, (void) align;
}
uintptr_t OSBaseAddress;
void* pc_resolve_ext_ptr(uint32_t id)
{
    (void) id;
    return NULL;
}

static u64 rng_state = 0x9E3779B97F4A7C15ull;
static u32 rnd(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (u32) ((rng_state * 0x2545F4914F6CDD1Dull) >> 32);
}
static u32 rnd_below(u32 n)
{
    return (u32) (((u64) rnd() * n) >> 32);
}
static f32 f_of(u32 u)
{
    f32 f;
    memcpy(&f, &u, 4);
    return f;
}
static u32 u_of(f32 f)
{
    u32 u;
    memcpy(&u, &f, 4);
    return u;
}

static const u32 special_bits[] = {
    0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF,
    0x00800000, 0x80800000, 0x3F800000, 0xBF800000, 0x3F000000, 0x7F7FFFFF,
    0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7F800001,
    0x7FBFFFFF, 0x3A83126F, 0x3C8EFA35, 0x34000000,
    /* pc_sinf/pc_cosf branch boundaries */
    0x39800000, 0x3F490FDA, 0x4016CBE3, 0x407B53D1, 0x40AFEDDF, 0x40E231D5,
    0x40490FDB, 0x3FC90FDB, 0x40C90FDB, 0x4A800000, 0x4B000000, 0x5D000000,
};
#define N_SPECIAL (sizeof(special_bits) / sizeof(special_bits[0]))

/* a float that exercises the code: specials, their neighbours, angles near
 * multiples of pi/2, ordinary magnitudes, or any bit pattern */
static f32 rnd_f32(void)
{
    switch (rnd_below(8)) {
    case 0:
        return f_of(special_bits[rnd_below(N_SPECIAL)] ^ (rnd() & 0x80000000));
    case 1:
        return f_of(special_bits[rnd_below(N_SPECIAL)] + rnd_below(9) - 4);
    case 2: {
        f32 k = (f32) ((int) rnd_below(33) - 16);
        return f_of(u_of(k * 1.57079632679f) + rnd_below(5) - 2);
    }
    case 3:
        return f_of(rnd());
    case 4:
        return ((f32) rnd() / 4294967296.0f - 0.5f) * 2.0f;
    case 5:
        return ((f32) rnd() / 4294967296.0f - 0.5f) * 20.0f;
    case 6:
        return f_of((rnd() & 0x807FFFFF) | ((0x60 + rnd_below(0x40)) << 23));
    default:
        return ((f32) rnd() / 4294967296.0f) * 4.0f;
    }
}

/* Same bits, or both NaN. Where two NaN operands meet, x86 keeps the first
 * one's payload, and which is first is the compiler's choice (it commutes
 * IEEE-commutative operations, and did differently once the sines came from
 * memory): a NaN only has to stay a NaN. Payloads never agreed between
 * builds anyway (AArch64 makes the default NaN). */
static int same_f(u32 a, u32 b)
{
    return a == b || ((a & 0x7FFFFFFF) > 0x7F800000 && (b & 0x7FFFFFFF) > 0x7F800000);
}

static int failures;
static void fail(const char* what, u64 i, u32 got, u32 want)
{
    if (failures++ < 20) {
        fprintf(stderr, "FAIL %s #%llu: %08X, reference %08X\n", what,
                (unsigned long long) i, (unsigned) got, (unsigned) want);
    }
}

/* ---- pc_sincosf */

static void check_sincos_one(u32 bits, u64 i)
{
    f32 x = f_of(bits), s, c;

    pc_sincosf(x, &s, &c);
    if (!same_f(u_of(s), u_of(pc_sinf(x)))) {
        fail("pc_sincosf sin", i, u_of(s), u_of(pc_sinf(x)));
    }
    if (!same_f(u_of(c), u_of(pc_cosf(x)))) {
        fail("pc_sincosf cos", i, u_of(c), u_of(pc_cosf(x)));
    }
}

static void test_sincos(int full, u64 samples)
{
    u64 i;
    u32 k;

    for (k = 0; k < N_SPECIAL; k++) {
        int d;
        for (d = -64; d <= 64; d++) {
            check_sincos_one(special_bits[k] + d, k);
            check_sincos_one((special_bits[k] ^ 0x80000000) + d, k);
        }
    }
    if (full) {
        for (i = 0; i <= 0xFFFFFFFFull; i++) {
            check_sincos_one((u32) i, i);
        }
        printf("pc_sincosf: all 2^32 floats\n");
    } else {
        /* an odd stride visits 2^32 / gcd = every residue: a spread sample */
        for (i = 0; i < samples; i++) {
            check_sincos_one((u32) (i * 0x9E3779B1u + 0x1234567u), i);
        }
        printf("pc_sincosf: %llu floats + branch boundaries\n",
               (unsigned long long) samples);
    }
}

/* ---- parseFloat and the spline */

static void test_parse_float(void)
{
    u32 frac, pat;
    u64 n = 0;

    for (frac = 0; frac < 256; frac++) {
        for (pat = 0; pat < 0x10000; pat++) {
            u8 buf[8];
            u8 *pa = buf, *pb = buf;
            u32 hi = rnd();
            f32 a, b;

            buf[0] = (u8) pat, buf[1] = (u8) (pat >> 8);
            buf[2] = (u8) hi, buf[3] = (u8) (hi >> 8);
            buf[4] = buf[5] = buf[6] = buf[7] = 0;
            a = parseFloat(&pa, (u8) frac);
            b = ref_parseFloat(&pb, (u8) frac);
            if (!same_f(u_of(a), u_of(b)) || pa != pb) {
                fail("parseFloat", (frac << 16) | pat, u_of(a), u_of(b));
            }
            n++;
        }
    }
    for (pat = 0; pat < 4000000; pat++) {
        u32 v = pat < N_SPECIAL ? special_bits[pat] : rnd();
        u8 buf[4] = { (u8) v, (u8) (v >> 8), (u8) (v >> 16), (u8) (v >> 24) };
        u8 *pa = buf, *pb = buf;
        f32 a = parseFloat(&pa, HSD_A_FRAC_FLOAT);
        f32 b = ref_parseFloat(&pb, HSD_A_FRAC_FLOAT);
        if (!same_f(u_of(a), u_of(b)) || pa != pb) {
            fail("parseFloat float", pat, u_of(a), u_of(b));
        }
        n++;
    }
    printf("parseFloat: %llu keys (every frac byte and 16-bit pattern)\n",
           (unsigned long long) n);
}

static void test_hermite(void)
{
    u32 fterm, k;
    u64 n = 0;

    for (fterm = 1; fterm < 0x10000; fterm++) {
        for (k = 0; k < 64; k++) {
            f32 t = k < 8 ? (f32) k : rnd_below(4) ? (f32) rnd_below(fterm + 2)
                                                   : rnd_f32();
            f32 p0 = rnd_f32(), p1 = rnd_f32(), d0 = rnd_f32(), d1 = rnd_f32();
            f32 a = FObjHermite(1.0F / (f32) (u16) fterm, t, p0, p1, d0, d1);
            f32 b = ref_splGetHelmite(1.0 / (u16) fterm, t, p0, p1, d0, d1);
            if (!same_f(u_of(a), u_of(b))) {
                fail("spline", ((u64) fterm << 8) | k, u_of(a), u_of(b));
            }
            n++;
        }
    }
    printf("spline: %llu evaluations (every u16 fterm)\n",
           (unsigned long long) n);
}

/* ---- keyframe streams */

#define STREAM_MAX 4096

static void put_varint(u8* buf, u32* len, u32 v)
{
    do {
        u8 d = v & 0x7F;
        v >>= 7;
        buf[(*len)++] = d | (v ? 0x80 : 0);
    } while (v != 0 && *len < STREAM_MAX - 8);
}

static u8 rnd_frac(void)
{
    static const u8 types[] = { HSD_A_FRAC_FLOAT, HSD_A_FRAC_FLOAT,
                                HSD_A_FRAC_S16,   HSD_A_FRAC_U16,
                                HSD_A_FRAC_S8,    HSD_A_FRAC_U8 };
    u8 type = rnd_below(20) ? types[rnd_below(6)] : (u8) (0xA0 + rnd_below(3) * 0x20);
    u8 shift = rnd_below(4) ? rnd_below(16) : rnd_below(32);
    return type | shift;
}

static void put_value(u8* buf, u32* len, u8 frac)
{
    u32 n = 0, v, i;

    switch (frac & 0xE0) {
    case HSD_A_FRAC_FLOAT:
        n = frac == HSD_A_FRAC_FLOAT ? 4 : 0;
        break;
    case HSD_A_FRAC_S16:
    case HSD_A_FRAC_U16:
        n = 2;
        break;
    case HSD_A_FRAC_S8:
    case HSD_A_FRAC_U8:
        n = 1;
        break;
    }
    /* a FLOAT key with shift bits is not FRAC_FLOAT to parseFloat: it falls
     * to the fixed-point switch's default and reads nothing */
    v = n == 4 ? u_of(rnd_f32()) : rnd();
    for (i = 0; i < n && *len < STREAM_MAX - 8; i++) {
        buf[(*len)++] = (u8) (v >> (8 * i));
    }
}

static u32 gen_stream(u8* buf, u8 frac_value, u8 frac_slope)
{
    u32 len = 0;
    u32 target = 8 + rnd_below(rnd_below(4) ? 200 : STREAM_MAX - 64);

    if (rnd_below(8) == 0) {
        for (len = 0; len < target; len++) {
            buf[len] = (u8) rnd();
        }
        return len;
    }
    while (len < target) {
        u32 op = rnd_below(24) ? 1 + rnd_below(6) : rnd_below(16);
        u32 nb = rnd_below(4) ? 1 + rnd_below(8) : 1 + rnd_below(rnd_below(8) ? 40 : 3000);
        u32 k;

        buf[len++] = (u8) (op | (((nb - 1) & 7) << 4) | ((nb - 1) >= 8 ? 0x80 : 0));
        if (nb - 1 >= 8) {
            put_varint(buf, &len, (nb - 1) >> 3);
        }
        for (k = 0; k < nb && k < 64 && len < target; k++) {
            switch (op) {
            case HSD_A_OP_SPL:
                put_value(buf, &len, frac_value);
                put_value(buf, &len, frac_slope);
                break;
            case HSD_A_OP_SLP:
                put_value(buf, &len, frac_slope);
                continue; /* no wait follows a slope */
            default:
                put_value(buf, &len, frac_value);
                break;
            }
            put_varint(buf, &len,
                       rnd_below(8)    ? rnd_below(12)
                       : rnd_below(4)  ? rnd_below(400)
                                       : rnd_below(1u << 21));
        }
    }
    return len;
}

typedef struct {
    u32 n;
    u32 type[512];
    u32 val[512];
} UpdateLog;
static UpdateLog log_new, log_ref;

static void update_new(void* obj, enum_t type, HSD_ObjData* val)
{
    (void) obj;
    if (log_new.n < 512) {
        log_new.type[log_new.n] = type;
        log_new.val[log_new.n++] = u_of(val->fv);
    }
}
static void update_ref(void* obj, enum_t type, HSD_ObjData* val)
{
    (void) obj;
    if (log_ref.n < 512) {
        log_ref.type[log_ref.n] = type;
        log_ref.val[log_ref.n++] = u_of(val->fv);
    }
}

static int logs_same(void)
{
    u32 i;

    if (log_new.n != log_ref.n) {
        return 0;
    }
    for (i = 0; i < log_new.n; i++) {
        if (log_new.type[i] != log_ref.type[i] ||
            !same_f(log_new.val[i], log_ref.val[i]))
        {
            return 0;
        }
    }
    return 1;
}

static int fobj_same(HSD_FObj* a, HSD_FObj* b)
{
    return a->ad - a->ad_head == b->ad - b->ad_head &&
           a->length == b->length && a->flags == b->flags && a->op == b->op &&
           a->op_intrp == b->op_intrp && a->obj_type == b->obj_type &&
           a->nb_pack == b->nb_pack && a->startframe == b->startframe &&
           a->fterm == b->fterm && same_f(u_of(a->time), u_of(b->time)) &&
           same_f(u_of(a->p0), u_of(b->p0)) && same_f(u_of(a->p1), u_of(b->p1)) &&
           same_f(u_of(a->d0), u_of(b->d0)) && same_f(u_of(a->d1), u_of(b->d1));
}

static f32 rnd_rate(void)
{
    static const f32 rates[] = { 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 2.0f,
                                 0.0f, 0.25f, 1.5f, 3.0f, 0.1f, 0.7f };
    if (rnd_below(64) == 0) {
        return rnd_f32();
    }
    if (rnd_below(64) == 0) {
        return -1.0f;
    }
    return rates[rnd_below(sizeof(rates) / sizeof(rates[0]))];
}

static void test_streams(u32 runs)
{
    static u8 streams[3][STREAM_MAX + 64];
    u64 steps = 0, updates = 0;
    u32 run;

    for (run = 0; run < runs; run++) {
        HSD_FObj fa[3], fb[3];
        u32 n = 1 + rnd_below(3), i, frame, frames = 20 + rnd_below(400);
        f32 start;

        for (i = 0; i < n; i++) {
            u8 fv = rnd_frac(), fs = rnd_frac();
            u32 len = gen_stream(streams[i], fv, fs);

            memset(streams[i] + len, 0, 64);
            memset(&fa[i], 0, sizeof(fa[i]));
            fa[i].next = i + 1 < n ? &fa[i + 1] : NULL;
            fa[i].startframe = (s16) (rnd_below(8) ? rnd_below(10) : rnd());
            fa[i].obj_type = (u8) rnd();
            fa[i].frac_value = fv;
            fa[i].frac_slope = fs;
            fa[i].ad_head = streams[i];
            fa[i].length = rnd_below(8) ? len : rnd_below(len + 1);
            fb[i] = fa[i];
            fb[i].next = i + 1 < n ? &fb[i + 1] : NULL;
        }
        start = rnd_below(4) ? 0.0f : rnd_below(4) ? (f32) rnd_below(30) : rnd_f32();
        HSD_FObjReqAnimAll(fa, start);
        ref_HSD_FObjReqAnimAll(fb, start);

        for (frame = 0; frame < frames; frame++) {
            f32 rate = rnd_rate();
            int nul = rnd_below(16) == 0;

            log_new.n = log_ref.n = 0;
            switch (rnd_below(64)) {
            case 0: /* a looping AObj's rewind: flush, stop, request */
                start = rnd_below(2) ? 0.0f : (f32) rnd_below(40);
                HSD_FObjStopAnimAll(fa, NULL, update_new, rate);
                ref_HSD_FObjStopAnimAll(fb, NULL, update_ref, rate);
                HSD_FObjReqAnimAll(fa, start);
                ref_HSD_FObjReqAnimAll(fb, start);
                break;
            case 1:
                HSD_FObjStopAnimAll(fa, NULL, update_new, rate);
                ref_HSD_FObjStopAnimAll(fb, NULL, update_ref, rate);
                break;
            default:
                HSD_FObjInterpretAnimAll(fa, NULL, nul ? NULL : update_new, rate);
                ref_HSD_FObjInterpretAnimAll(fb, NULL, nul ? NULL : update_ref,
                                             rate);
                break;
            }
            steps++;
            updates += log_new.n;
            if (!logs_same()) {
                fail("stream updates", ((u64) run << 16) | frame, log_new.n,
                     log_ref.n);
                break;
            }
            for (i = 0; i < n; i++) {
                if (!fobj_same(&fa[i], &fb[i])) {
                    fail("stream fobj state", ((u64) run << 16) | frame,
                         u_of(fa[i].time), u_of(fb[i].time));
                    break;
                }
            }
        }
    }
    printf("keyframe streams: %u runs, %llu frames, %llu updates\n", runs,
           (unsigned long long) steps, (unsigned long long) updates);
}

/* ---- matrices */

static void fail_mtx(const char* what, u64 i, Mtx a, Mtx b)
{
    int k;

    for (k = 0; k < 12; k++) {
        if (!same_f(u_of((&a[0][0])[k]), u_of((&b[0][0])[k]))) {
            break;
        }
    }
    if (failures < 20) {
        fprintf(stderr, "  element [%d][%d]\n", k / 4, k % 4);
    }
    fail(what, i, u_of((&a[0][0])[k]), u_of((&b[0][0])[k]));
}

static int mtx_same(Mtx a, Mtx b)
{
    int k;

    for (k = 0; k < 12; k++) {
        if (!same_f(u_of((&a[0][0])[k]), u_of((&b[0][0])[k]))) {
            return 0;
        }
    }
    return 1;
}

static void rnd_mtx(Mtx m)
{
    int i, j, kind = rnd_below(4);

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++) {
            m[i][j] = kind == 0 ? rnd_f32()
                      : rnd_below(16) ? ((f32) rnd() / 4294967296.0f - 0.5f) * 4.0f
                                      : rnd_f32();
        }
    }
}

static void test_mtx(u32 runs)
{
    u32 run;

    for (run = 0; run < runs; run++) {
        Vec3 s, r, t, ps;
        Mtx a, b;
        int with_ps = rnd_below(4) != 0;

        s.x = rnd_below(4) ? 1.0f + (f32) rnd_below(100) / 64.0f : rnd_f32();
        s.y = rnd_below(4) ? s.x : rnd_f32();
        s.z = rnd_below(4) ? s.x : rnd_f32();
        r.x = rnd_below(3) ? 0.0f : rnd_f32();
        r.y = rnd_f32();
        r.z = rnd_below(3) ? rnd_f32() : -0.0f;
        t.x = rnd_f32(), t.y = rnd_f32(), t.z = rnd_f32();
        ps.x = rnd_below(4) ? 1.0f : rnd_f32();
        ps.y = rnd_below(4) ? ps.x : rnd_f32();
        ps.z = rnd_below(4) ? ps.x : rnd_f32();
        HSD_MtxSRT(a, &s, &r, &t, with_ps ? &ps : NULL);
        ref_HSD_MtxSRT(b, &s, &r, &t, with_ps ? &ps : NULL);
        if (!mtx_same(a, b)) {
            fail_mtx("HSD_MtxSRT", run, a, b);
        }
    }
    printf("HSD_MtxSRT: %u matrices\n", runs);

    for (run = 0; run < runs; run++) {
        Mtx acc_new, acc_ref, x, y, tmp;
        int n = 1 + rnd_below(4), k, i, j;

        for (i = 0; i < 3; i++) {
            for (j = 0; j < 4; j++) {
                acc_new[i][j] = acc_ref[i][j] = rnd_below(2) ? 0.0f : rnd_f32();
            }
        }
        for (k = 0; k < n; k++) {
            f32 w = rnd_below(4) ? (f32) rnd() / 4294967296.0f : rnd_f32();

            rnd_mtx(x);
            rnd_mtx(y);
            HSD_MtxConcatScaledAdd(x, y, acc_new, w);
            C_MTXConcat(x, y, tmp);
            ref_HSD_MtxScaledAdd(tmp, acc_ref, acc_ref, w);
        }
        if (!mtx_same(acc_new, acc_ref)) {
            fail_mtx("HSD_MtxConcatScaledAdd", run, acc_new, acc_ref);
        }
    }
    printf("envelope blend: %u matrices\n", runs);
}

int main(int argc, char** argv)
{
    int full = argc > 1 && strcmp(argv[1], "--full") == 0;

    test_sincos(full, 1u << 24);
    test_parse_float();
    test_hermite();
    test_streams(20000);
    test_mtx(2000000);
    if (failures) {
        printf("FAILED: %d mismatches\n", failures);
        return 1;
    }
    printf("ok: same bits as the reference\n");
    return 0;
}
