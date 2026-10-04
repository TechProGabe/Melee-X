/* dl_cull_ref.c - gx_dl_culled's box test (xbox/src/sdk/gx/gx_vtx.c) before
 * the clip planes were kept per projection (gx_cull.h): the planes made
 * again for every box, from the projection global, as it was. Reference for
 * tests/xbox/test_dl_cull.c. */
#include <stdint.h>

float ref_proj[4][4];   /* g_xgx.proj */

/* the planes as the loop below makes them, for the test to compare */
void ref_planes(float out[5][4]) {
    static const float sx[5] = { 1, -1, 0, 0, 0 }, sy[5] = { 0, 0, 1, -1, 0 };
    int i, k;
    for (i = 0; i < 5; i++)
        for (k = 0; k < 4; k++) out[i][k] = ref_proj[3][k] + sx[i] * ref_proj[0][k] + sy[i] * ref_proj[1][k];
}

int ref_culled(const float bmin[3], const float bmax[3], const float mtx[3][4]) {
    int i, k;
    {
        float c[3], h[3], v[3], hv[3];
        static const float sx[5] = { 1, -1, 0, 0, 0 }, sy[5] = { 0, 0, 1, -1, 0 };
        for (k = 0; k < 3; k++) {
            c[k] = (bmin[k] + bmax[k]) * 0.5f;
            h[k] = (bmax[k] - bmin[k]) * 0.5f;
        }
        for (k = 0; k < 3; k++) {
            v[k] = mtx[k][0] * c[0] + mtx[k][1] * c[1] + mtx[k][2] * c[2] + mtx[k][3];
            hv[k] = __builtin_fabsf(mtx[k][0]) * h[0] + __builtin_fabsf(mtx[k][1]) * h[1] + __builtin_fabsf(mtx[k][2]) * h[2];
        }
        for (i = 0; i < 5; i++) {
            float n[4], d, r;
            for (k = 0; k < 4; k++) n[k] = ref_proj[3][k] + sx[i] * ref_proj[0][k] + sy[i] * ref_proj[1][k];
            d = n[0] * v[0] + n[1] * v[1] + n[2] * v[2] + n[3];
            r = __builtin_fabsf(n[0]) * hv[0] + __builtin_fabsf(n[1]) * hv[1] + __builtin_fabsf(n[2]) * hv[2];
            if (i < 4 ? d + r < 0 : d + r <= 0) return 1;
        }
    }
    return 0;
}
