#ifndef _mtx_h_
#define _mtx_h_

#include <Runtime/platform.h>

#include <math.h>

#include <dolphin/mtx.h>
#include <sysdolphin/baselib/objalloc.h>

#define VEC2_SQ_LEN(v) ((SQ((v).x) + SQ((v).y)))
#define VEC3_SQ_LEN(v) ((SQ((v).x) + SQ((v).y) + SQ((v).z)))

typedef Vec3 VecMtx[4];
typedef Vec3* VecMtxPtr;

void HSD_MtxInverse(Mtx src, Mtx dest);
void HSD_MtxInverseConcat(Mtx inv, Mtx src, Mtx dest);
void HSD_MtxInverseTranspose(Mtx src, Mtx dest);
void HSD_MtxGetRotation(Mtx m, Vec3* vec);
void HSD_MtxGetTranslate(Mtx mat, Vec3* vec);
void HSD_MtxGetScale(Mtx arg0, Vec3* arg1);
void HSD_MkRotationMtx(Mtx arg0, Vec3* arg1);
void HSD_MtxQuat(Mtx arg0, Quaternion* arg1);
void HSD_MtxSRT(Mtx m, Vec3* vec1, Vec3* vec2, Vec3* vec3, Vec3* vec4);
void HSD_MtxSRTQuat(Mtx arg0, Vec3* arg1, Quaternion* arg2, Vec3* arg3,
                    Vec3* arg4);
void HSD_MtxScaledAdd(Mtx arg0, Mtx arg1, Mtx arg2, f32 arg3);
void* HSD_VecAlloc(void);
void HSD_VecFree(void* arg0);
void* HSD_MtxAlloc(void);
void HSD_MtxFree(void* arg0);
HSD_ObjAllocData* HSD_VecGetAllocData(void);
void HSD_VecInitAllocData(void);
HSD_ObjAllocData* HSD_MtxGetAllocData(void);
void HSD_MtxInitAllocData(void);

/* PORT: acc += w * (a . b), the envelope-blend step of PObjSetupMtx and
 * ftParts_PObjSetupMtx, which called PSMTXConcat(a, b, tmp) and then
 * HSD_MtxScaledAdd(tmp, acc, acc, w). In SSE on the Xbox, one row of four at
 * a time: each lane does what C_MTXConcat's SSE lanes do (extern/aurora's
 * mtx.c), then acc + (w * that) as HSD_MtxScaledAdd does, so the bits are
 * the same; it saves the two calls and the round trip through tmp.
 * tests/xbox/test_anim_mtx.c compares it with the two calls. */
#if defined(TARGET_XBOX) && defined(__SSE__)
typedef f32 HSD_MtxRow __attribute__((vector_size(16), aligned(4)));

static inline void HSD_MtxConcatScaledAdd(Mtx a, Mtx b, Mtx acc, f32 w)
{
    const HSD_MtxRow b0 = *(const HSD_MtxRow*) b[0];
    const HSD_MtxRow b1 = *(const HSD_MtxRow*) b[1];
    const HSD_MtxRow b2 = *(const HSD_MtxRow*) b[2];
    const HSD_MtxRow wv = { w, w, w, w };
    int i;

    for (i = 0; i < 3; i++) {
        const HSD_MtxRow x = { a[i][0], a[i][0], a[i][0], a[i][0] };
        const HSD_MtxRow y = { a[i][1], a[i][1], a[i][1], a[i][1] };
        const HSD_MtxRow z = { a[i][2], a[i][2], a[i][2], a[i][2] };
        const HSD_MtxRow t = { 0.0f, 0.0f, 0.0f, a[i][3] };
        const HSD_MtxRow r = (z * b2 + (x * b0 + y * b1)) + t;
        *(HSD_MtxRow*) acc[i] = *(HSD_MtxRow*) acc[i] + wv * r;
    }
}
#else
static inline void HSD_MtxConcatScaledAdd(Mtx a, Mtx b, Mtx acc, f32 w)
{
    Mtx tmp;

    PSMTXConcat(a, b, tmp);
    HSD_MtxScaledAdd(tmp, acc, acc, w);
}
#endif

static inline f32 fabsf_bitwise(f32 v)
{
    *(u32*) &v &= ~0x80000000;
    return v;
}

static inline void HSD_MtxColVec(MtxPtr mtx, int col, Vec3* vec)
{
    vec->x = mtx[0][col];
    vec->y = mtx[1][col];
    vec->z = mtx[2][col];
}

static inline void HSD_MtxSetColVec(MtxPtr mtx, int col, Vec3* vec)
{
    mtx[0][col] = vec->x;
    mtx[1][col] = vec->y;
    mtx[2][col] = vec->z;
}

static inline f32 HSD_MtxColMag(MtxPtr mtx, int col)
{
    return sqrtf((mtx[0][col] * mtx[0][col]) + (mtx[1][col] * mtx[1][col]) +
                 (mtx[2][col] * mtx[2][col]));
}

#endif
