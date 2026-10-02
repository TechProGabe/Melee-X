/* rc_ref.h - nv2a_rc.h before swap tables (tests/xbox/rc_ref.c), types renamed so
 * test_rc.c can include it next to the current header. */
#ifndef RC_REF_H
#define RC_REF_H
#include <stdint.h>

#define RC_MAX_TEV 16
#define RC_MAX_STAGES 8

typedef struct {
    uint8_t cin[4], ain[4];
    uint8_t cop, aop, cbias, cscale, abias, ascale;
    uint8_t cclamp, aclamp, cout, aout;
    uint8_t kcsel, kasel;
    int8_t unit;       /* NV2A texture unit sampled by this stage, -1: none */
    uint8_t ras;       /* 0: colour channel 0, 1: channel 1, 2: zero */
    uint8_t tex_alpha_bcast, ras_alpha_bcast;   /* swap table replicates alpha */
    uint8_t pad;
} RefStage;

typedef struct {
    uint8_t nstages;
    uint8_t units_used;    /* bit u: texture unit u carries a texture */
    uint8_t v1_used;       /* colour channel 1 reaches a stage */
    uint8_t pad;
    RefStage st[RC_MAX_TEV];
} RefCfg;

#ifndef NV2A_RC_H
/* where a combiner constant comes from, resolved per draw */
enum {
    RREF_NONE = 0,
    RREF_TEVREG_RGB,   /* param: 0 PREV .. 3 REG2 */
    RREF_TEVREG_A,
    RREF_KONST_C,      /* param: kcsel */
    RREF_KONST_A,      /* param: kasel */
    RREF_FIXED,        /* param: index into nv2a.c's k_rc_fixed (rgb and a) */
};
#define RREF(t, p) ((uint16_t)(((t) << 8) | ((p) & 0xFF)))
#endif

typedef struct {
    int nstages;
    uint32_t cicw[RC_MAX_STAGES], cocw[RC_MAX_STAGES];
    uint32_t aicw[RC_MAX_STAGES], aocw[RC_MAX_STAGES];
    uint32_t cw0, cw1;
    uint16_t cref[RC_MAX_STAGES][4];   /* C0.rgb C0.a C1.rgb C1.a */
    uint16_t fref[4];
    int approximated;
} RefProg;

void rc_ref_compile(const RefCfg* cfg, RefProg* out);

#endif
