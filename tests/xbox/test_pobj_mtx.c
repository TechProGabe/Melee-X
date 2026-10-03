/* test_pobj_mtx.c - host check that the Xbox's PObj matrix setup
 * (pobj.c's PObjSetupMtx with the per-DObj envelope memo computed in
 * place, the fused blend and the prefetch hints) and the SSE
 * HSD_MtxInverseTranspose (mtx.c) give what the upstream code gives
 * (tests/xbox/pobj_mtx_ref.c), bit for bit:
 *   - HSD_MtxInverseTranspose on random matrices (specials, denormals,
 *     singular ones), in place and not;
 *   - random DObjs drawn the way HSD_DObjDisp draws them: memo reset, then
 *     PObjs of every kind (rigid, shared-vertex, shape-anim, envelope lists
 *     of up to 12 entries whose envelopes repeat across PObjs as separate
 *     objects, 1-5 joints, weights at and around 1), with and without
 *     lighting, reflection and highlight texgens, a model node matrix,
 *     dirty joints and rendermode 0x4000000. Every GX matrix load (id and
 *     the 12 words), GXSetCurrentMtx, envelope-blend count and joint
 *     update is logged; both logs, the matrix marks, the joints and the
 *     perf counters must agree.
 * Built and run by tools/xbox/test_pobj_mtx.py. */
#include <stdio.h>
#include <stdlib.h>

#include "pobj_mtx_ref.c"

/* ---- what pobj.c and mtx.c link against */

void __assert(const char* file, u32 line, const char* cond)
{
    fprintf(stderr, "assert %s:%u: %s\n", file, (unsigned) line, cond);
    abort();
}
uintptr_t OSBaseAddress;
void* pc_resolve_ext_ptr(uint32_t id)
{
    (void) id;
    return NULL;
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
HSD_PerfStat HSD_PerfCurrentStat;

/* the log both versions write */
enum { EV_CUR, EV_POS, EV_NRM, EV_TEX, EV_BLEND, EV_JOINT };
typedef struct {
    int kind;
    u32 id;
    u32 w[12];
} LogEvent;
#define LOG_MAX 4096
static LogEvent log_buf[LOG_MAX];
static int log_n;

static LogEvent* ev(int kind, u32 id)
{
    LogEvent* e;

    if (log_n == LOG_MAX) {
        fprintf(stderr, "log full\n");
        abort();
    }
    e = &log_buf[log_n++];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->id = id;
    return e;
}
static void ev_mtx(int kind, const void* mtx, u32 id)
{
    memcpy(ev(kind, id)->w, mtx, 48);
}

void GXSetCurrentMtx(u32 id)
{
    ev(EV_CUR, id);
}
void GXLoadPosMtxImm(const void* mtx, u32 id)
{
    ev_mtx(EV_POS, mtx, id);
}
void GXLoadNrmMtxImm(const void* mtx, u32 id)
{
    ev_mtx(EV_NRM, mtx, id);
}
void GXLoadTexMtxImm(const void* mtx, u32 id, GXTexMtxType type)
{
    ev_mtx(EV_TEX, mtx, id * 16 + type);
}
void HSD_PerfCountEnvelopeBlending(s32 n)
{
    ev(EV_BLEND, (u32) n);
}
s32 HSD_Index2PosNrmMtx(u32 i)
{
    return GX_PNMTX0 + 3 * (s32) i;
}
GXTexMtx HSD_Index2TexMtx(u32 i)
{
    return (GXTexMtx) (GX_TEXMTX0 + 3 * i);
}

/* the scene */
#define N_JOINTS 24
static HSD_JObj* joints[N_JOINTS];
static Mtx joint_env[N_JOINTS];
static HSD_JObj* cur_jobj;
static int cur_reflection, cur_highlight;
static Mtx node_mtx;
static int cur_node;

HSD_JObj* HSD_JObjGetCurrent(void)
{
    return cur_jobj;
}
/* a dirty joint's matrix "recomputed": changed, deterministically */
void HSD_JObjSetupMatrixSub(HSD_JObj* jobj)
{
    int i;

    for (i = 0; i < N_JOINTS && joints[i] != jobj; i++) {
    }
    ev(EV_JOINT, (u32) i);
    jobj->flags &= ~JOBJ_MTX_DIRTY;
    jobj->mtx[0][3] += 1.0f;
    jobj->mtx[1][1] *= 0.5f;
}
HSD_TObj* _HSD_TObjGetCurrentByType(HSD_TObj* from, u32 mapping)
{
    static HSD_TObj dummy;

    (void) from;
    if (mapping == TEX_COORD_REFLECTION) {
        return cur_reflection ? &dummy : NULL;
    }
    if (mapping == TEX_COORD_HILIGHT) {
        return cur_highlight ? &dummy : NULL;
    }
    return NULL;
}
MtxPtr _HSD_mkEnvelopeModelNodeMtx(HSD_JObj* m, MtxPtr mtx)
{
    (void) m;
    if (!cur_node) {
        return NULL;
    }
    memcpy(mtx, node_mtx, sizeof(Mtx));
    return mtx;
}

/* ---- random numbers and floats (as test_anim_mtx.c) */

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
    0x7FBFFFFF, 0x2EDBE6FF, 0x2EDBE6FE, 0x34000000, 0x3F7FFFFF, 0x3F7FFFFE,
    0x3F800001, 0x1E3CE508,
};
#define N_SPECIAL (sizeof(special_bits) / sizeof(special_bits[0]))

static f32 rnd_f32(void)
{
    switch (rnd_below(6)) {
    case 0:
        return f_of(special_bits[rnd_below(N_SPECIAL)] ^ (rnd() & 0x80000000));
    case 1:
        return f_of(special_bits[rnd_below(N_SPECIAL)] + rnd_below(9) - 4);
    case 2:
        return f_of(rnd());
    case 3:
        return f_of((rnd() & 0x807FFFFF) | ((0x60 + rnd_below(0x40)) << 23));
    default:
        return ((f32) rnd() / 4294967296.0f - 0.5f) * 4.0f;
    }
}

/* mostly ordinary transforms; now and then specials, near-singular or
 * singular ones (a zero or repeated row) */
static void rnd_mtx(Mtx m)
{
    int i, j, kind = rnd_below(8);

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++) {
            m[i][j] = kind == 0 ? rnd_f32()
                      : rnd_below(24) ? ((f32) rnd() / 4294967296.0f - 0.5f) * 4.0f
                                      : rnd_f32();
        }
    }
    if (kind == 1) {
        i = rnd_below(3);
        for (j = 0; j < 4; j++) {
            m[i][j] = rnd_below(2) ? 0.0f : m[(i + 1) % 3][j];
        }
    } else if (kind == 2) {
        f32 s = f_of((u32) (0x10 + rnd_below(0x30)) << 23);
        for (i = 0; i < 3; i++) {
            for (j = 0; j < 3; j++) {
                m[i][j] *= s;
            }
        }
    }
}

/* Same bits, or both NaN (see test_anim_mtx.c) */
static int same_w(u32 a, u32 b)
{
    return a == b || ((a & 0x7FFFFFFF) > 0x7F800000 && (b & 0x7FFFFFFF) > 0x7F800000);
}

static int failures;

/* ---- HSD_MtxInverseTranspose */

static void test_inverse_transpose(u32 runs)
{
    u32 run;

    for (run = 0; run < runs; run++) {
        Mtx src, a, b;
        int k, in_place = rnd_below(4) == 0;

        rnd_mtx(src);
        for (k = 0; k < 12; k++) {
            (&a[0][0])[k] = (&b[0][0])[k] = rnd_f32();
        }
        if (in_place) {
            memcpy(a, src, sizeof a);
            memcpy(b, src, sizeof b);
            HSD_MtxInverseTranspose(a, a);
            ref_HSD_MtxInverseTranspose(b, b);
        } else {
            HSD_MtxInverseTranspose(src, a);
            ref_HSD_MtxInverseTranspose(src, b);
        }
        for (k = 0; k < 12; k++) {
            u32 x = u_of((&a[0][0])[k]), y = u_of((&b[0][0])[k]);
            if (!same_w(x, y)) {
                if (failures++ < 20) {
                    fprintf(stderr,
                            "FAIL HSD_MtxInverseTranspose #%u%s [%d][%d]: "
                            "%08X, reference %08X\n",
                            run, in_place ? " in place" : "", k / 4, k % 4,
                            (unsigned) x, (unsigned) y);
                }
                break;
            }
        }
    }
    printf("HSD_MtxInverseTranspose: %u matrices\n", runs);
}

/* ---- PObjSetupMtx */

#define MAX_POBJS 10
#define MAX_SPECS 8
typedef struct {
    int n;
    int joint[5];
    f32 weight[5];
} EnvSpec;

static HSD_Envelope* make_envelope(const EnvSpec* s)
{
    HSD_Envelope *head = NULL, **p = &head;
    int i;

    for (i = 0; i < s->n; i++) {
        *p = calloc(1, sizeof(HSD_Envelope));
        (*p)->jobj = joints[s->joint[i]];
        (*p)->weight = s->weight[i];
        p = &(*p)->next;
    }
    return head;
}

static void rnd_spec(EnvSpec* s)
{
    int i;
    u32 r = rnd_below(16);

    s->n = r < 6 ? 1 : r < 11 ? 2 : r < 14 ? 3 : r < 15 ? 4 : 5;
    for (i = 0; i < s->n; i++) {
        s->joint[i] = rnd_below(N_JOINTS);
        s->weight[i] = rnd_below(8) ? (f32) rnd() / 4294967296.0f : rnd_f32();
    }
    if (s->n == 1 || rnd_below(16) == 0) {
        /* at the weight-1 test: 1, 1 - 2^-24, 1 - 2^-23, 1 - 2^-22, more */
        static const u32 w1[] = { 0x3F800000, 0x3F7FFFFF, 0x3F7FFFFE,
                                  0x3F7FFFFC, 0x3F800001, 0x40000000 };
        s->weight[0] = rnd_below(4) ? 1.0f : f_of(w1[rnd_below(6)]);
    }
}

typedef struct {
    Mtx mtx;
    u32 flags;
} JointState;

static void save_joints(JointState* s)
{
    int i;

    for (i = 0; i < N_JOINTS; i++) {
        memcpy(s[i].mtx, joints[i]->mtx, sizeof(Mtx));
        s[i].flags = joints[i]->flags;
    }
}
static void load_joints(const JointState* s)
{
    int i;

    for (i = 0; i < N_JOINTS; i++) {
        memcpy(joints[i]->mtx, s[i].mtx, sizeof(Mtx));
        joints[i]->flags = s[i].flags;
    }
}

static LogEvent log_new[LOG_MAX];
static int log_new_n;

static int logs_same(u32 run)
{
    int i, k;

    if (log_new_n != log_n) {
        if (failures++ < 20) {
            fprintf(stderr, "FAIL DObj #%u: %d events, reference %d\n", run,
                    log_new_n, log_n);
        }
        return 0;
    }
    for (i = 0; i < log_n; i++) {
        const LogEvent *a = &log_new[i], *b = &log_buf[i];
        if (a->kind != b->kind || a->id != b->id) {
            if (failures++ < 20) {
                fprintf(stderr,
                        "FAIL DObj #%u event %d: kind %d id %u, reference "
                        "kind %d id %u\n",
                        run, i, a->kind, a->id, b->kind, b->id);
            }
            return 0;
        }
        for (k = 0; k < 12; k++) {
            /* a normal matrix's fourth column is never read (GX takes
             * 3x3), but it is 0 in both */
            if (!same_w(a->w[k], b->w[k])) {
                if (failures++ < 20) {
                    fprintf(stderr,
                            "FAIL DObj #%u event %d (kind %d id %u) [%d][%d]: "
                            "%08X, reference %08X\n",
                            run, i, a->kind, a->id, k / 4, k % 4,
                            (unsigned) a->w[k], (unsigned) b->w[k]);
                }
                return 0;
            }
        }
    }
    return 1;
}

static void test_pobjs(u32 runs)
{
    static Mtx vmtx_pool[3];
    u64 events = 0, loads = 0;
    u32 run;
    int i, j;

    for (i = 0; i < N_JOINTS; i++) {
        joints[i] = calloc(1, sizeof(HSD_JObj));
        joints[i]->envelopemtx = joint_env[i];
    }

    for (run = 0; run < runs; run++) {
        HSD_PObj pobjs[MAX_POBJS];
        EnvSpec specs[MAX_SPECS];
        JointState before[N_JOINTS], after_new[N_JOINTS];
        Mtx pmtx;
        MtxPtr vmtx;
        u32 rendermode, mtx_load_new;
        int n_pobjs = 1 + rnd_below(MAX_POBJS), n_specs = 1 + rnd_below(MAX_SPECS);
        void* mark_obj[2];
        u32 mark[2];

        /* joints: refreshed now and then, a few dirty */
        for (i = 0; i < N_JOINTS; i++) {
            if (run == 0 || rnd_below(4) == 0) {
                rnd_mtx(joints[i]->mtx);
                rnd_mtx(joint_env[i]);
            }
            joints[i]->flags = (rnd_below(2) ? JOBJ_LIGHTING : 0) |
                               (rnd_below(10) == 0 ? JOBJ_MTX_DIRTY : 0) |
                               (rnd_below(40) == 0 ? JOBJ_USER_DEF_MTX : 0);
        }
        for (i = 0; i < 3; i++) {
            if (run == 0 || rnd_below(8) == 0) {
                rnd_mtx(vmtx_pool[i]);
            }
        }
        if (rnd_below(16) == 0) {
            memcpy(vmtx_pool[1], vmtx_pool[0], sizeof(Mtx));   /* equal, not same */
        }
        vmtx = vmtx_pool[rnd_below(3)];
        rnd_mtx(pmtx);
        cur_jobj = joints[rnd_below(N_JOINTS)];
        cur_reflection = rnd_below(5) == 0;
        cur_highlight = rnd_below(5) == 0;
        cur_node = rnd_below(6) == 0;
        rnd_mtx(node_mtx);
        rendermode = rnd_below(8) == 0 ? 0x4000000 : 0;
        if (run == 0 || rnd_below(3) == 0) {
            HSD_PObjClearMtxMark(rnd_below(2) ? cur_jobj : NULL,
                                 rnd_below(2) ? HSD_MTX_RIGID : HSD_MTX_ENVELOPE);
        }

        for (i = 0; i < n_specs; i++) {
            rnd_spec(&specs[i]);
        }
        memset(pobjs, 0, sizeof pobjs);
        for (i = 0; i < n_pobjs; i++) {
            u32 r = rnd_below(20);
            HSD_PObj* p = &pobjs[i];

            if (r < 14) {
                HSD_SList *head = NULL, **lp = &head;
                int n_list = 1 + rnd_below(rnd_below(8) ? 6 : 12);

                p->flags = POBJ_ENVELOPE;
                for (j = 0; j < n_list; j++) {
                    *lp = calloc(1, sizeof(HSD_SList));
                    (*lp)->data = make_envelope(&specs[rnd_below(n_specs)]);
                    lp = &(*lp)->next;
                }
                p->u.envelope_list = head;
            } else if (r < 17) {
                p->flags = POBJ_SKIN;
            } else if (r < 19) {
                p->flags = POBJ_SKIN;
                p->u.jobj = joints[rnd_below(N_JOINTS)];
            } else {
                p->flags = POBJ_SHAPEANIM;
            }
        }

        /* the code under test, as HSD_DObjDisp calls it */
        save_joints(before);
        HSD_PObjGetMtxMark(0, &mark_obj[0], &mark[0]);
        HSD_PObjGetMtxMark(1, &mark_obj[1], &mark[1]);
        HSD_PerfCurrentStat.nb_mtx_load = 0;
        log_n = 0;
        HSD_PObjEnvelopeMemoReset();
        for (i = 0; i < n_pobjs; i++) {
            PObjSetupMtx(&pobjs[i], vmtx, pmtx, rendermode);
        }
        memcpy(log_new, log_buf, sizeof(LogEvent) * log_n);
        log_new_n = log_n;
        mtx_load_new = HSD_PerfCurrentStat.nb_mtx_load;
        save_joints(after_new);
        {
            void* o0;
            void* o1;
            u32 m0, m1;
            HSD_PObjGetMtxMark(0, &o0, &m0);
            HSD_PObjGetMtxMark(1, &o1, &m1);

            /* the reference, from the same state */
            load_joints(before);
            HSD_PObjSetMtxMark(0, mark_obj[0], mark[0]);
            HSD_PObjSetMtxMark(1, mark_obj[1], mark[1]);
            HSD_PerfCurrentStat.nb_mtx_load = 0;
            log_n = 0;
            for (i = 0; i < n_pobjs; i++) {
                ref_PObjSetupMtx(&pobjs[i], vmtx, pmtx, rendermode);
            }
            HSD_PObjGetMtxMark(0, &mark_obj[0], &mark[0]);
            HSD_PObjGetMtxMark(1, &mark_obj[1], &mark[1]);
            if (o0 != mark_obj[0] || o1 != mark_obj[1] || m0 != mark[0] ||
                m1 != mark[1])
            {
                if (failures++ < 20) {
                    fprintf(stderr, "FAIL DObj #%u: matrix marks differ\n", run);
                }
            }
        }
        if (mtx_load_new != HSD_PerfCurrentStat.nb_mtx_load) {
            if (failures++ < 20) {
                fprintf(stderr, "FAIL DObj #%u: %u matrix loads counted, reference %u\n",
                        run, mtx_load_new, HSD_PerfCurrentStat.nb_mtx_load);
            }
        }
        for (i = 0; i < N_JOINTS; i++) {
            if (memcmp(after_new[i].mtx, joints[i]->mtx, sizeof(Mtx)) != 0 ||
                after_new[i].flags != joints[i]->flags)
            {
                if (failures++ < 20) {
                    fprintf(stderr, "FAIL DObj #%u: joint %d differs\n", run, i);
                }
                break;
            }
        }
        logs_same(run);
        events += log_n;
        loads += HSD_PerfCurrentStat.nb_mtx_load;

        for (i = 0; i < n_pobjs; i++) {
            if (pobj_type((&pobjs[i])) == POBJ_ENVELOPE) {
                HSD_SList* l = pobjs[i].u.envelope_list;
                while (l != NULL) {
                    HSD_SList* next = l->next;
                    HSD_Envelope* e = l->data;
                    while (e != NULL) {
                        HSD_Envelope* en = e->next;
                        free(e);
                        e = en;
                    }
                    free(l);
                    l = next;
                }
            }
        }
    }
    printf("PObjSetupMtx: %u DObjs, %llu events, %llu matrix loads\n", runs,
           (unsigned long long) events, (unsigned long long) loads);
}

int main(void)
{
    test_inverse_transpose(4000000);
    test_pobjs(300000);
    if (failures) {
        printf("FAILED: %d mismatches\n", failures);
        return 1;
    }
    printf("ok: same bits as the reference\n");
    return 0;
}
