/* nv2a_vp.h - NV2A vertex programs generated from GX transform state.
 * Constant rows (hardware index) used by every generated program: */
#ifndef NV2A_VP_H
#define NV2A_VP_H
#include <stdint.h>

#define VPC_PROJ 0       /* 4 rows: projection with viewport + content rect folded in */
#define VPC_K 4          /* (0, 1, 0.5, 2) */
#define VPC_POS 6        /* 10 position matrices x 3 rows, addressed by a0 = PNMTXIDX */
#define VPC_NRM 36       /* 10 normal matrices x 3 rows, same addressing */
#define VPC_CHAN 66      /* mat0, amb0, mat1, amb1 (0..1 floats) */
#define VPC_LIGHT 70     /* 8 lights x 5 rows: pos, dir, colour, a, k */
#define VPC_TEXGEN 110   /* 4 units x 3 rows (s, t, q), post matrix folded in */
#define VPC_POSTMTX 122  /* 4 units x 3 rows: post-transform after normalize */
#define VPC_FOG 134      /* 3 rows: fog numerator, denominator, curve (nv2a_fog.h) */
#define VPC_COUNT 137

/* vertex inputs */
#define VPI_POS 0
#define VPI_MTX 1
#define VPI_NRM 2
#define VPI_COL0 3
#define VPI_COL1 4
/* GX TEX0..7 -> v9..v15, v8 */
static inline int vpi_tex(int n) { return n < 7 ? 9 + n : 8; }

/* light attenuation variants the generator knows */
enum { VPL_OFF = 0, VPL_DIFFUSE, VPL_SPOT, VPL_SPEC };

/* GX fog curves the generator writes oFog for (nv2a_fog.h) */
enum { VPF_OFF = 0, VPF_LIN, VPF_EXP, VPF_EXP2 };

typedef struct {
    uint8_t enable, amb_vtx, mat_vtx;
    uint8_t diff_fn;              /* GXDiffuseFn */
    uint8_t attn;                 /* VPL_* for every light in the mask */
    uint8_t light_mask;
} VpChan;

typedef struct {
    uint8_t src;                  /* GXTexGenSrc */
    uint8_t proj;                 /* MTX3x4: q is used */
    uint8_t normalize;            /* then post matrix */
} VpTexGen;

/* Everything the program's code depends on; constants are separate. */
typedef struct {
    uint8_t has_nrm;
    uint8_t nchans;               /* 0..2 */
    VpChan chan[4];               /* COLOR0 ALPHA0 COLOR1 ALPHA1 */
    uint8_t ntex;                 /* NV2A texture units fed, 0..4 */
    VpTexGen tex[4];
    uint8_t copy;                 /* EFB copy pass (nv2a.c): position and texcoord 0 as given */
    uint8_t fog;                  /* VPF_*: GX fog amount -> oFog.x */
    uint8_t pad;
} VpKey;

#define VP_MAX_INSNS 136

typedef struct {
    uint32_t n;                   /* instructions */
    uint32_t words[VP_MAX_INSNS * 4];
    int approximated;
} VpProgram;

void vp_generate(const VpKey* key, VpProgram* out);

#endif
