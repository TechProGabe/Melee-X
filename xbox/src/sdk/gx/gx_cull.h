/* gx_cull.h - gx_dl_culled's box test (gx_vtx.c), apart so that
 * tests/xbox/test_dl_cull.c can check it against the version before the
 * planes were kept (tests/xbox/dl_cull_ref.c).
 *
 * The clip planes x >= -w, x <= w, y >= -w, y <= w and w > 0 are row
 * combinations of the projection. They were made again for every list
 * tested (~40 multiplies and adds, a third of the function's instructions);
 * now once per projection: GXInit and GXSetProjection, the only writers of
 * g_xgx.proj, count its changes (gx_proj_gen). The same expressions, so the
 * same floats and the same answers. */
#ifndef XSDK_GX_CULL_H
#define XSDK_GX_CULL_H
#include <stdint.h>

typedef struct {
    float n[5][4];              /* the planes, n.(x y z 1) >= 0 inside */
    float an[5][3];             /* |n| of their x y z */
    uint32_t gen;               /* the projection's gx_proj_gen they were made of */
    int valid;
} GxCullPlanes;

/* the planes of `proj`, made again only when its generation changed */
static inline const GxCullPlanes* gx_cull_planes(GxCullPlanes* p, const float proj[4][4], uint32_t gen) {
    static const float sx[5] = { 1, -1, 0, 0, 0 }, sy[5] = { 0, 0, 1, -1, 0 };
    int i, k;
    if (p->valid && p->gen == gen) return p;
    for (i = 0; i < 5; i++) {
        for (k = 0; k < 4; k++) p->n[i][k] = proj[3][k] + sx[i] * proj[0][k] + sy[i] * proj[1][k];
        for (k = 0; k < 3; k++) p->an[i][k] = __builtin_fabsf(p->n[i][k]);
    }
    p->gen = gen;
    p->valid = 1;
    return p;
}

/* 1 when the box bmin..bmax, moved by mtx (model to view), lies wholly
 * outside one of the planes: its view-space centre and the half extents of
 * the view-aligned box around it, tested at the support point,
 * n.c + |n|.h < 0 (or <= 0 for w) */
static inline int gx_cull_box(const GxCullPlanes* p, const float bmin[3], const float bmax[3], const float mtx[3][4]) {
    float c[3], h[3], v[3], hv[3];
    int i, k;
    for (k = 0; k < 3; k++) {
        c[k] = (bmin[k] + bmax[k]) * 0.5f;
        h[k] = (bmax[k] - bmin[k]) * 0.5f;
    }
    for (k = 0; k < 3; k++) {
        v[k] = mtx[k][0] * c[0] + mtx[k][1] * c[1] + mtx[k][2] * c[2] + mtx[k][3];
        hv[k] = __builtin_fabsf(mtx[k][0]) * h[0] + __builtin_fabsf(mtx[k][1]) * h[1] + __builtin_fabsf(mtx[k][2]) * h[2];
    }
    for (i = 0; i < 5; i++) {
        const float *n = p->n[i], *an = p->an[i];
        float d = n[0] * v[0] + n[1] * v[1] + n[2] * v[2] + n[3];
        float r = an[0] * hv[0] + an[1] * hv[1] + an[2] * hv[2];
        if (i < 4 ? d + r < 0 : d + r <= 0) return 1;
    }
    return 0;
}

#endif
